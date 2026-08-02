#include "render/shaper/run_splitter.h"

#include <hb.h>

#include <array>
#include <utility>

namespace krait::render {
namespace {

// The codepoints behind one cell. A literal single-codepoint cell has no pool
// entry, so `literal` backs the returned span in that case and must outlive it.
// Returns an empty span for a cell that carries nothing shapeable.
std::span<const char32_t> cellText(char32_t ch, const core::vt::ClusterPool& pool,
                                   std::array<char32_t, 1>& literal) {
    if (!core::vt::isClusterRef(ch)) {
        literal[0] = ch;
        return literal;
    }
    // A tagged ch whose index is gone (a ref that outlived its pool) must NOT
    // fall through to the literal path: `ch` still has the tag bit set, and
    // handing 0x4000'0000 to HarfBuzz as a codepoint is not a real character.
    return pool.lookup(ch);
}

// The script that decides a cluster's run. Combining marks are Inherited, so a
// Thai syllable's script comes from its base consonant — hence "first STRONG".
ScriptTag clusterScript(std::span<const char32_t> cps) {
    for (const char32_t cp : cps) {
        const ScriptTag script = scriptOf(cp);
        if (!isWeakScript(script)) {
            return script;
        }
    }
    return static_cast<ScriptTag>(HB_SCRIPT_COMMON);
}

}  // namespace

ScriptTag scriptOf(char32_t cp) {
    // hb_unicode_funcs_get_default() returns a process-wide singleton. HarfBuzz
    // does NOT document it as safe for concurrent use, and we do not rely on it
    // being so: run splitting happens on the thread that owns the grid, and only
    // shaping is handed to the worker pool.
    return static_cast<ScriptTag>(
        hb_unicode_script(hb_unicode_funcs_get_default(), static_cast<hb_codepoint_t>(cp)));
}

bool isWeakScript(ScriptTag script) {
    const auto s = static_cast<hb_script_t>(script);
    return s == HB_SCRIPT_COMMON || s == HB_SCRIPT_INHERITED || s == HB_SCRIPT_UNKNOWN ||
           s == HB_SCRIPT_INVALID;
}

void splitRow(std::span<const core::vt::Cell> cells, const core::vt::ClusterPool& pool, int row,
              std::vector<Run>& out) {
    const auto cols = static_cast<int>(cells.size());
    Run cur;
    bool open = false;

    const auto flush = [&] {
        if (open) {
            cur.rightToLeft = hb_script_get_horizontal_direction(
                                  static_cast<hb_script_t>(cur.script)) == HB_DIRECTION_RTL;
            out.push_back(std::move(cur));
            cur = Run{};
            open = false;
        }
    };

    for (int col = 0; col < cols; ++col) {
        const core::vt::Cell& cell = cells[static_cast<std::size_t>(col)];

        if (cell.ch == 0) {
            flush();  // unwritten: a gap, not a space
            continue;
        }
        if (core::vt::isWideTrailing(cell.ch)) {
            continue;  // owned by the cluster to the left, which counted it
        }

        std::array<char32_t, 1> literal{};
        std::span<const char32_t> cps = cellText(cell.ch, pool, literal);
        // ClusterRef::len is a uint8_t. Grid caps a cluster at kMaxClusterLen
        // (16), but splitRow is handed a ClusterPool, not a Grid, and the pool
        // itself imposes no length limit — so clamp here rather than rely on an
        // invariant owned by another module. A 256-codepoint cluster would
        // otherwise wrap len to 0 and silently drop the cell's text.
        if (cps.size() > 255) {
            cps = cps.first(255);
        }
        if (cps.empty()) {
            flush();  // nothing shapeable here; do not bridge across it
            continue;
        }

        const std::uint16_t bits = shapingBits(cell.attr);
        const ScriptTag script = clusterScript(cps);
        const auto scale = static_cast<std::uint8_t>(cell.attr.scale());

        if (open) {
            const bool attrBreak = bits != cur.shaping;
            const bool scriptBreak =
                !isWeakScript(script) && !isWeakScript(cur.script) && script != cur.script;
            // A scale change ends the run for the same reason a bold change
            // does: pxHeight is part of a face's identity (shaper.h), so the
            // two sides shape against different faces and their glyph ids are
            // not comparable. Without this, OSC 66 text merged into the
            // surrounding line and every character of it drew at 1x.
            const bool scaleBreak = scale != cur.scale;
            if (attrBreak || scriptBreak || scaleBreak) {
                flush();
            }
        }
        if (!open) {
            cur.row = row;
            cur.col = col;
            cur.shaping = bits;
            cur.script = script;
            cur.scale = scale;
            open = true;
        } else if (isWeakScript(cur.script) && !isWeakScript(script)) {
            // Leading digits or spaces joined a run with no script yet; the
            // first strong cluster names it for the whole run.
            cur.script = script;
        }

        // A wide cluster owns the cell to its right, marked kWideTrailing by the
        // grid. Read that rather than re-measuring the width: the cell storage
        // is the single source of truth (the rule reflow follows too).
        const bool wide =
            col + 1 < cols && core::vt::isWideTrailing(cells[static_cast<std::size_t>(col) + 1].ch);

        cur.clusters.push_back(ClusterRef{
            .col = col,
            .cells = static_cast<std::uint8_t>(wide ? 2 : 1),
            .len = static_cast<std::uint8_t>(cps.size()),
            .offset = static_cast<std::uint32_t>(cur.text.size()),
        });
        cur.text.append(cps.data(), cps.size());
    }

    flush();
}

}  // namespace krait::render

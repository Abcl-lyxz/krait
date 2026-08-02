#include "render/frame_builder.h"

#include <algorithm>
#include <utility>

namespace krait::render {
namespace {

// 26.6 fixed point (what the shaper reports) to pixels.
constexpr float kFixedScale = 1.0F / 64.0F;

// Thickness of an underline / strikethrough / bar cursor, as a fraction of the
// cell height, floored at one pixel so it never vanishes at small sizes.
int decorationThickness(int cellHeight) {
    return std::max(1, cellHeight / 12);
}

}  // namespace

void unpackColor(std::uint32_t rgb, float& r, float& g, float& b) {
    r = static_cast<float>((rgb >> 16) & 0xFFU) / 255.0F;
    g = static_cast<float>((rgb >> 8) & 0xFFU) / 255.0F;
    b = static_cast<float>(rgb & 0xFFU) / 255.0F;
}

std::uint32_t paletteColor(std::uint8_t index, const Theme& theme) {
    if (index < 16) {
        return theme.ansi[index];
    }
    if (index < 232) {
        // The 6x6x6 cube. xterm's levels are not evenly spaced — 0, then
        // 95 + 40*n — which is why this is a table and not index * 51.
        static constexpr std::array<std::uint32_t, 6> kLevels{0, 95, 135, 175, 215, 255};
        const int cube = index - 16;
        const std::uint32_t r = kLevels[static_cast<std::size_t>(cube / 36)];
        const std::uint32_t g = kLevels[static_cast<std::size_t>((cube / 6) % 6)];
        const std::uint32_t b = kLevels[static_cast<std::size_t>(cube % 6)];
        return (r << 16) | (g << 8) | b;
    }
    // The 24-step greyscale ramp, 8 to 238.
    const std::uint32_t level = 8 + (static_cast<std::uint32_t>(index - 232) * 10);
    return (level << 16) | (level << 8) | level;
}

void resolveColors(const core::vt::Attr& attr, const Theme& theme, std::uint32_t& fg,
                   std::uint32_t& bg) {
    const auto resolve = [&theme](const core::vt::Color& color, std::uint32_t fallback) {
        switch (color.kind()) {
        case core::vt::Color::Kind::Indexed:
            return paletteColor(color.index(), theme);
        case core::vt::Color::Kind::Rgb:
            return color.rgb();
        case core::vt::Color::Kind::Default:
            break;
        }
        return fallback;
    };

    fg = resolve(attr.fg, theme.fg);
    bg = resolve(attr.bg, theme.bg);

    // Bold brightens an INDEXED colour from the low half to the high half. It
    // must not touch a truecolor or default fg: an application that asked for an
    // exact RGB and got a different one would have no way to opt out.
    if ((attr.flags & core::vt::Attr::kBold) != 0 &&
        attr.fg.kind() == core::vt::Color::Kind::Indexed && attr.fg.index() < 8) {
        fg = paletteColor(static_cast<std::uint8_t>(attr.fg.index() + 8), theme);
    }
    if ((attr.flags & core::vt::Attr::kDim) != 0) {
        fg = (fg >> 1) & 0x7F7F7FU;  // halve each channel
    }
    if ((attr.flags & core::vt::Attr::kReverse) != 0) {
        std::swap(fg, bg);
    }
    if ((attr.flags & core::vt::Attr::kInvisible) != 0) {
        fg = bg;  // after reverse, so SGR 7 + SGR 8 still hides the text
    }
}

bool selectionContains(const Selection& selection, int row, int col) {
    if (!selection.active) {
        return false;
    }
    // Normalise: the anchor is after the cursor when dragging upwards.
    int firstRow = selection.anchorRow;
    int firstCol = selection.anchorCol;
    int lastRow = selection.cursorRow;
    int lastCol = selection.cursorCol;
    if (lastRow < firstRow || (lastRow == firstRow && lastCol < firstCol)) {
        std::swap(firstRow, lastRow);
        std::swap(firstCol, lastCol);
    }
    if (row < firstRow || row > lastRow) {
        return false;
    }
    if (row == firstRow && row == lastRow) {
        return col >= firstCol && col <= lastCol;
    }
    if (row == firstRow) {
        return col >= firstCol;
    }
    if (row == lastRow) {
        return col <= lastCol;
    }
    return true;  // a whole row in the middle
}

namespace {

// One codepoint to UTF-8. src/core/unicode/utf8.h only decodes — nothing in the
// tree encodes yet, and this is four lines rather than a new module.
void appendUtf8(std::string& out, char32_t code) {
    const auto value = static_cast<std::uint32_t>(code);
    if (value < 0x80) {
        out += static_cast<char>(value);
    } else if (value < 0x800) {
        out += static_cast<char>(0xC0U | (value >> 6));
        out += static_cast<char>(0x80U | (value & 0x3FU));
    } else if (value < 0x10000) {
        out += static_cast<char>(0xE0U | (value >> 12));
        out += static_cast<char>(0x80U | ((value >> 6) & 0x3FU));
        out += static_cast<char>(0x80U | (value & 0x3FU));
    } else {
        out += static_cast<char>(0xF0U | (value >> 18));
        out += static_cast<char>(0x80U | ((value >> 12) & 0x3FU));
        out += static_cast<char>(0x80U | ((value >> 6) & 0x3FU));
        out += static_cast<char>(0x80U | (value & 0x3FU));
    }
}

}  // namespace

std::string selectionText(std::span<const core::vt::Line> viewport, const Selection& selection,
                          const core::vt::ClusterPool& clusters) {
    std::string out;
    if (!selection.active) {
        return out;
    }
    for (int row = 0; row < static_cast<int>(viewport.size()); ++row) {
        const core::vt::Line& line = viewport[static_cast<std::size_t>(row)];
        std::string rowText;
        std::size_t lastNonBlank = 0;
        bool any = false;
        for (int col = 0; col < static_cast<int>(line.cells.size()); ++col) {
            if (!selectionContains(selection, row, col)) {
                continue;
            }
            any = true;
            const char32_t ch = line.cells[static_cast<std::size_t>(col)].ch;
            if (core::vt::isWideTrailing(ch)) {
                // The right-hand cell of a two-column cluster. It owns no text
                // — the cluster lives in the cell to its LEFT and was already
                // emitted — so it contributes nothing at all, not even a space.
                //
                // It must be skipped BEFORE the lookup below: kWideTrailing is
                // not a cluster ref, so lookup() returns an empty span and the
                // literal-codepoint arm would encode 0x80000000 as UTF-8. That
                // put four garbage bytes into the clipboard after every CJK or
                // emoji character a mouse drag crossed.
                continue;
            }
            if (ch == 0) {
                // An unwritten cell. One space here; trailing runs of them are
                // trimmed below.
                rowText += ' ';
                continue;
            }
            const auto cluster = clusters.lookup(ch);
            if (cluster.empty()) {
                appendUtf8(rowText, ch);  // a literal codepoint, not interned
            } else {
                for (const char32_t part : cluster) {
                    appendUtf8(rowText, part);
                }
            }
            lastNonBlank = rowText.size();
        }
        if (!any) {
            continue;
        }
        // Trailing blanks are padding, not content: copying them is how a
        // pasted command ends up with a screen's worth of spaces after it.
        rowText.resize(lastNonBlank);
        out += rowText;

        // A wrapped continuation is the SAME logical line. Breaking it here is
        // what turns one copied command into two broken ones on paste.
        const int next = row + 1;
        const bool lastSelectedRow =
            next >= static_cast<int>(viewport.size()) || !selectionContains(selection, next, 0);
        const bool nextWraps = next < static_cast<int>(viewport.size()) &&
                               viewport[static_cast<std::size_t>(next)].wrappedFromPrev;
        if (!lastSelectedRow && !nextWraps) {
            out += '\n';
        }
    }
    return out;
}

FrameBuilder::FrameBuilder(FaceMetrics metrics, Theme theme) : m_metrics(metrics), m_theme(theme) {}

void FrameBuilder::invalidate() {
    m_invalidated = true;
}

void FrameBuilder::setTheme(Theme theme) {
    m_theme = theme;
    invalidate();  // every cached colour is now wrong
}

void FrameBuilder::build(std::span<const core::vt::Line> viewport,
                         const core::vt::DamageList& damage, const FrameParams& params,
                         const RasterFn& raster, GlyphAtlas& atlas,
                         const std::function<RowRuns(int row)>& rowRuns) {
    const auto rowCount = static_cast<int>(viewport.size());
    if (static_cast<int>(m_rows.size()) != rowCount) {
        m_rows.assign(static_cast<std::size_t>(rowCount), RowCache{});
        m_invalidated = true;  // a resize is one of the three legitimate full redraws
    }

    m_rowsRebuilt = 0;
    for (int row = 0; row < rowCount; ++row) {
        RowCache& cache = m_rows[static_cast<std::size_t>(row)];
        if (!rowNeedsRebuild(row, damage)) {
            continue;
        }
        buildRow(row, viewport[static_cast<std::size_t>(row)], rowRuns(row), raster, atlas,
                 params.sizedAtlas, cache);
        cache.valid = true;
        ++m_rowsRebuilt;
    }
    m_invalidated = false;

    // Flatten. A memcpy of the cached rows, not a rebuild — the point of the
    // per-row cache is that shaping and atlas work were skipped above.
    //
    // ponytail: O(total instances) per frame. At 240x63 that is ~15k instances,
    // which the M0 baseline already showed is far inside budget. If it ever is
    // not, the upgrade is per-row sub-buffer uploads at fixed offsets, which
    // costs a per-row capacity bound.
    m_solids.clear();
    m_glyphs.clear();
    m_sizedGlyphs.clear();
    for (const RowCache& cache : m_rows) {
        m_solids.insert(m_solids.end(), cache.solids.begin(), cache.solids.end());
        m_glyphs.insert(m_glyphs.end(), cache.glyphs.begin(), cache.glyphs.end());
        m_sizedGlyphs.insert(m_sizedGlyphs.end(), cache.sizedGlyphs.begin(),
                             cache.sizedGlyphs.end());
    }

    // Selection and cursor are NOT part of the row cache: they move without the
    // grid changing, so caching them per row would need its own invalidation.
    // Highlights UNDER the selection: the selection is what the user is doing
    // right now, and it has to stay visible over anything a trigger painted.
    appendHighlights(params, rowCount);
    appendSelection(params, rowCount);
    appendCursor(viewport, params);
    appendImages(params);
}

void FrameBuilder::appendImages(const FrameParams& params) {
    m_images.clear();
    m_imageBatches.clear();
    m_belowBatches = 0;
    if (params.images == nullptr || params.placements.empty() || params.rowStable.empty()) {
        return;
    }

    // Stable index -> the FIRST viewport row showing that logical line. Built
    // once per frame rather than scanned per placement: the placement cap is
    // 4096 and the viewport is ~63 rows, so a per-placement scan is a quarter
    // of a million comparisons a hostile stream can ask for on every frame.
    m_rowOfStable.clear();
    for (std::size_t r = 0; r < params.rowStable.size(); ++r) {
        m_rowOfStable.emplace(params.rowStable[r], static_cast<int>(r));
    }

    // Draw order: zIndex ascending, so a negative watermark goes under the text
    // and a higher z sits over a lower one. stable_sort keeps transmission
    // order among equal z, which is what kitty specifies for overlaps.
    m_sortedPlacements.clear();
    for (std::uint32_t i = 0; i < params.placements.size(); ++i) {
        m_sortedPlacements.push_back(i);
    }
    std::stable_sort(m_sortedPlacements.begin(), m_sortedPlacements.end(),
                     [&params](std::uint32_t lhs, std::uint32_t rhs) {
                         return params.placements[lhs].zIndex < params.placements[rhs].zIndex;
                     });

    const float cellW = static_cast<float>(m_metrics.cellWidth);
    const float cellH = static_cast<float>(cellHeight());
    bool boundaryCrossed = false;

    // ponytail: at most this many quads a frame. ImageStore caps placements at
    // 4096 and they can all be anchored inside one viewport, so without a bound
    // a hostile stream buys itself 4096 pipeline binds and draws per frame —
    // remote input choosing the render cost (rules/net.md). A terminal showing
    // more than this many pictures at once has nothing legible on it anyway.
    // Upgrade path: sort by z, keep the topmost N, if anyone ever hits it.
    constexpr std::size_t kMaxDrawnPlacements = 256;

    for (const std::uint32_t index : m_sortedPlacements) {
        if (m_images.size() >= kMaxDrawnPlacements) {
            break;
        }
        const core::vt::Placement& placement = params.placements[index];
        if (placement.cols <= 0 || placement.rows <= 0) {
            continue;
        }
        const core::vt::Image* image = params.images->find(placement.imageId);
        if (image == nullptr || image->empty()) {
            continue;  // the pixels were evicted; the placement outlived them
        }
        const auto found = m_rowOfStable.find(placement.anchor);
        if (found == m_rowOfStable.end()) {
            // Scrolled out of the viewport entirely.
            //
            // ponytail: a placement whose anchor line is ABOVE the top row
            // vanishes rather than drawing clipped, because working out how far
            // above it went costs a walk back through history this has no other
            // reason to do. Visible effect: an image pops out as its first row
            // leaves the top, instead of sliding off. Upgrade path: have the
            // caller report an out-of-view anchor as a negative row and let the
            // render pass clip the quad.
            continue;
        }

        // The source rectangle. Zero width or height means the whole image,
        // which is the common case and what both decoders emit.
        const float imgW = static_cast<float>(image->width);
        const float imgH = static_cast<float>(image->height);
        const float srcW = placement.srcW > 0 ? static_cast<float>(placement.srcW) : imgW;
        const float srcH = placement.srcH > 0 ? static_cast<float>(placement.srcH) : imgH;

        // The anchor names a LOGICAL line, so it resolves to that line's first
        // viewport row; rowInLine is how far into the line the image actually
        // sat. Without it an image emitted after 120 columns of output at
        // width 80 draws a row above its own text.
        const int row = found->second + placement.rowInLine;
        if (row >= static_cast<int>(params.rowStable.size())) {
            continue;  // the continuation row it belongs to is off the bottom
        }

        ImageInstance quad;
        quad.x = static_cast<float>(placement.col) * cellW;
        quad.y = static_cast<float>(row) * cellH;
        quad.w = static_cast<float>(placement.cols) * cellW;
        quad.h = static_cast<float>(placement.rows) * cellH;
        quad.u0 = std::clamp(static_cast<float>(placement.srcX) / imgW, 0.0F, 1.0F);
        quad.v0 = std::clamp(static_cast<float>(placement.srcY) / imgH, 0.0F, 1.0F);
        quad.u1 = std::clamp((static_cast<float>(placement.srcX) + srcW) / imgW, 0.0F, 1.0F);
        quad.v1 = std::clamp((static_cast<float>(placement.srcY) + srcH) / imgH, 0.0F, 1.0F);
        // rgb at 1 so the fragment shader's multiply is a no-op; alpha is the
        // placement's opacity, which today is always fully opaque.
        quad.r = 1.0F;
        quad.g = 1.0F;
        quad.b = 1.0F;
        quad.a = 1.0F;

        // Sorted by zIndex, so every under-text placement precedes every
        // over-text one and the boundary is crossed at most once.
        bool startBatch =
            m_imageBatches.empty() || m_imageBatches.back().imageId != placement.imageId;
        if (placement.zIndex >= 0 && !boundaryCrossed) {
            boundaryCrossed = true;
            m_belowBatches = static_cast<std::uint32_t>(m_imageBatches.size());
            startBatch = true;  // one batch must never straddle the text
        }
        if (startBatch) {
            m_imageBatches.push_back({.imageId = placement.imageId,
                                      .first = static_cast<std::uint32_t>(m_images.size()),
                                      .count = 1});
        } else {
            ++m_imageBatches.back().count;
        }
        m_images.push_back(quad);
    }
    if (!boundaryCrossed) {
        // Every placement drew under the text (or there were none at all).
        m_belowBatches = static_cast<std::uint32_t>(m_imageBatches.size());
    }
}

void FrameBuilder::appendHighlights(const FrameParams& params, int rowCount) {
    const float cellW = static_cast<float>(m_metrics.cellWidth);
    const float cellH = static_cast<float>(cellHeight());
    for (const HighlightSpan& span : params.highlights) {
        if (span.row < 0 || span.row >= rowCount || span.endCol <= span.beginCol) {
            continue;
        }
        const int first = std::max(0, span.beginCol);
        const int last = std::min(params.cols, span.endCol);
        if (last <= first) {
            continue;
        }
        SolidInstance rect;
        rect.x = static_cast<float>(first) * cellW;
        rect.y = static_cast<float>(span.row) * cellH;
        rect.w = static_cast<float>(last - first) * cellW;
        rect.h = cellH;
        unpackColor(m_theme.highlightBg, rect.r, rect.g, rect.b);
        // Semi-transparent for the same reason the selection is: the glyphs
        // underneath stay legible without a second text pass in another colour.
        rect.a = 0.35F;
        m_solids.push_back(rect);
    }
}

bool FrameBuilder::rowNeedsRebuild(int row, const core::vt::DamageList& damage) const {
    if (m_invalidated || damage.all()) {
        return true;
    }
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return true;  // a row we have never seen
    }
    if (!m_rows[static_cast<std::size_t>(row)].valid) {
        return true;
    }
    const auto& spans = damage.spans();
    return row < static_cast<int>(spans.size()) && spans[static_cast<std::size_t>(row)].col0 >= 0;
}

void FrameBuilder::buildRow(int row, const core::vt::Line& line, const RowRuns& runs,
                            const RasterFn& raster, GlyphAtlas& atlas, GlyphAtlas* sizedAtlas,
                            RowCache& out) const {
    out.solids.clear();
    out.glyphs.clear();
    out.sizedGlyphs.clear();

    const float cellW = static_cast<float>(m_metrics.cellWidth);
    const float cellH = static_cast<float>(cellHeight());
    const float top = static_cast<float>(row) * cellH;
    const auto cols = static_cast<int>(line.cells.size());

    // Backgrounds first, coalesced into runs of equal colour. A terminal row is
    // usually one background, so emitting one quad per cell would multiply the
    // instance count by the column count for nothing.
    int spanStart = 0;
    std::uint32_t spanBg = 0;
    bool spanOpen = false;
    const auto flushSpan = [&](int endCol) {
        if (!spanOpen || spanBg == m_theme.bg) {
            return;  // the default background is already the clear colour
        }
        SolidInstance rect;
        rect.x = static_cast<float>(spanStart) * cellW;
        rect.y = top;
        rect.w = static_cast<float>(endCol - spanStart) * cellW;
        rect.h = cellH;
        unpackColor(spanBg, rect.r, rect.g, rect.b);
        out.solids.push_back(rect);
    };

    for (int col = 0; col < cols; ++col) {
        std::uint32_t fg = 0;
        std::uint32_t bg = 0;
        resolveColors(line.cells[static_cast<std::size_t>(col)].attr, m_theme, fg, bg);
        if (!spanOpen || bg != spanBg) {
            flushSpan(col);
            spanStart = col;
            spanBg = bg;
            spanOpen = true;
        }
    }
    flushSpan(cols);

    // Underline and strikethrough, per cell — they follow the pen, not the
    // background, and coalesce rarely enough not to be worth the bookkeeping.
    const float thickness = static_cast<float>(decorationThickness(cellHeight()));
    for (int col = 0; col < cols; ++col) {
        const core::vt::Attr& attr = line.cells[static_cast<std::size_t>(col)].attr;
        if (attr.underline == core::vt::Underline::None &&
            (attr.flags & core::vt::Attr::kStrike) == 0) {
            continue;
        }
        std::uint32_t fg = 0;
        std::uint32_t bg = 0;
        resolveColors(attr, m_theme, fg, bg);
        // A SET underline colour survives reverse video; an unset one tracks the
        // foreground. That is exactly why Attr keeps `ul` separate (see cell.h).
        std::uint32_t lineColor = fg;
        if (attr.ul.kind() == core::vt::Color::Kind::Rgb) {
            lineColor = attr.ul.rgb();
        } else if (attr.ul.kind() == core::vt::Color::Kind::Indexed) {
            lineColor = paletteColor(attr.ul.index(), m_theme);
        }

        if (attr.underline != core::vt::Underline::None) {
            SolidInstance rect;
            rect.x = static_cast<float>(col) * cellW;
            // Just below the baseline, clamped inside the cell.
            rect.y =
                top + std::min(cellH - thickness, static_cast<float>(m_metrics.ascent) + thickness);
            rect.w = cellW;
            rect.h = thickness;
            unpackColor(lineColor, rect.r, rect.g, rect.b);
            out.solids.push_back(rect);
            if (attr.underline == core::vt::Underline::Double) {
                SolidInstance second = rect;
                second.y = std::min(top + cellH - thickness, rect.y + (2 * thickness));
                out.solids.push_back(second);
            }
            // ponytail: Curly, Dotted and Dashed draw as a plain single line for
            // now — they need a pattern texture or a shader branch. The styles
            // are already parsed and stored (T17), so this is a pure render
            // upgrade with no core change behind it.
        }
        if ((attr.flags & core::vt::Attr::kStrike) != 0) {
            SolidInstance rect;
            rect.x = static_cast<float>(col) * cellW;
            rect.y = top + (static_cast<float>(m_metrics.ascent) * 0.6F);
            rect.w = cellW;
            rect.h = thickness;
            unpackColor(lineColor, rect.r, rect.g, rect.b);
            out.solids.push_back(rect);
        }
    }

    // Glyphs. Each glyph is positioned from its CLUSTER's column with the
    // shaper's offsets on top — that is what puts a Thai mark over its base
    // instead of in the next cell.
    for (std::size_t i = 0; i < runs.shaped.size() && i < runs.runs.size(); ++i) {
        const Run& run = runs.runs[i];
        const ShapedRun& shaped = runs.shaped[i];
        const std::uint32_t faceId = i < runs.faces.size() ? runs.faces[i] : shaped.faceId;

        // OSC 66 (T84). A scaled run was shaped against a face registered at
        // scale * pxHeight, so its glyphs are that much bigger and go in the
        // sized atlas — the main one's slots are one line tall and it rejects
        // anything larger. With no sized atlas the run is skipped entirely
        // rather than drawn at the wrong size.
        //
        // shaped.scale, NOT run.scale: the re-shape can fail (a face that will
        // not load at that height, a batch that timed out) and then these are
        // ordinary 1x glyphs. Routing them by what was ASKED FOR would put a
        // 1x glyph in a scale-7 slot and draw it a whole block too low.
        const int scale = std::max<int>(1, shaped.scale);
        GlyphAtlas* target = scale > 1 ? sizedAtlas : &atlas;
        if (target == nullptr) {
            continue;
        }
        std::vector<GlyphInstance>& sink = scale > 1 ? out.sizedGlyphs : out.glyphs;
        const float atlasW = static_cast<float>(target->width());
        const float atlasH = static_cast<float>(target->height());
        // The block is `scale` rows tall and hangs DOWN from the row the cursor
        // was on, so the baseline is that many ascents below the row top. The
        // spec never says "downward" in one sentence, but its overwrite rules
        // are all phrased around "the top-left cell" and "rows after the first"
        // (kitty text-sizing protocol), which only reads that way.
        const float baseline =
            top + (static_cast<float>(scale) * static_cast<float>(m_metrics.ascent));

        for (const ShapedGlyph& glyph : shaped.glyphs) {
            if (glyph.cluster >= run.clusters.size()) {
                continue;  // defensive: a mismatched run/shaped pair
            }
            const ClusterRef& cluster = run.clusters[glyph.cluster];
            const auto col = static_cast<std::size_t>(cluster.col);
            if (col >= line.cells.size()) {
                continue;
            }
            std::uint32_t fg = 0;
            std::uint32_t bg = 0;
            resolveColors(line.cells[col].attr, m_theme, fg, bg);
            if (fg == bg) {
                continue;  // SGR 8, invisible: skip the draw entirely
            }
            const AtlasEntry* entry =
                target->get(GlyphKey{.faceId = faceId, .glyphId = glyph.glyphId}, raster);
            if (entry == nullptr || entry->width == 0 || entry->height == 0) {
                continue;  // no ink (a space), or refused by the atlas
            }

            GlyphInstance inst;
            inst.x = (static_cast<float>(cluster.col) * cellW) +
                     (static_cast<float>(glyph.xOffset) * kFixedScale) +
                     static_cast<float>(entry->bearingX);
            inst.y = baseline - static_cast<float>(entry->bearingY) -
                     (static_cast<float>(glyph.yOffset) * kFixedScale);
            inst.w = static_cast<float>(entry->width);
            inst.h = static_cast<float>(entry->height);
            inst.u0 = static_cast<float>(entry->x) / atlasW;
            inst.v0 = static_cast<float>(entry->y) / atlasH;
            inst.u1 = static_cast<float>(entry->x + entry->width) / atlasW;
            inst.v1 = static_cast<float>(entry->y + entry->height) / atlasH;
            unpackColor(fg, inst.r, inst.g, inst.b);
            sink.push_back(inst);
        }
    }
}

void FrameBuilder::appendShapedRun(const Run& run, const ShapedRun& shaped, std::uint32_t faceId,
                                   std::uint32_t fg, const RasterFn& raster, GlyphAtlas& atlas,
                                   std::vector<GlyphInstance>& out) const {
    const float cellW = static_cast<float>(m_metrics.cellWidth);
    const float top = static_cast<float>(run.row) * static_cast<float>(cellHeight());
    const float atlasW = static_cast<float>(atlas.width());
    const float atlasH = static_cast<float>(atlas.height());

    for (const ShapedGlyph& glyph : shaped.glyphs) {
        if (glyph.cluster >= run.clusters.size()) {
            continue;  // defensive: a mismatched run/shaped pair
        }
        const ClusterRef& cluster = run.clusters[glyph.cluster];
        const AtlasEntry* entry =
            atlas.get(GlyphKey{.faceId = faceId, .glyphId = glyph.glyphId}, raster);
        if (entry == nullptr || entry->width == 0 || entry->height == 0) {
            continue;  // no ink (a space), or refused by the atlas
        }

        GlyphInstance inst;
        inst.x = (static_cast<float>(cluster.col) * cellW) +
                 (static_cast<float>(glyph.xOffset) * kFixedScale) +
                 static_cast<float>(entry->bearingX);
        inst.y = top + static_cast<float>(m_metrics.ascent) - static_cast<float>(entry->bearingY) -
                 (static_cast<float>(glyph.yOffset) * kFixedScale);
        inst.w = static_cast<float>(entry->width);
        inst.h = static_cast<float>(entry->height);
        inst.u0 = static_cast<float>(entry->x) / atlasW;
        inst.v0 = static_cast<float>(entry->y) / atlasH;
        inst.u1 = static_cast<float>(entry->x + entry->width) / atlasW;
        inst.v1 = static_cast<float>(entry->y + entry->height) / atlasH;
        unpackColor(fg, inst.r, inst.g, inst.b);
        out.push_back(inst);
    }
}

void FrameBuilder::appendSelection(const FrameParams& params, int rowCount) {
    if (!params.selection.active) {
        return;
    }
    const float cellW = static_cast<float>(m_metrics.cellWidth);
    const float cellH = static_cast<float>(cellHeight());

    for (int row = 0; row < rowCount; ++row) {
        // One rect per contiguous selected span, not per cell.
        int start = -1;
        for (int col = 0; col <= params.cols; ++col) {
            const bool inside = col < params.cols && selectionContains(params.selection, row, col);
            if (inside && start < 0) {
                start = col;
            } else if (!inside && start >= 0) {
                SolidInstance rect;
                rect.x = static_cast<float>(start) * cellW;
                rect.y = static_cast<float>(row) * cellH;
                rect.w = static_cast<float>(col - start) * cellW;
                rect.h = cellH;
                unpackColor(m_theme.selectionBg, rect.r, rect.g, rect.b);
                // Semi-transparent so the glyphs beneath stay legible without a
                // second text pass in a selection foreground colour.
                rect.a = 0.45F;
                m_solids.push_back(rect);
                start = -1;
            }
        }
    }
}

void FrameBuilder::appendCursor(std::span<const core::vt::Line> viewport,
                                const FrameParams& params) {
    const CursorState& cursor = params.cursor;
    if (!cursor.visible || cursor.row < 0 || cursor.row >= static_cast<int>(viewport.size())) {
        return;
    }
    const float cellW = static_cast<float>(m_metrics.cellWidth);
    const float cellH = static_cast<float>(cellHeight());
    const float x = static_cast<float>(cursor.col) * cellW;
    const float y = static_cast<float>(cursor.row) * cellH;
    const float thickness = static_cast<float>(decorationThickness(cellHeight()));

    // An unfocused terminal shows an outline instead of a filled block. Every
    // terminal does this, and it is the only cue that keystrokes go elsewhere.
    const CursorStyle style = !cursor.focused && cursor.style == CursorStyle::Block
                                  ? CursorStyle::HollowBlock
                                  : cursor.style;

    const auto push = [this](float rx, float ry, float rw, float rh) {
        SolidInstance rect;
        rect.x = rx;
        rect.y = ry;
        rect.w = rw;
        rect.h = rh;
        unpackColor(m_theme.cursor, rect.r, rect.g, rect.b);
        m_solids.push_back(rect);
    };

    switch (style) {
    case CursorStyle::Block:
        push(x, y, cellW, cellH);
        break;
    case CursorStyle::HollowBlock:
        push(x, y, cellW, thickness);                      // top
        push(x, y + cellH - thickness, cellW, thickness);  // bottom
        push(x, y, thickness, cellH);                      // left
        push(x + cellW - thickness, y, thickness, cellH);  // right
        break;
    case CursorStyle::Underline:
        push(x, y + cellH - thickness, cellW, thickness);
        break;
    case CursorStyle::Bar:
        push(x, y, thickness, cellH);
        break;
    }
}

}  // namespace krait::render

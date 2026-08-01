// libFuzzer target (ADR-0010): bytes -> parser -> grid/handlers, with
// invariant asserts. Build via the `fuzz` (clang-cl, primary) or
// `fuzz-msvc` (experimental fallback) preset — both keep asserts enabled.
#include "core/caps/caps.h"
#include "core/grid/grid.h"
#include "core/parser/csi_cursor.h"
#include "core/parser/csi_mode.h"
#include "core/parser/csi_scroll.h"
#include "core/parser/machine.h"
#include "core/parser/osc.h"
#include "core/parser/sgr.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace {

using namespace krait::core::vt;

class FuzzSink final : public ParserEvents {
  public:
    Grid grid{24, 80};
    Capabilities caps;
    ReplyLimiter limiter;
    OscHandler osc;
    std::string replies;

    void print(char32_t cp) override {
        grid.putChar(cp);
        checkCursor();
    }

    void execute(std::uint8_t control) override {
        handleControl(grid, control);
        checkCursor();
    }

    void escDispatch(std::span<const std::uint8_t> intermediates, std::uint8_t final) override {
        assert(intermediates.size() <= 2);
        (void)intermediates;
        (void) final;
    }

    void csiDispatch(const Params& params, std::span<const std::uint8_t> intermediates,
                     std::uint8_t final) override {
        assert(params.count <= Params::kMaxParams);
        for (std::size_t i = 0; i < params.count; ++i) {
            assert(params.values[i] <= Params::kMaxValue);
        }
        if (final == 'm') {
            applySgr(grid.pen, params, intermediates);
        } else if (final == 'J' || final == 'K') {
            handleErase(grid, params, intermediates, final);
        } else if (final == 'r' || final == 'L' || final == 'M' || final == 'S' || final == 'T') {
            handleScroll(grid, params, intermediates, final);
        } else if (final == 'h' || final == 'l') {
            handleMode(grid, params, intermediates, final);
        } else if (final == 'c' || final == 'n') {
            handleReport(grid, caps, params, intermediates, final, limiter, replies);
        } else if (final == 'p') {
            handleDecrqm(grid, caps, params, intermediates, final, limiter, replies);
        } else {
            handleCsiCursor(grid, params, intermediates, final);
        }
        checkCursor();
        checkRowWidths();
    }

    void dcsHook(const Params&, std::span<const std::uint8_t>, std::uint8_t) override {}

    void dcsPut(std::uint8_t) override {}

    void dcsUnhook(bool) override {}

    // OSC is routed exactly as Session::oscEnd routes it (T66). Without this
    // the OSC 133 seeds would exercise the state machine and nothing else —
    // a fuzz run that looks clean because it never reached the code.
    void oscStart() override { osc.start(); }

    void oscPut(std::uint8_t byte) override { osc.put(byte); }

    void oscEnd(bool aborted) override {
        const OscAction action = osc.end(aborted);
        if (action.kind != OscAction::Kind::PromptMark) {
            return;  // clipboard/title/hyperlink are app-layer, nothing to model
        }
        grid.markPrompt(action.promptMark);
        if (action.promptMark == kMarkCommandEnd) {
            grid.setCommandExit(action.exitCode);
        }
        // NOT checkPrompts() here: it is O(history) and would run per OSC,
        // which collapses exec/s once the fuzzer learns to fill scrollback and
        // shows up as timeouts rather than as the slowdown it is. Once per
        // input, from LLVMFuzzerTestOneInput, finds the same bugs.
    }

    // Every invariant at once. Public because resize() is driven from
    // LLVMFuzzerTestOneInput rather than from a parser event.
    // Not const: checkPrompts() drives the viewport, which is the only way to
    // fuzz the offset arithmetic behind jump-to-prompt (T67). It puts the view
    // back at the bottom before it returns.
    void checkAll() {
        checkCursor();
        checkRowWidths();
        checkClusters();
        checkPrompts();
    }

    // T66. Prompt navigation is index arithmetic across two containers whose
    // sizes both move under a resize, and it is handed indices by a UI that
    // does not resynchronise first — so an answer outside the list is how this
    // becomes an out-of-bounds read at the call site rather than here.
    //
    // T67 adds the jump itself, which walks attacker-controlled CELLS to turn a
    // logical line index into a visual offset — the one place in the core where
    // remote content decides how far a loop runs.
    void checkPrompts() {
        const std::size_t count = grid.absoluteLineCount();
        for (const std::size_t from :
             {std::size_t{0}, count / 2, count, count * 4, static_cast<std::size_t>(-1)}) {
            if (const auto at = grid.prevPrompt(from)) {
                assert(*at < count);
                assert((grid.absoluteLineAt(*at).marks & kMarkPromptStart) != 0);
                grid.scrollToLine(*at);
                assert(grid.viewOffset() >= 0);
                assert(grid.viewportRows().size() == static_cast<std::size_t>(grid.rows));
                assert(grid.viewTopLine() <= grid.absoluteLineCount());
            }
            if (const auto at = grid.nextPrompt(from)) {
                assert(*at < count);
                assert(*at > from);
                assert((grid.absoluteLineAt(*at).marks & kMarkPromptStart) != 0);
            }
        }
        grid.scrollViewToBottom();
    }

  private:
    void checkCursor() const {
        assert(grid.row >= 0 && grid.row < grid.rows);
        assert(grid.col >= 0 && grid.col < grid.cols);
        // The margins index m_screen directly in scrollRegionUp/Down, so an
        // inverted or out-of-range region is an out-of-bounds write waiting for
        // the right input. DECSTBM is reachable from the first byte.
        assert(grid.scrollTop >= 0);
        assert(grid.scrollTop <= grid.scrollBottom);
        assert(grid.scrollBottom < grid.rows);
    }

    // O(rows), so it stays OUT of checkCursor() — that runs per printable byte.
    // Only a buffer swap can make a row the wrong width, and only csiDispatch
    // can trigger one.
    void checkRowWidths() const {
        for (int r = 0; r < grid.rows; ++r) {
            assert(grid.lineAt(r).cells.size() == static_cast<std::size_t>(grid.cols));
        }
    }

    // T20. Both of these are how a wide-cell or reflow bug turns into an
    // out-of-bounds read in the renderer, which reads a cell's left neighbour
    // to find the cluster a trailing half belongs to.
    //
    // NOT asserted: that a trailing half's lead is non-blank. ED/EL can erase
    // the lead of a wide cluster and leave its trailing cell behind — a real
    // gap, recorded in docs/conformance.md, but a cosmetic one rather than a
    // memory-safety one. Asserting it here would fuzz-fail on known behaviour.
    void checkClusters() const {
        for (int r = 0; r < grid.rows; ++r) {
            for (int c = 0; c < grid.cols; ++c) {
                const char32_t ch = grid.cellAt(r, c).ch;
                if (isWideTrailing(ch)) {
                    assert(c > 0);  // never the first column
                    assert(!isWideTrailing(grid.cellAt(r, c - 1).ch));
                } else if (isClusterRef(ch)) {
                    assert(!grid.clusters().lookup(ch).empty());
                }
            }
        }
    }
};

}  // namespace

// MSVC's libFuzzer port requires this optional hook to exist; clang treats
// it as weak. No-op either way.
extern "C" int LLVMFuzzerInitialize(int*, char***) {
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzSink sink;
    Parser parser(sink, size % 2 == 0);  // exercise both C1 policies
    sink.limiter.addInput(size);
    parser.feed({data, size});

    // Amplification invariant: replies bounded by the limiter's windows.
    const std::size_t maxReplies = 8 * (1 + size / 256);
    assert(sink.replies.size() <= maxReplies * 16);

    sink.checkPrompts();  // T66, once per input rather than per OSC

    // T20: reflow, driven from the same bytes. A resize is reachable from any
    // window drag, so rewrap has to survive whatever the parser just wrote —
    // and it is the one path that walks every cell it did not write itself.
    // Both extremes matter: 1 column cannot hold a wide pair at all, and the
    // round trip back to the original geometry catches a rewrap that corrupts
    // state only on the second pass.
    // Two resizes, not three: the derived width already reaches 1 column (the
    // degenerate case a wide pair cannot fit in), and a rewrap is O(rows*cols)
    // with an allocation per row, so each extra one is paid on every single
    // execution. An explicit 1x1 pass cost ~7x the total exec count for
    // coverage the derived case already had.
    if (size >= 2) {
        sink.grid.resize(1 + (data[size - 1] % 60), 1 + (data[0] % 120));
        sink.checkAll();
        sink.grid.resize(24, 80);  // round trip: catches a rewrap that only corrupts on pass two
        sink.checkAll();
    }
    (void)sink;
    return 0;
}

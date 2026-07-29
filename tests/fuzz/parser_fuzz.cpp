// libFuzzer target (ADR-0010): bytes -> parser -> grid/handlers, with
// invariant asserts. Build via the `fuzz` (clang-cl, primary) or
// `fuzz-msvc` (experimental fallback) preset — both keep asserts enabled.
#include "core/caps/caps.h"
#include "core/grid/grid.h"
#include "core/parser/csi_cursor.h"
#include "core/parser/machine.h"
#include "core/parser/sgr.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace {

using namespace krait::core::vt;

class FuzzSink final : public ParserEvents {
  public:
    Grid grid{24, 80};
    Capabilities caps;
    ReplyLimiter limiter;
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
        } else if (final == 'c' || final == 'n') {
            handleReport(grid, caps, params, intermediates, final, limiter, replies);
        } else {
            handleCsiCursor(grid, params, intermediates, final);
        }
        checkCursor();
    }

    void dcsHook(const Params&, std::span<const std::uint8_t>, std::uint8_t) override {}

    void dcsPut(std::uint8_t) override {}

    void dcsUnhook(bool) override {}

    void oscStart() override {}

    void oscPut(std::uint8_t) override {}

    void oscEnd(bool) override {}

  private:
    void checkCursor() const {
        assert(grid.row >= 0 && grid.row < grid.rows);
        assert(grid.col >= 0 && grid.col < grid.cols);
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
    (void)sink;
    return 0;
}

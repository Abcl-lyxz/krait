#pragma once

#include "core/grid/cell.h"
#include "core/parser/events.h"

#include <cstdint>
#include <span>
#include <vector>

namespace krait::core::vt {

// Minimal cursor-only grid for T6: position, fixed tab stops, shift state,
// bell count. No cells, no scrolling, no margins — the real grid lands in
// T8 and replaces this. Internally 0-based; wire parameters are 1-based.
struct StubGrid {
    int rows = 24;
    int cols = 80;
    int row = 0;
    int col = 0;
    bool g1Invoked = false;  // SO/SI shift state; charset mapping is later work
    int bells = 0;
    Attr pen;  // current SGR pen (T7)
    // Fixed 24x80 cell store for T7 ED/EL tests; sized for the default
    // dimensions only — the real, resizable grid is T8.
    std::vector<Cell> cells = std::vector<Cell>(static_cast<std::size_t>(rows) * cols);

    // Writes at the cursor with the current pen and advances; no wrap until
    // the real grid (clamps at the last column, overwriting).
    void putChar(char32_t ch) {
        cells[static_cast<std::size_t>(row) * cols + col] = {ch, pen};
        col = col < cols - 1 ? col + 1 : cols - 1;
    }
};

// Applies one C0 control (BEL BS HT LF CR SO SI). Returns false for
// controls T6 does not implement (caller decides; nothing today).
bool handleControl(StubGrid& grid, std::uint8_t control) noexcept;

// Applies one CSI cursor sequence (CUU CUD CUF CUB CUP HVP CHA VPA).
// Returns false for other finals or when private markers/intermediates are
// present — those belong to other families.
bool handleCsiCursor(StubGrid& grid, const Params& params,
                     std::span<const std::uint8_t> intermediates, std::uint8_t final) noexcept;

}  // namespace krait::core::vt

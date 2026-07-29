#pragma once

#include "core/grid/cell.h"
#include "core/parser/csi_cursor.h"
#include "core/parser/events.h"

#include <cstdint>
#include <span>

namespace krait::core::vt {

// SGR: 0-29, 30-49, 90-107 plus extended color and underline styles (T17).
// 38/48/58 accept the legacy semicolon form, the ITU/xterm colon form with a
// color-space id, and the colon form without one — told apart by counting
// subparameters. A malformed or out-of-range color changes nothing but still
// consumes its own arguments, so following parameters are never misread.
// 4:0-4:5 select underline styles; 21 is doubly-underlined per ECMA-48.
// Returns false when intermediates/markers are present.
bool applySgr(Attr& pen, const Params& params,
              std::span<const std::uint8_t> intermediates) noexcept;

// ED (CSI Ps J) / EL (CSI Ps K) on the stub grid. Ps defaults to 0 here
// (0 is a real value for erase, unlike the cursor family). ED 3 (scrollback)
// is a no-op until the real grid. Returns false for other finals or when
// intermediates/markers are present (DECSED/DECSEL not implemented).
bool handleErase(Grid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
                 std::uint8_t final) noexcept;

}  // namespace krait::core::vt

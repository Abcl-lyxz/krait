#pragma once

#include "core/grid/cell.h"
#include "core/parser/csi_cursor.h"
#include "core/parser/events.h"

#include <cstdint>
#include <span>

namespace krait::core::vt {

// SGR basic (T7): 0-29, 30-49, 90-107. Extended color (38/48/58) is
// consumed with correct arity (colon or legacy semicolon form) but applies
// nothing until M1 — following parameters are never misread. Returns false
// when intermediates/markers are present.
bool applySgr(Attr& pen, const Params& params,
              std::span<const std::uint8_t> intermediates) noexcept;

// ED (CSI Ps J) / EL (CSI Ps K) on the stub grid. Ps defaults to 0 here
// (0 is a real value for erase, unlike the cursor family). ED 3 (scrollback)
// is a no-op until the real grid. Returns false for other finals or when
// intermediates/markers are present (DECSED/DECSEL not implemented).
bool handleErase(Grid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
                 std::uint8_t final) noexcept;

}  // namespace krait::core::vt

#pragma once

#include "core/grid/grid.h"
#include "core/parser/events.h"

#include <cstdint>
#include <span>

namespace krait::core::vt {

// Scrolling-region and line-editing family (T18):
//   DECSTBM  CSI Pt ; Pb r    set the scrolling region
//   IL       CSI Pn L         insert lines inside the region
//   DL       CSI Pn M         delete lines inside the region
//   SU       CSI Pn S         scroll the region up
//   SD       CSI Pn T         scroll the region down
//
// Returns false when the sequence is not ours, so the caller can try the next
// family: other finals, any intermediate/private marker, or a colon
// subparameter (subparameters are SGR-only, the pattern every non-SGR handler
// in this parser follows).
//
// `CSI Ps T` is ambiguous with highlight mouse tracking. It is SD only with
// EXACTLY one parameter whose value is non-zero — `CSI 0 T` is not SD, and the
// 2-to-4 parameter forms are neither SD nor a documented tracking form, so
// they are consumed and ignored rather than guessed at.
bool handleScroll(Grid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
                  std::uint8_t final) noexcept;

}  // namespace krait::core::vt

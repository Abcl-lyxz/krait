#pragma once

#include "core/grid/grid.h"
#include "core/parser/events.h"

#include <cstdint>
#include <span>

namespace krait::core::vt {

// Applies one C0 control (BEL BS HT LF CR SO SI). Returns false for
// controls not implemented yet (caller decides; nothing today).
bool handleControl(Grid& grid, std::uint8_t control) noexcept;

// Applies one CSI cursor sequence (CUU CUD CUF CUB CUP HVP CHA VPA).
// Returns false for other finals or when private markers/intermediates or
// colon subparams are present — those belong to other families.
bool handleCsiCursor(Grid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
                     std::uint8_t final) noexcept;

}  // namespace krait::core::vt

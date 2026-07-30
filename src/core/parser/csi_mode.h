#pragma once

#include "core/grid/grid.h"
#include "core/parser/events.h"

#include <cstdint>
#include <span>

namespace krait::core::vt {

// DECSET / DECRST — the DEC private mode seam (T18):
//   DECOM  CSI ? 6 h / l      origin mode: addressing relative to the margins
//   1049   CSI ? 1049 h / l   alternate screen buffer + cursor save/restore
//
// Only the `?` private forms are ours. ANSI modes (CSI 4 h IRM, CSI 20 h LNM,
// ...) are NOT implemented, so they fall through unhandled rather than being
// silently accepted, and `CSI > 4 h` style markers belong to other families.
//
// An unrecognised DEC private mode IS consumed and ignored — that is what every
// terminal does with a mode it lacks, and it cannot become a false claim
// because DECRQM is not answered yet (T22). When DECRQM lands it must report
// "not recognised" for everything not switched on here, which is why this
// handler keeps an explicit list instead of a catch-all setter.
//
// Modes 47 and 1047 (the older alt-screen spellings, which differ in clear and
// save-cursor behavior) are deliberately not implemented — nothing modern emits
// them without 1049.
bool handleMode(Grid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
                std::uint8_t final) noexcept;

}  // namespace krait::core::vt

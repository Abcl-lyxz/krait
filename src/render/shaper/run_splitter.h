#pragma once

#include "core/grid/cluster_pool.h"
#include "render/shaper/shaped_run.h"

#include <span>
#include <vector>

namespace krait::render {

// Splits one row of cells into HarfBuzz-sized pieces (T23).
//
// A run is a maximal span of a row that can go into ONE hb_buffer_t. What ends
// a run, and why each rule exists:
//
//   * a change in shapingBits() — bold/italic pick a different face, so the
//     glyph ids are not comparable across the boundary.
//   * a script change. This is the rule a naive splitter skips, and skipping it
//     is why Thai renders wrong: HarfBuzz shapes one script per buffer and
//     hb_buffer_guess_segment_properties() guesses from the FIRST character, so
//     `user@host:~$ สวัสดี` in a single buffer is shaped as Latin and the Thai
//     marks never position. Common/Inherited/Unknown are weak — digits, spaces
//     and combining marks join whatever run they land in rather than breaking
//     it (UAX #24's model).
//   * an unwritten cell (ch == 0). A row is mostly empty most of the time;
//     including its tail would put a 240-codepoint key in the cache for what is
//     three characters of text. Note that a PRINTED space (0x20) is text and
//     stays inside the run — Krait can tell the two apart (T20).
//   * the end of the row.
//
// A kWideTrailing cell breaks nothing and contributes no text: it is the right
// half of the cluster to its left, and that cluster already carries cells == 2.
//
// Clusters arrive already segmented from the grid (T19/T20). Nothing here calls
// utf8proc or re-measures width — the cell storage is the single source of
// truth, the same rule reflow follows.
//
// `out` is appended to and never cleared, so a caller can walk a whole viewport
// into one vector and reuse its capacity across frames.
void splitRow(std::span<const core::vt::Cell> cells, const core::vt::ClusterPool& pool, int row,
              std::vector<Run>& out);

// The script of a single codepoint, as an ISO 15924 tag. Exposed for tests;
// production code goes through splitRow.
ScriptTag scriptOf(char32_t cp);

// Whether a script joins the surrounding run instead of breaking it
// (Common, Inherited, Unknown). Exposed for tests.
bool isWeakScript(ScriptTag script);

}  // namespace krait::render

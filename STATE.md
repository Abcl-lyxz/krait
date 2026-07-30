# STATE

Phase: **M1 in progress** — T17-T25 done. **Next task: T26 (device robustness).**

## Now

T23, T24 and T25 are on `t23-shaper` as **PR #21**, four commits:

| Commit | Task |
|---|---|
| `a33675b` | T23 shaper — run splitting, worker pool, shaped-run cache |
| `f0ecd11` | T24 font stack — DirectWrite resolution, fallback, ligature toggle |
| `749eb0c` | T25 (1/2) — glyph atlas with LRU eviction, damage-driven frame builder |
| `4b1b3a8` | T25 (2/2) — the real renderer replaces the spike path |

**PR #21 is not merged.** Merge it before starting T26, which builds directly on
the renderer it adds.

**Next task: T26** (`docs/plan/02-m0-tasks.md:50`) — device-lost fake harness +
recovery, WARP fallback selection, per-monitor DPI change handling. Depends on
T25. `TerminalRenderer::ensureResources` already resets every resource when
`rhi()` changes and re-creates lazily, so the recovery *path* exists; T26 owes
the fake-lost harness that proves it, plus the DPI script.

## What landed

**T23 — shaper (`src/render/shaper/`).** A Qt-free `krait-shaper` static library,
so the plain Catch2 binary exercises real HarfBuzz shaping with no
QGuiApplication. Run splitting breaks a row on shaping attrs, **script**, and
unwritten cells. The script rule is the one a naive splitter skips: HarfBuzz
shapes one script per buffer and guesses it from the first character, so
`user@host:~$ สวัสดี` in one buffer is shaped as Latin and every Thai mark loses
its positioning.

**T24 — font stack (`src/render/shaper/fontdb.*`).** DirectWrite answers *which
file* covers some text; FreeType still does every raster. Fallback is per RUN,
not per row, so a mixed Latin/Thai line keeps the user's font for the prompt.

**T25 — renderer (`src/render/atlas/`, `src/render/frame_builder.*`,
`src/app/terminal_item.*`).** Two pipelines: solid rects then textured glyph
quads. The atlas uses uniform slots with LRU eviction and height-only growth.
`FrameBuilder` caches instances per row and rebuilds only damaged rows.

## Verified API facts — do NOT re-derive these

Each of these changed the design, and each is easy to get wrong from memory:

- **FreeType allows ONE `FT_Face` on one thread at a time**, and even sharing an
  `FT_Library` needs a mutex around face create/destroy. Hence a whole private
  stack per worker.
- **`hb_ft_font_create_referenced` already installs the FreeType funcs.** Calling
  `hb_ft_font_set_funcs` after it builds HarfBuzz its OWN `FT_Face` and discards
  yours. Do not add it.
- **`size->metrics.max_advance` is the max over EVERY glyph**, so one CJK or
  powerline glyph inflates it. Cell width comes from a real ASCII glyph's
  advance. (`src/render/spike/glyph_atlas.cpp` still uses max_advance — spike
  only.)
- **`GetFirstMatchingFont` takes (weight, STRETCH, style)** while
  **`MapCharacters` takes (weight, style, stretch)**. All enums, so a swap
  compiles silently and only shows as the wrong face.
- **`DWriteCreateFactory`'s out-param is `IUnknown**`** — `IID_PPV_ARGS` does not
  type-check; the `reinterpret_cast` is the documented form.
- **FreeType has NO COLRv1 rendering.** Colour emoji is out of scope, not
  half-done: a non-grayscale pixel mode returns false. Segoe UI Emoji resolves as
  a fallback face but renders monochrome.
- **`QQuickRhiItemRenderer::update()` asks for a re-RENDER, not a re-SYNCHRONIZE.**

## Watch out

- **`render()` does NOT run with the GUI thread blocked; `synchronize()` does.**
  Mutating the grid, atlas or frame vectors from `render()` races the GUI thread
  and crashed only in a release build. Anything that touches item state belongs
  in `synchronize()` or on the GUI thread.
- **Never shape per row.** `FrameBuilder`'s callback is per row; shaping inside
  it costs one blocking pool round trip per row and made the bench miss its
  60 s watchdog entirely. Use `rowNeedsRebuild()` to pre-split every damaged row,
  then shape the frame in ONE `shapeAll`.
- **A bench that does not log its churn is not evidence.** The flood harness
  reported ~2900 fps while re-drawing one static frame. `bench: step N ...
  rowsRebuilt 63` exists to make that visible; keep it.
- **Do not run clang-format on a CMakeLists.txt.** It mangles it into a parse
  error. (Done once here, recovered with `git checkout`.)
- **Python heredoc edits bypass the clang-format hook** — run `clang-format -i`
  on anything patched that way, or CI fails on formatting.
- The shaped-run cache bound is an aggregate BYTE budget, not an entry count and
  not a codepoint cap. A codepoint cap silently stopped caching Thai past ~170
  columns, because a run holds up to `cols x kMaxClusterLen` = 3840 codepoints.

## Evidence

| Gate | Result |
|---|---|
| `cmake --build --preset dev` | pass, no warnings beyond known Qt D9025 |
| `ctest --preset dev` | **125/125** (was 89 at T22) |
| clang-format | clean |
| clang-tidy, gating set (`bugprone-*`,`concurrency-*`) | clean |
| T25 release flood, 60 fps budget | **PASS** — 140.7 fps dev GPU, 139.9 fps WARP, p99 8.7 ms |
| T25 flood vs M0, WARP | **PASS** — 1.7x faster CPU, 2.4x faster GPU |
| T25 flood vs M0, dev GPU | **FAIL as measured** — 140.7 vs 180.1 fps; M0's number was vsync-capped at the 180 Hz display, so re-measure both with vsync off before calling it a regression |
| T25 Debug flood | 51.3 fps — under 60, but Debug is not the release gate |

Benches: `bench/baselines/t23-shaper.json`, `bench/baselines/t25-renderer.json`.
A RelWithDebInfo build lives in `build/rel` (configure with
`cmake --preset dev -B build/rel -DCMAKE_BUILD_TYPE=RelWithDebInfo`); there is
still no release *preset*, which is worth adding since render.md's budgets are
release gates.

## Open, not blocking

- **`src/core/grid/scrollback.cpp:47` calls `shrink_to_fit()` in the
  continuation-append hot path.** Past the 200k-cell cap every push reallocates
  ~4 MB, and one T21 test burns 41 of the suite's 44 seconds. Pre-existing from
  T21 (`733b4c3`), confirmed not a regression from this branch. Deserves its own
  ticket before scrollback sits in front of live ConPTY output.
- Curly/dotted/dashed underlines draw as a single line; the styles are parsed and
  stored (T17), so it is a pure render upgrade.
- Fallback is ONE hop, and the whole run is re-shaped with the mapped face rather
  than only the sub-span MapCharacters covered.
- Selection rects exist but clipboard copy does not — that is T27.
- Golden-image gate: the atlas PNG dump (`KRAIT_ATLAS_DUMP`) is in place and was
  produced, but there is no committed reference image or comparison step yet.

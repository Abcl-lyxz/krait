# STATE

Phase: **M1 COMPLETE** — T17-T35 done. **Next task: M2** (SSH backend), once
PR #22 is merged.

## Now

Everything from T26 to T35 is on `t26-device` as **PR #22**:

| Commit | Task |
|---|---|
| `e12584e` | T26 + T27 — device-lost harness, WARP selection, DPI; the real input path |
| `42f80f9` | T28 — paste guard and the per-tab banner |
| `fe466f0` | T29 — IME composition, measured in cells |
| `3bf9719` | T30 — settings registry v1 |
| `a041e3f` | T31 + T34 — settings reach the subsystems; the config has a home |
| `2bdca53` | T33 — backend errors as per-tab banners |
| `7c16829` | T32 — EN and TH ship together |
| _this one_ | T35 — M1 wrap |

**PR #22 is not merged.** Merge it before starting M2.

## What landed in M1's second half

**T26 — device robustness.** `render::GpuResources` exists so the device-lost
path can be TESTED: a `QQuickRhiItemRenderer` takes its QRhi from Qt Quick and
cannot be handed a fake one. The harness runs on the QRhi **Null backend**.
DPI was broken outright — the shaders divided by the item's LOGICAL size while
the render target is `size * dpr` — and the fix takes the viewport from
`renderTarget()->pixelSize()` where the uniform is written, so the two agree by
construction.

**T27 — input.** `krait-input` is Qt6::Core-only so the keymap table is testable
with no window, pty or session. Mouse reporting needed core state that did not
exist: DECCKM, `?1000/?1002/?1003`, `?1006`.

**T28 — paste guard.** The clipboard is remote input. C0 stripped, the
bracketed-paste END marker neutralised, risk ORDERED not accumulated.

**T29 — IME.** Positioning is a free function over `FaceMetrics`, so a font or
DPI change cannot move the glyphs without moving the candidate window too.

**T30/T31/T34 — settings.** One declaration in `schema.cpp`; the dotted id IS
the TOML path. Resolution order: `KRAIT_CONFIG_DIR`, then a `krait.portable`
marker, then `%APPDATA%`.

**T32 — locales.** A test reads the `.ts` files and fails when a string lands
without Thai. **T33 — errors.** `PeerClosed` joins the taxonomy; the ConPTY
reader loop now tells a dead pty from a shell that ran `exit`.

## Verified API facts — do NOT re-derive these

Carried forward from T23-T25 (FreeType one-face-per-thread;
`hb_ft_font_create_referenced` already installs the funcs; `max_advance` is the
max over EVERY glyph; `GetFirstMatchingFont` vs `MapCharacters` argument order;
`DWriteCreateFactory` needs the `reinterpret_cast`; FreeType has no COLRv1;
`QQuickRhiItemRenderer::update()` asks for a re-RENDER), plus:

- **There is NO `QWindow::devicePixelRatioChanged` signal.** The per-monitor DPI
  hook for an item is `itemChange(ItemDevicePixelRatioHasChanged)`.
  `QWindow::screenChanged` fires only when the window MOVES, not when the same
  monitor is rescaled.
- **`ItemSceneChange` is not a DPI change**, but it is where the initial ratio
  comes from: an item born on a 200% monitor never receives a CHANGE event. It
  also arrives BEFORE `componentComplete()`, so `anchors.fill: parent` has not
  been applied and the item is still 0x0 — which spawned a 2x2 pseudoconsole
  until it was guarded.
- **An unconnected `QQuickWindow::sceneGraphError` makes Qt show a MESSAGE BOX
  and terminate.** `ui.md` bans app-modal surfaces, so the slot is mandatory,
  and it must be connected before `show()`.
- **QRhi's FRONTEND rejects an empty `QShader`** ("Empty shader passed to
  graphics pipeline") before any backend sees it — including the Null backend.
  A stub shader cannot get a pipeline created.
- **Catch2 runs a test binary at BUILD time to enumerate cases**, so a binary
  needing Qt DLLs fails the build with "Error listing tests from executable"
  long before ctest runs. `catch_discover_tests(... DL_PATHS ...)` is the fix.
- **vcpkg's tomlplusplus is prebuilt WITH exceptions**, and `TOML_EXCEPTIONS=0`
  moves the library into a different inline namespace (`toml::v3::noex`), so
  linking its target while asking for the no-exceptions API fails with an
  unresolved `toml::v3::noex::parse`. The port also sets its header-only choice
  as compile OPTIONS, which land after ours. Use the include path only.
- **lupdate cannot see through an indirection.** A local `translate(...)` lambda
  wrapping `QCoreApplication::translate` hides every literal from extraction —
  it cost eight strings in `error_banner.h`, and it was silent.
- **toml++'s `value<T>()` is not strict**: it returns `true` for
  `ligatures = 3`. Check `is_boolean()`/`is_integer()`/`is_string()` first.

## Watch out

- **`render()` does NOT run with the GUI thread blocked; `synchronize()` does.**
- **Never shape per row.** Pre-split every damaged row, then shape the frame in
  ONE `shapeAll`.
- **A bench that does not log its churn is not evidence.** `rowsRebuilt 63` is
  what proves the flood ran.
- **Do not run clang-format on a CMakeLists.txt.**
- **Python heredoc edits bypass the clang-format hook** — run `clang-format -i`
  on anything patched that way.
- The shaped-run cache bound is an aggregate BYTE budget.
- **Destroying a QRhi before the resources built on it is undefined**, and it
  made the suite fail intermittently rather than reproducibly. Qt's contract is
  release resources, THEN destroy the device.
- **A short atlas upload must stay pending.** `takeGrew()` consumes the flag, so
  a frame can report a taller atlas with `atlasGrew` already false, and clearing
  the pending flag left the new half of the atlas blank for the session.

## Evidence

| Gate | Result |
|---|---|
| `cmake --build --preset dev` | pass |
| `cmake --build --preset release` | pass (T35 added the preset) |
| `ctest --preset dev` | **196/196** (was 125 at T25) |
| `ctest --preset release` | **196/196** |
| `tests\fuzz\run-smoke.cmd` | 60 s, zero crashes, 271 new units |
| core-standalone (sacred rule 1) | builds with no Qt, no vcpkg toolchain |
| clang-format, whole tree | clean |
| clang-tidy, changed files | clean |
| `tools\vttest-check.cmd` | corpus green, ledger covers every corpus area |
| `tools\dpi-check.cmd` | **PASS** — 20px cell 12x23 at 100%, 40px cell 23x46 at 200% |
| Release flood, WARP, 60 fps budget | **PASS** — >=180 fps (vsync-bound), cpu 5.56 ms |
| Release flood vs T25, WARP | **PASS** — 139.9 -> >=180 fps, cpu 7.15 -> 5.56 ms |
| Locales | lupdate 21 strings, lrelease 21 finished / 0 unfinished, both load |
| `cpp-reviewer`, whole branch | 3 blocking + 11 others, all fixed on this branch |

Baseline: `bench/baselines/m1-wrap.json`.

Reviewed by `cpp-reviewer` over the whole branch. Three blocking findings, all
real and all fixed here: an active IME composition suppressed every atlas upload
for that frame; the clipboard was read with no size cap on the UI thread; and
Banner.qml rendered hostile clipboard text as RICH text, so a pasted tag could
restyle the warning that was about it. Eleven more findings down to Low, also
fixed — the notable ones being stale glyph UVs after atlas growth (pre-existing
from T25), unchecked `create()` calls in the device-lost rebuild, and plain
Enter confirming a paste-guard banner that had just stolen focus.

## Open, not blocking

- **The hardware flood leg is UNMEASURED this session.** The renderer
  initialises on the RTX 4060 and then never presents a frame — the machine had
  no usable attached display (`qt.qpa.screen: Unable to open monitor interface
  to DISPLAY1`) and a D3D11 present with nowhere to go blocks. That is the
  condition T26's WARP fallback exists for, not a renderer regression, but the
  last real hardware number is still T25's 140.7 fps. **Re-measure on a machine
  with a display before M2 closes.**
- **The manual gates have not been run by a human this session.**
  `tools\ime-check.cmd`, `tools\paste-check.cmd` and `tools\backend-check.cmd`
  are written and the logic behind each is unit-tested, but the parts only a
  person can see — where the IME candidate window lands, that the banner is not
  modal, that killing conhost produces the right banner — are unverified. Same
  reason as above: no attached display.
- **The M1 daily-drive checklist (01-milestones.md) has not been run.** Ten
  items, all needing a person at a terminal.
- `src/core/grid/scrollback.cpp:47` calls `shrink_to_fit()` in the
  continuation-append hot path; one T21 test burns most of the suite's runtime.
  Pre-existing from T21, deserves its own ticket.
- Curly/dotted/dashed underlines draw as a single line; parsed and stored since
  T17, so it is a pure render upgrade.
- Font fallback is ONE hop, and the whole run is re-shaped with the mapped face.
- Golden-image gate: the atlas PNG dump exists (`KRAIT_ATLAS_DUMP`) but there is
  still no committed reference image or comparison step.
- **The settings UI does not exist.** The registry, schema, EN+TH search
  keywords and hot reload are all in place and the file is hand-editable; the
  QML settings page and command palette `ui.md` describes are M2 work.
- No migrations exist yet — `kSchemaVersion` is 1 and nothing has been renamed.
  The mechanism and the future-version refusal are tested; the first real
  migration will be the first exercise of the table.

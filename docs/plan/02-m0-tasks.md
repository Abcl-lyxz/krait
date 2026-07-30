# M0 + M1 task breakdown

Tasks are ≤ half a day, dependency-ordered (a task may start when the tasks in
its **Needs** column are done). Every task ends with its verification command
green and, where code changed, tests in the same commit. Governing rules load
automatically by path; listed for orientation.

Toolchain facts the tasks rely on (verified 2026-07-29, ledger in
00-architecture.md): Qt 6.11.1 · vcpkg ports libssh 0.12.0 / harfbuzz 14.2.1 /
freetype 2.14.3 / utf8proc 2.11.3 / tomlplusplus 3.4.0 / fmt 12.2.0 /
catch2 3.15.3 / benchmark 1.9.5 · QQuickRhiItem since Qt 6.7 · clang-cl for
libFuzzer.

## M0

| # | Task | Needs | Files | Verify | Rules |
|---|---|---|---|---|---|
| T1 | `git init`; add `LICENSE` (MIT, copyright "Krait contributors"); `.gitignore` (build/, .vs/, .claude/settings.local.json, .claude/.cache/); `.gitattributes` (eol=lf except .cmd); README stub (one paragraph + build TL;DR); commit; `gh repo create --public` + push | — | LICENSE, .gitignore, .gitattributes, README.md | `git log --oneline` shows commit; repo URL opens | — |
| T2 | `CMakeLists.txt` root + `CMakePresets.json` (`dev`: Ninja, MSVC cl, `/std:c++23preview /W4 /WX /utf-8`, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, vcpkg toolchain) + `vcpkg.json` manifest (catch2, fmt, utf8proc; pinned `builtin-baseline`) + `vcpkg-configuration.json`. Update SETUP.md with Qt 6.11.1 aqtinstall line | T1 | CMakeLists.txt, CMakePresets.json, vcpkg.json, SETUP.md | `cmake --preset dev` succeeds | cpp.md |
| T3 | `src/core/` skeleton: `krait-core` static lib (empty api header + version fn), **standalone proof**: `tests/core-standalone/CMakeLists.txt` compiles krait-core with `CMAKE_DISABLE_FIND_PACKAGE_Qt6` and no Qt on path; Catch2 wiring (`catch_discover_tests`); one smoke test | T2 | src/core/*, tests/unit/*, tests/core-standalone/* | `ctest --preset dev` (1 test green) | cpp.md, vt-core.md |
| T4 | UTF-8 decoder: incremental, malformed→U+FFFD per WHATWG replacement rules; feeds codepoints to the parser layer. Corpus harness v0: golden files `tests/corpus/*.case` (format: `IN <hex/escaped bytes>` / `EXPECT <event list>`) + runner | T3 | src/core/unicode/utf8.{h,cpp}, tests/corpus/harness.cpp, tests/corpus/utf8/*.case | `ctest --preset dev -R corpus` | vt-core.md |
| T5 | Parser state machine: the 14 states / 14 actions of vt100.net/emu/dec_ansi_parser as generated tables (`tools/gen-parser-tables.py` or constexpr); documented deviations: UTF-8 outside machine, 0x3A subparam-legal in CSI param state, C1 policy flag, param cap 32. Dispatch → `ParserEvents` interface (print/execute/csi/esc/osc/dcs hooks) | T4 | src/core/parser/{machine,tables,events}.*, tests/corpus/parser/*.case | corpus: interrupted-mid-sequence + garbage cases green | vt-core.md |
| T6 | C0 controls (BEL BS HT LF CR SO SI) + CSI cursor family (CUU CUD CUF CUB CUP HVP CHA VPA) on a stub grid; conformance.md rows flip ✗→✔; fuzz seeds per sequence | T5 | src/core/parser/csi_cursor.*, docs/conformance.md, tests/corpus/csi/*.case, tests/fuzz/seeds/* | corpus green; conformance rows updated same commit | vt-core.md |
| T7 | SGR basic (0–29, 30–49, 90–107; colon-subparam tolerant skeleton) + ED/EL; attributes in cell struct | T6 | src/core/parser/sgr.*, src/core/grid/cell.h, tests/corpus/sgr/*.case | corpus green | vt-core.md |
| T8 | Grid: logical lines + wrap points + cursor + damage list; resize-reflow test scaffold (resize during wrapped line / wide char at boundary / active prompt — cases marked `[!mayfail]` until M1 reflow lands) | T5 | src/core/grid/{grid,line,damage}.*, tests/unit/grid_*.cpp | `ctest -R grid` | vt-core.md |
| T9 | Capability table + honest replies: DA1, DSR 5/6 generated from the table gating implementation; parser answerback rate-limit hook | T6 | src/core/caps/*, tests/corpus/reports/*.case | corpus: DA1 reply matches table exactly | vt-core.md |
| T10 | Fuzz target `tests/fuzz/parser_fuzz.cpp` (bytes→parser→grid, ASSERTs on invariants); clang-cl build preset `fuzz`; seed corpus = all corpus inputs; `run-smoke.cmd` (60 s) | T7,T8 | tests/fuzz/*, CMakePresets.json | `tests\fuzz\run-smoke.cmd` zero crashes | vt-core.md, ADR-0010 |
| T11 | Qt shell: `src/app/main.cpp` + one QML window; `QQuickWindow::setGraphicsApi(D3D11)`; QQuickRhiItem subclass drawing a colored triangle (proves qsb pipeline via `qt_add_shaders`) | T2 | src/app/main.cpp, src/app/qml/Main.qml, src/render/spike/*, src/render/shaders/* | app opens; log line `rhi backend: D3D11` | ui.md, render.md |
| T12 | Spike renderer: R8 atlas texture (ASCII glyphs pre-rasterized via FreeType — add freetype to manifest), instanced quads for 240×63 grid, per-cell fg/bg from a uniform/storage buffer | T11 | src/render/spike/*, vcpkg.json | grid of glyphs visible | render.md |
| T13 | Flood bench: synthetic full-grid-change generator + frame timer (QRhi timestamps + CPU clock); `flood-report.cmd` emits fps/ms table; run on dev GPU **and** `QRhiD3D11InitParams`-forced WARP; record `bench/baselines/m0-spike.json` | T12 | bench/spike/*, bench/baselines/m0-spike.json | report emitted with both adapters | render.md |
| T14 | Go/no-go: compare numbers to gate (≥60 fps 4K dev GPU, ≥30 fps WARP, <10 ms/frame); write ADR addendum `docs/decisions/0013-m0-spike-verdict.md` | T13 | docs/decisions/0013-*.md | ADR exists, numbers cited | — |
| T15 | ConPTY backend: acquire pinned OpenConsole build (`third_party/openconsole/` + MS LICENSE + VERSION.md per ADR-0011); `IBackend` + `ConptyBackend` (CreatePseudoConsole, per-pipe threads, resize); wire backend→parser→grid→spike renderer + keyboard input | T9,T12 | src/net/ibackend.h, src/net/error.h, src/net/conpty/*, third_party/openconsole/* | manual: type `dir` in PowerShell through Krait | net.md |
| T16 | CI fast gate: `.github/workflows/ci.yml` — windows-latest, install-qt-action (6.11.1, cache:true), vcpkg cache, build, ctest, clang-format check, clang-tidy on changed files, core-standalone build; branch protection on | T3 | .github/workflows/ci.yml | green run on GitHub | — |

M0 done = M0 acceptance block in 01-milestones.md passes. **M0: DONE.**

## M1 — DONE (2026-07-30, T17-T35)

| # | Task | Needs | Files | Verify | Rules |
|---|---|---|---|---|---|
| T17 | SGR extended: 38/48 truecolor+256 (colon + semicolon forms), 58/59, 4:x underline styles; cell attr storage widened | T7 | src/core/parser/sgr.*, cell.h, corpus | corpus incl. colon/semicolon ambiguity cases | vt-core.md |
| T18 | Erase/scroll completion: DECSTBM, IL DL SU SD, origin mode; alt screen 1049 | T8 | src/core/{parser,grid}/*, corpus | corpus + vttest screen 1/2 goldens | vt-core.md |
| T19 | Grapheme/width engine: utf8proc clustering + generated width tables (`tools/gen-width-tables.py` from UCD 17) + VS15/16 + EAA setting; mode 2027 signal | T4 | src/core/unicode/*, tools/gen-width-tables.py, tests/unit/width_*.cpp | width tests incl. farmer-emoji=2, Thai clusters | vt-core.md |
| T20 | Reflow: resize re-wraps logical lines; T8's `[!mayfail]` cases now must pass; wide-char boundary + prompt cases | T18,T19 | src/core/grid/reflow.*, tests | `ctest -R reflow` all green | vt-core.md |
| T21 | Scrollback: ring of logical lines + per-tab cap + viewport; damage integration | T20 | src/core/grid/scrollback.* | unit + flood keeps O(cap) memory | vt-core.md |
| T22 | Modes: 2004 bracketed paste, 2026 sync output + 150 ms guard, DECRQM honest replies wired to capability table | T9,T18 | src/core/*, corpus | corpus: DECRQM answers match table; 2026 timeout test | vt-core.md |
| T23 | Shaper: HarfBuzz+FreeType worker pool (per-worker FT_Face), run-splitting, shaped-run cache keyed (font,attrs,cluster-text) | T12,T19 | src/render/shaper/* | unit: Thai สวัสดี shapes to clusters; bench records | render.md |
| T24 | Font stack: DirectWrite enumeration + `IDWriteFontFallback::MapCharacters` chain → FT faces; Nerd Font + emoji fallback; ligature toggle | T23 | src/render/shaper/fontdb.* | unit: missing-glyph run re-shapes into fallback | render.md |
| T25 | Real renderer v1: replace spike path — damage-driven draw, atlas LRU + growth caps + eviction test, cursor styles, selection rects | T13,T23 | src/render/atlas/*, src/render/*.cpp | golden-image tests on WARP; flood ≥ M0 baseline | render.md |
| T26 | Device robustness: device-lost fake harness + recovery, WARP fallback selection, per-monitor DPI change handling | T25 | src/render/*, tests | fake-lost test green; DPI script | render.md |
| T27 | Input path: keyboard translation (incl. win32-input-mode passthrough knowledge), mouse reporting basics, clipboard copy | T15 | src/app/input/* | unit: keymap table cases | ui.md |
| T28 | Paste-guard: bracketed paste, ESC/C0 strip, multiline/`sudo` confirm banner; per-tab banner component | T27 | src/app/input/paste.*, src/app/qml/Banner.qml | unit + manual script | ui.md, net.md |
| T29 | IME: `ItemAcceptsInputMethod` + `inputMethodEvent` + `ImCursorRectangle` from renderer cell metrics; composition overlay; Thai + JP scripted tests | T25,T27 | src/render/ime_metrics.*, src/app/input/ime.* | IME demo script (01-milestones M1) | render.md, ui.md |
| T30 | Settings registry v1: schema table + TOML IO (toml++, no-exceptions mode) + hot reload (file watcher) + `/add-setting` compatibility | T2 | src/app/settings/* | unit: round-trip + migration + hot-reload test | ui.md |
| T31 | Settings→subsystems: font/size/theme/EAA/scrollback-cap wired; theme tokens (no hex in QML) | T30,T25 | src/app/*, qml | change font size in TOML → live reflow | ui.md |
| T32 | Locales: EN+TH `tr()` scaffolding, .ts files, language setting; every existing string localized | T30 | src/app/i18n/*, *.qml | lupdate/lrelease clean; TH visible in UI | ui.md |
| T33 | Per-tab error banners wired to ErrorCode taxonomy end-to-end (kill conhost test, pipe break test) | T15,T28 | src/app/qml/*, src/net/* | contract test: PeerClosed → banner, no dialog | net.md, ui.md |
| T34 | Portable mode (config next to exe) + config-dir resolution order + docs | T30 | src/app/settings/paths.* | unit: resolution order table | ui.md |
| T35 | M1 wrap: bench baselines refresh, daily-drive checklist run, vttest golden script (`tools/vttest-check.cmd`), STATE.md milestone flip | all | bench/baselines/*, tools/* | 01-milestones M1 acceptance block green | — |

## Fresh-session note

T1 needs only git + gh (no toolchain). T2 needs VS2026 (or VS2022 17.x+),
CMake ≥3.30, Ninja, vcpkg checkout, Python 3 — all listed in SETUP.md.
Qt 6.11.1 install: `aqt install-qt windows desktop 6.11.1 win64_msvc2022_64
-m qtshadertools` (exact module list finalized in T2).

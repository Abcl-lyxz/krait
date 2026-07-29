# STATE

Phase: **M0 COMPLETE** — M1 not started

## Now

M0 is done and merged. `main` is at the T16 merge, all 12 PRs landed, and
`fast-gate` is green on `main` and required for merge.

**Next task: T17** (`docs/plan/02-m0-tasks.md` §M1) — SGR extended:
38/48 truecolor+256 in colon AND semicolon forms, 58/59, 4:x underline
styles, cell attr storage widened. T7 already consumes 38/48/58 with correct
arity but keeps them inert, and collapses 4:x to on/off — those two lines in
`docs/conformance.md` are what T17 flips.

## M0 acceptance — evidence, not assertion

Every line of `docs/plan/01-milestones.md` §M0, re-run on the final tree:

| Gate | Result |
|---|---|
| `cmake --preset dev` + `--build` | pass — MSVC 19.51.36243, Qt 6.10.3, 48/48 |
| `ctest --preset dev` | pass — **22/22**, 0.48 s |
| `tests\fuzz\run-smoke.cmd` | pass — 278,238 execs, **0 crashes** |
| `bench\spike\flood-report.cmd` | pass — table emitted, both adapters |
| dev GPU 4K flood | 180.0 fps / 0.419 ms GPU — gate ≥60 fps, <10 ms |
| WARP flood | 152.4 fps / 5.14 ms GPU — gate ≥30 fps, <10 ms |
| Manual demo | pass — `dir` renders through the grid, `cls` clears to a fresh prompt at home (captured via KRAIT_TERM_INJECT + KRAIT_SPIKE_SCREENSHOT) |
| Go/no-go ADR | ADR-0013, verdict GO |
| CI fast gate | green on `main`, **5m34s** vs ADR-0008's <15 min target |

NOT verified: the parenthetical "vttest menu renders legibly" in the M0
acceptance block. vttest is not wired up; `tools/vttest-check.cmd` is M1 T35.
Do not claim it.

## Done so far

- T1 ✔ (c941753): repo live at https://github.com/Abcl-lyxz/krait, MIT, main.
- T2 ✔ (3ceac79): CMakePresets (`dev`), root CMakeLists, vcpkg.json (baseline
  9d7f79f5). Toolchain: MSVC 19.51 (VS2026 18 Community), CMake 4.2.3 + Ninja
  from VS, vcpkg at `C:\vcpkg` (`VCPKG_ROOT=C:\vcpkg` in the dev shell).
  No wrapper scripts exist in-repo — build via a VS x64 dev shell.
- T3 ✔ (0920812): `krait-core` static lib + Catch2 wiring + standalone
  zero-dep proof (`cmake -S tests/core-standalone -B build/core-standalone -G Ninja`).
- T4 ✔ (c65553e): WHATWG UTF-8 decoder + corpus harness + utf8 cases.
- T5 ✔: parser state machine — 14 states/14 actions as constexpr tables
  (`src/core/parser/{events,tables,machine}.*`), dispatching to `ParserEvents`.
  Deviations implemented: UTF-8 outside the machine, 0x3A subparam-legal in
  CSI, C1 policy flag (`acceptC1`, default off), param cap 32 / clamp 16383,
  BEL ends OSC, DEL prints in ground (display layer decides), aborted-string
  flag on `oscEnd`/`dcsUnhook` (CAN/SUB/C1 = aborted; ESC assumed ST).
  Corpus: `tests/corpus/parser/{basic,interrupted,garbage,c1}.case`, every
  case run whole-buffer AND byte-at-a-time. Harness grew `MODE c1` directive
  and event-token EXPECT format (documented in harness.cpp header).
  vt-spec-auditor watch items live in
  `.claude/agent-memory/vt-spec-auditor/t5-audit-findings.md`.

- T6 ✔: C0 controls (BEL BS HT LF CR SO SI) + CSI cursor family
  (CUU CUD CUF CUB CUP HVP CHA VPA) on `StubGrid`
  (`src/core/parser/csi_cursor.*`, 24x80, no scroll/margins/DECOM/LNM/SCS —
  partials declared in the ledger). Colon subparams and intermediates reject
  the sequence (xterm: subparams are SGR-only). `docs/conformance.md`
  existed from the scaffold as a FAMILY ledger — T6 flipped its two rows
  ✗→◐; fuzz seeds started (`tests/fuzz/seeds/`, 10 files).
  Corpus `tests/corpus/csi/{cursor,c0}.case` asserts final grid state
  (`cur:R,C` / `g1:on` / `bell:N` tokens via CursorSink).

- T7 ✔: SGR basic (`src/core/parser/sgr.*`, `src/core/grid/cell.h`
  Color/Attr/Cell) + ED/EL on the stub grid (now has cells + pen +
  putChar, no wrap). 38/48/58 consumed with correct arity (colon+legacy)
  but inert until M1; 21≈underline, 4:x collapses to on/off — declared in
  ledger. Non-SGR handlers (cursor, erase) reject colon subparams — that's
  now a PATTERN both reviewers enforce; keep it for every new CSI family.
  Corpus `tests/corpus/sgr/{basic,erase}.case`.

- T8 ✔: real Grid (`src/core/grid/{grid,line,damage}.*`) REPLACED StubGrid
  everywhere — row storage with explicit `wrappedFromPrev` wrap flags
  (reflow-from-day-one), DEC deferred wrap (`pendingWrap`; cleared by
  BS/HT/CR/LF/cursor-family AND by EL/ED per DEC STD 070+xterm ResetWrap —
  spec-audit catch), erase-to-EOL severs the wrap join to the next row,
  scroll into 10k-capped deque scrollback, per-row coalesced DamageList,
  naive resize clamped to 1x1 (rows shrink feeds top into scrollback; no
  rewrap until M1 — 3 `[!mayfail]` scaffolds in tests/unit/grid_test.cpp
  document desired reflow). Watch item for new sequence work: every op
  touching the cursor line must decide `pendingWrap` explicitly
  (ICH/DCH/IL/DL when they come).

- T9 ✔: capability table + honest replies (`src/core/caps/caps.*`) — DA1
  generated from the table (VT100+AVO `?1;2c`; VT220 identity only when a
  real VT220 feature flips), DSR 5 OK + DSR 6 CPR; DA2/DECXCPR/?-DSR =
  honest silence. ReplyLimiter: clock-free token bucket, 8 replies per 256
  INPUT bytes — integrator must call addInput(chunk) before each feed
  (contract documented in caps.h; harness wires it that way). Corpus
  `tests/corpus/reports/basic.case` incl. refill-across-window burst and
  8-bit C1 request. Watch item (M1): ansiColor deliberately under-claimed
  until the 62-identity is honest.

- T10 ✔: fuzz target (`tests/fuzz/parser_fuzz.cpp` — FuzzSink with cursor/
  params/amplification invariant asserts, both C1 policies by input parity),
  presets `fuzz` (clang-cl+ASan, ADR-0010 primary — **BROKEN, see Debts**)
  and `fuzz-msvc` (the verified path). Seeds: `tools/extract-seeds.mjs`
  (node) extracts every corpus IN payload byte-exactly (latin1 read) +
  tests/fuzz/seeds → 189 files. `tests/fuzz/run-smoke.cmd` = the per-commit
  gate; replays tests/fuzz/regressions/ first (empty). RelWithDebInfo
  override keeps asserts live in fuzz builds.

- T11 ✔: Qt shell + D3D11 triangle spike. `krait-app` (console subsystem
  for M0 so gates read stdout): main.cpp sets D3D11, loads Krait/Main.qml;
  `Triangle` = QQuickRhiItem spike (src/render/spike) with qsb shaders via
  qt_add_shaders; pipeline-create failure degrades to clear-only. VERIFIED:
  `rhi backend: D3D11` logged, autoquit hook KRAIT_SPIKE_AUTOQUIT. Qt
  DEVIATION: pin 6.11.1 not installable via aqt — built against 6.10.3 +
  qtshadertools module, floor 6.8, `QT_ROOT` env feeds CMAKE_PREFIX_PATH
  (SETUP.md). GuiPrivate linked for rhi/qrhi.h; QML module SOURCES +
  include dir needed for qmltyperegistrations. sceneGraphInitialized signal
  unreliable cross-thread — backend logged via 1s singleShot instead.

- T12 ✔: glyph-atlas spike renderer. FreeType (vcpkg) rasterizes ASCII
  into an R8 atlas (`src/render/spike/glyph_atlas.*`); `SpikeGrid`
  QQuickRhiItem draws 240×63 instanced quads, per-cell glyph+fg+bg in a
  DYNAMIC PER-INSTANCE BUFFER (bench-equivalent deviation from the plan's
  storage-buffer wording — ratified by T13/T14 numbers). VERIFIED via
  screenshot (KRAIT_SPIKE_SCREENSHOT hook). HARD-WON QRhi FACTS:
  (1) first initialize() runs BEFORE first synchronize() — create
  resources lazily, re-attempt from render(); (2) cb->setShaderResources()
  after setGraphicsPipeline or all bindings read zeros (black screen);
  (3) KRAIT_SPIKE_ATLAS_DUMP env dumps the atlas PNG for debugging.

- T13 ✔ (PR #9): flood bench. KRAIT_BENCH/_4K/_WARP env drive the spike;
  `bench/spike/flood-report.cmd`; baseline `bench/baselines/m0-spike.json`
  (machine HomeCenter, RTX 4060). WARP selected via
  QQuickGraphicsConfiguration::setPreferSoftwareDevice; timestamps via
  setTimestamps — BOTH only take effect if set BEFORE first expose, hence
  Main.qml visible:false + show-after-config in main().

- T14 ✔ (PR #10): ADR-0013 verdict = **GO**, all gates cleared with margin.

- T15 ✔ (PR #11): ConPTY backend + full wiring — KRAIT IS A TERMINAL.
  `third_party/openconsole/` = REPACKAGED Microsoft.Windows.Console.ConPTY
  v1.24 nupkg (ADR-0011; conpty.dll + OpenConsole.exe + conpty.h + MS
  LICENSE + VERSION.md pin). `src/net/{ibackend,error}.h`,
  `src/net/conpty/` (bundled-dll Conpty* exports, reader/writer threads,
  3s-timeout stop). `src/core/terminal/session.*` = parser+grid+handlers
  behind feed(). `src/app/terminal_item.*` = TerminalView (keyboard map,
  resize, cursor inversion). HARD-WON: STARTF_USESTDHANDLES with NULL std
  handles is MANDATORY — without it the child bypasses the pty through
  inherited std pipes (prompt leaked to parent console).

- T16 ✔ (PR #12): CI fast gate — `.github/workflows/ci.yml`, ADR-0008 shape
  exactly: configure+build → ctest → krait-core standalone zero-dep proof →
  60 s fuzz smoke → clang-format → clang-tidy on changed files. Green on
  `main` in 5m34s. Branch protection ON: `fast-gate` required + strict, no
  force-push, no deletion, `enforce_admins` deliberately OFF (solo owner
  needs an escape hatch; the local guard hook already blocks direct commits).
  ENVIRONMENT FACTS THAT COST RUNS — do not re-derive:
  * `install-qt-action@v4` exports `QT_ROOT_DIR`, **not** `QT_ROOT`, and only
    `Qt5_DIR`, never `Qt6_DIR`. The image exports `VCPKG_INSTALLATION_ROOT`,
    not `VCPKG_ROOT`. Both bridged into `GITHUB_ENV`.
  * Pinning `builtin-baseline` is NOT enough on a runner: vcpkg reads the
    version DATABASE (`versions/`) off disk and only `baseline.json` from the
    commit, so a 2026 baseline against the image's 2024 `versions/` tree dies
    with "no version database entry for <port> at <date>". CI checks the whole
    registry out at the baseline.
  * vcpkg's `x-gha` binary-cache backend was REMOVED upstream in 2025 —
    `actions/cache` over `%LOCALAPPDATA%\vcpkg\archives` is what remains.
  * The `windows-2025-vs2026` label resolves cl to MSVC 14.51.36231, matching
    the dev machine. Plain `windows-latest` is reportedly still VS2022 17.14,
    which contradicts ADR-0008's premise — see Open questions.

## Environment debts (blocked on the owner, or on real work)

1. **ADR-0010's primary fuzz preset does not link — and cannot as written.**
   Not merely "untested" any more; CI executed it and produced the diagnosis.
   CMake drives an MSVC-like toolchain through `lld-link` **directly** rather
   than through the compiler driver, so `tests/fuzz/CMakeLists.txt:10`'s
   `target_link_options(parser-fuzz PRIVATE -fsanitize=fuzzer,address)` is
   discarded — `lld-link: warning: ignoring unknown argument` — and every
   `__asan_*` / `__sanitizer_cov_*` symbol comes back undefined. The fix is to
   name the `clang_rt` import libraries explicitly
   (`clang_rt.asan_dynamic-x86_64.lib`, `clang_rt.fuzzer-x86_64.lib`, and the
   thunk) plus their `lib/clang/<ver>/lib/windows` directory.
   `run-smoke.cmd` now defaults to `fuzz-msvc` instead of auto-selecting the
   broken preset whenever LLVM appears on PATH; `KRAIT_FUZZ_PRESET=fuzz` opts
   back in. **This means the tool's default now contradicts ADR-0010's stated
   primary/fallback order** — decide whether to fix the linking or write a
   superseding ADR. Do not leave it implicit.
2. **Qt 6.11.1 pin is not installable and now provably won't be soon.**
   Qt 6.11+ moved to a per-arch repo layout; aqtinstall support is merged
   upstream but UNRELEASED (newest on PyPI is 3.3.0, 2025-06-02), and
   install-qt-action v4 pins `aqtversion: ==3.3.*`. So this blocks CI as well
   as the dev machine. Everything builds on 6.10.3. To move: official Qt
   installer locally, then flip `QT_ROOT`, `QT_VERSION` in ci.yml, SETUP.md
   and the plan docs together. If you bump before aqt >3.3.0 ships, CI also
   needs `aqtsource: 'git+https://github.com/miurahr/aqtinstall'`.
3. clang-cl IS now exercised on CI (LLVM 20.1.8 is on the runner), so debt 1
   no longer needs a local install to make progress. `winget install
   LLVM.LLVM` (admin) is still wanted for local ASan work.

## Known-not-fixed (deliberate, with reasons)

- **`bench/baselines/m0-spike.json` is stale in the favourable direction.**
  Measured this session: WARP 152.4 fps / 5.14 ms vs 80.7 fps / 10.07 ms
  recorded; cpu_avg halved. Reproducible across two runs, so not noise —
  likely display/vsync conditions (both runs log `qt.qpa.screen: "Unable to
  open monitor interface to \\.\DISPLAY1"`, and the baseline notes say
  cpu_*_ms include vsync pacing). Harmless for M0 (favourable), but it means
  M1's "no >5% regression" gate is calibrated ~1.9x too slow — a real 40%
  regression would pass. M1 T35 refreshes baselines; it must actually happen.
- **14x `D9025: overriding '/std:c++17' with '/std:c++23preview'`** in
  `krait-app`. `/WX` cannot catch these: it promotes C-series compilation
  warnings, not D-series command-line ones, so exit 0 does not mean clean.
  Root cause is NOT `qt_standard_project_setup` (the Qt 6.11 docs list what it
  sets — AUTOMOC/AUTOUIC, GNUInstallDirs, RPATH, USE_FOLDERS, policies — and
  the C++ standard is not among them); it is Qt's `cxx_std_17` INTERFACE
  compile feature propagating through `Qt6::Core`, which CMake satisfies with
  `-std:c++17` because the project sets its standard with a raw
  `add_compile_options(/std:c++23preview)` flag CMake knows nothing about.
  `/std:c++23preview` does win — that is what D9025 says — so the build is
  correct. The real fix is a project-wide decision about adopting
  `CMAKE_CXX_STANDARD`, which touches the root lists and both fuzz presets.
  Evidence lives in `build/dev/build.ninja` (14 hits), NOT in CMakeCache, and
  the flag is dash-form `-std:c++17` so a `/std:` grep misses it.
- `src/render` still has no dedicated CMake target (promised at T12);
  `src/app` compiles the render sources directly.

## Open questions (non-blocking)

- Code-signing certificate vendor + timing (needed by M6, decide by M4).
- ADR-0008 states `windows-latest` is Server 2025 + VS2026 18.4 "verified
  runner-images #14017". Live docs say VS Enterprise 2022 17.14 with a
  separate `windows-2025-vs2026` label. One is stale; CI does not depend on
  the answer (it names the label explicitly) but the ADR should be corrected.
- clang-tidy config gaps from T5's preflight: `/Zc:preprocessor` triggers a
  driver-arg complaint (CI passes `-Wno-unknown-argument`
  `-Wno-unused-command-line-argument`), and `cppcoreguidelines-avoid-c-arrays`
  still fires though its modernize alias is disabled. Non-gating; tidy's error
  set (bugprone/concurrency) is clean and IS gating in CI.

## Watchouts

- Follow task order; interview decisions live in ADRs 0004–0008 — don't re-ask.
- `.claude/settings.local.json` and `.claude/.cache/` are machine-local; never commit.
- clang-tidy must run from the VS dev shell (needs `INCLUDE`); outside it,
  MSVC headers don't resolve and findings are parse garbage.
- The parser's `entry.next == m_state` early-return skips entry actions on
  same-state table transitions — currently provably harmless, but re-verify
  if you ever add a same-state transition with an entry action.
- **`main` is protected on GitHub now, not just by the local hook.** Every
  change needs a PR with `fast-gate` green, including doc-only ones. Do NOT
  add `paths-ignore` to the workflow to speed those up: a required check that
  is skipped stays pending forever and the PR becomes unmergeable.
- **Merging a stacked PR chain:** GitHub only auto-retargets when the base
  branch is DELETED on merge. With branches kept, retarget each to main first
  (`gh pr edit N --base main`) or you merge into the predecessor branch.
- **In batch scripts, never `if errorlevel 1`.** cmd compares SIGNED, so it is
  false for negative exit codes — a failed link reported as `[code=4294967295]`
  (-1) walked straight past it in `run-smoke.cmd` and the gate kept going with
  no binary built. Use `if %ERRORLEVEL% neq 0`, and never inside a
  parenthesised block (it expands at parse time, before the command runs).
- In `pwsh` CI steps, a native command's non-zero exit does NOT throw
  mid-script. Any `git`/tool call whose empty output is treated as "nothing to
  do" must check `$LASTEXITCODE` first, or the step goes green having done
  nothing.
- First session on a new machine: follow `SETUP.md`.

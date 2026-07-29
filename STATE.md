# STATE

Phase: M0 in progress

## Now

T5 + T6 are done, stacked as PRs (main is hook-protected, see Watchouts):
PR #1 = t5-parser (parser machine), PR #2 = t6-cursor stacked on it.
Merge #1 then #2. The `ParserEvents` seam and the T6 `StubGrid` are what T7
builds on; the stub grid dies in T8 when the real grid lands.

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
  cpp-reviewer: clean (both findings fixed in-commit). vt-spec-auditor: no
  undocumented deviations; its watch items live in
  `.claude/agent-memory/vt-spec-auditor/t5-audit-findings.md`.
  /preflight GREEN (tidy note below).

- T6 ✔: C0 controls (BEL BS HT LF CR SO SI) + CSI cursor family
  (CUU CUD CUF CUB CUP HVP CHA VPA) on `StubGrid`
  (`src/core/parser/csi_cursor.*`, 24x80, no scroll/margins/DECOM/LNM/SCS —
  partials declared in the ledger). Colon subparams and intermediates reject
  the sequence (xterm: subparams are SGR-only). `docs/conformance.md`
  existed from the scaffold as a FAMILY ledger — T6 flipped its two rows
  ✗→◐; fuzz seeds started (`tests/fuzz/seeds/`, 10 files, no harness yet —
  that's a later task). Corpus `tests/corpus/csi/{cursor,c0}.case` asserts
  final grid state (`cur:R,C` / `g1:on` / `bell:N` tokens via CursorSink).
  Both reviewers clean after fixes; ctest 4/4; format+tidy clean.

- T7 ✔: SGR basic (`src/core/parser/sgr.*`, `src/core/grid/cell.h`
  Color/Attr/Cell) + ED/EL on the stub grid (now has cells + pen +
  putChar, no wrap). 38/48/58 consumed with correct arity (colon+legacy)
  but inert until M1; 21≈underline, 4:x collapses to on/off — declared in
  ledger. Non-SGR handlers (cursor, erase) reject colon subparams — that's
  now a PATTERN both reviewers enforce; keep it for every new CSI family.
  Corpus `tests/corpus/sgr/{basic,erase}.case`; ctest 5/5.

- T8 ✔: real Grid (`src/core/grid/{grid,line,damage}.*`) REPLACED StubGrid
  everywhere — row storage with explicit `wrappedFromPrev` wrap flags
  (reflow-from-day-one), DEC deferred wrap (`pendingWrap`; cleared by
  BS/HT/CR/LF/cursor-family AND by EL/ED per DEC STD 070+xterm ResetWrap —
  spec-audit catch), erase-to-EOL severs the wrap join to the next row,
  scroll into 10k-capped deque scrollback, per-row coalesced DamageList,
  naive resize clamped to 1x1 (rows shrink feeds top into scrollback; no
  rewrap until M1 — 3 `[!mayfail]` scaffolds in tests/unit/grid_test.cpp
  document desired reflow). ctest 17/17. Watch item for T9+ sequence work:
  every op touching the cursor line must decide `pendingWrap` explicitly
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
  presets `fuzz` (clang-cl+ASan primary, ADR-0010 — UNTESTED, clang-cl not
  installed; `winget install LLVM.LLVM` needs an admin elevation) and
  `fuzz-msvc` (verified fallback: 60s smoke = 213–350k execs, ZERO
  crashes). Seeds: `tools/extract-seeds.mjs` (node) extracts every corpus
  IN payload byte-exactly (latin1 read) + tests/fuzz/seeds → 189 files.
  `tests/fuzz/run-smoke.cmd` = the per-commit gate; replays
  tests/fuzz/regressions/ first (empty). RelWithDebInfo override keeps
  asserts live in fuzz builds. Qt 6.11.1 (the pin) now installed at
  C:\Qt\6.11.1\msvc2022_64 via aqtinstall.

- T11 ✔: Qt shell + D3D11 triangle spike. `krait-app` (console subsystem
  for M0 so gates read stdout): main.cpp sets D3D11, loads Krait/Main.qml;
  `Triangle` = QQuickRhiItem spike (src/render/spike) with qsb shaders via
  qt_add_shaders; pipeline-create failure degrades to clear-only. VERIFIED:
  `rhi backend: D3D11` logged, autoquit hook KRAIT_SPIKE_AUTOQUIT. Qt
  DEVIATION: pin 6.11.1 not installable via aqt (metadata gap) — built
  against 6.10.3 + qtshadertools module, floor 6.8, `QT_ROOT` env feeds
  CMAKE_PREFIX_PATH (SETUP.md). GuiPrivate linked for rhi/qrhi.h; QML
  module SOURCES + include dir needed for qmltyperegistrations.
  sceneGraphInitialized signal unreliable cross-thread — backend logged
  via 1s singleShot instead.

- T12 ✔: glyph-atlas spike renderer. FreeType (vcpkg) rasterizes ASCII
  into an R8 atlas (`src/render/spike/glyph_atlas.*`); `SpikeGrid`
  QQuickRhiItem draws 240×63 instanced quads, per-cell glyph+fg+bg in a
  DYNAMIC PER-INSTANCE BUFFER (bench-equivalent deviation from the plan's
  storage-buffer wording — decide finally with T13 data). VERIFIED via
  screenshot (KRAIT_SPIKE_SCREENSHOT hook): full glyph grid on D3D11;
  sent to the user for the human eyeball check. HARD-WON QRhi FACTS:
  (1) first initialize() runs BEFORE first synchronize() — create
  resources lazily, re-attempt from render(); (2) cb->setShaderResources()
  after setGraphicsPipeline or all bindings read zeros (black screen);
  (3) KRAIT_SPIKE_ATLAS_DUMP env dumps the atlas PNG for debugging.

- T13 ✔ (PR #9): flood bench. KRAIT_BENCH/_4K/_WARP env drive the spike;
  `bench/spike/flood-report.cmd`; baseline `bench/baselines/m0-spike.json`
  (machine HomeCenter, RTX 4060). Results: dev GPU 180 fps (display cap) /
  0.169 ms GPU at 4K flood; WARP 80.7 fps / 10.07 ms. WARP selected via
  QQuickGraphicsConfiguration::setPreferSoftwareDevice; timestamps via
  setTimestamps — BOTH only take effect if set BEFORE first expose, hence
  Main.qml visible:false + show-after-config in main().
- T14 ✔ (PR #10): ADR-0013 verdict = **GO**, all gates cleared with
  margin; the T12 per-instance-buffer deviation ratified by the numbers.

- T15 ✔ (PR #11): ConPTY backend + full wiring — KRAIT IS A TERMINAL.
  `third_party/openconsole/` = REPACKAGED Microsoft.Windows.Console.ConPTY
  v1.24 nupkg (decision per ADR-0011; conpty.dll + OpenConsole.exe +
  conpty.h + MS LICENSE + VERSION.md pin). `src/net/{ibackend,error}.h`,
  `src/net/conpty/` (bundled-dll Conpty* exports, reader/writer threads,
  3s-timeout stop). `src/core/terminal/session.*` = parser+grid+handlers
  behind feed(). `src/app/terminal_item.*` = TerminalView (keyboard map,
  resize, cursor inversion). HARD-WON: STARTF_USESTDHANDLES with NULL std
  handles is MANDATORY — without it the child bypasses the pty through
  inherited std pipes (prompt leaked to parent console). Verified: `dir`
  output renders through grid + screenshots sent to owner. Debug hooks:
  KRAIT_TERM_INJECT (auto-type), KRAIT_SPIKE_SCREENSHOT.

## Next task (exactly one)

**M0 CLOSEOUT — user actions required:**
1. MANUAL GATE (plan T15): run `build\dev\src\app\krait-app.exe` and type
   `dir` in the PowerShell that appears — the plan's acceptance line.
2. Merge the PR stack IN ORDER: #1 → #2 → … → #11.
3. Environment debts: clang-cl not installed (ADR-0010 primary fuzz
   preset untested; `winget install LLVM.LLVM` as admin), Qt pin 6.11.1
   not installable via aqt (built on 6.10.3; use the official installer,
   then flip QT_ROOT).
Then M1 begins per `docs/plan/01-milestones.md`.

## After that

T7 onward in dependency order, `/preflight` green after each. M0 acceptance =
`docs/plan/01-milestones.md` §M0 incl. the QRhi spike gate (T12–T14),
verdict recorded as ADR-0013.

## Open questions (non-blocking)

- Code-signing certificate vendor + timing (needed by M6, decide by M4).
- Reference perf machine spec for `bench/baselines/` (record with T13).
- OpenConsole acquisition path — decided inside T15 per ADR-0011.
- clang-tidy config gaps found in T5's preflight: `/Zc:preprocessor` triggers
  a driver-arg error under clang-tidy (needs `-Wno-unused-command-line-argument`
  in a wrapper or the config), and `cppcoreguidelines-avoid-c-arrays` still
  fires though its modernize alias is disabled. Non-gating; tidy's error set
  (bugprone/concurrency) is clean.

## Watchouts

- Follow task order; interview decisions live in ADRs 0004–0008 — don't re-ask.
- `.claude/settings.local.json` and `.claude/.cache/` are machine-local; never commit.
- clang-tidy must run from the VS dev shell (needs `INCLUDE`); outside it,
  MSVC headers don't resolve and findings are parse garbage.
- The parser's `entry.next == m_state` early-return skips entry actions on
  same-state table transitions — currently provably harmless, but re-verify
  if you ever add a same-state transition with an entry action (cpp-reviewer
  watch item).
- First session on a new machine: follow `SETUP.md`.

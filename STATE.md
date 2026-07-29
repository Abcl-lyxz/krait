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

## Next task (exactly one)

**T7 of `docs/plan/02-m0-tasks.md`**: SGR basic (0–29, 30–49, 90–107;
colon-subparam tolerant skeleton) + ED/EL; attributes in cell struct.
Deliverables: `src/core/parser/sgr.*`, `src/core/grid/cell.h`,
`tests/corpus/sgr/*.case`. Verify: corpus green + conformance rows same
commit. Note: T6's `StubGrid` has no cells — ED/EL need at least attribute
cells; keep it minimal, the full grid is T8.

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

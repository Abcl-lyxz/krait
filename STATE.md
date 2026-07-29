# STATE

Phase: M0 in progress

## Now

T5 just landed: the DEC ANSI parser state machine is real and corpus-tested.
The `ParserEvents` interface (print/execute/esc/csi/dcs/osc + `aborted` flag
on string ends) is the seam T6 builds on. Deviations from the vt100.net
diagram are documented in the `tables.h` header comment — read it before
touching the tables.

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

## Next task (exactly one)

**T6 of `docs/plan/02-m0-tasks.md`**: C0 controls (BEL BS HT LF CR SO SI) +
CSI cursor family (CUU CUD CUF CUB CUP HVP CHA VPA) on a stub grid;
`docs/conformance.md` rows flip ✗→✔ (file starts here); fuzz seeds per
sequence (`tests/fuzz/seeds/` starts here).
Verify: corpus green + conformance rows updated in the same commit.
Note for T6: OSC/DCS sinks must discard aborted strings and bound their own
payload accumulation — the parser streams unbounded (events.h doc comments).

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

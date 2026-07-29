# STATE

Phase: M0 planned

## Now

The master plan is written and approved (2026-07-29): `docs/plan/00..03`,
ADRs 0004–0012 recorded. All load-bearing API facts were MCP/doc-verified —
the ledger is Appendix A of `docs/plan/00-architecture.md`. Notable plan-time
findings: libssh 0.11+ has native ProxyJump (ADR-0012 partially supersedes
ADR-0002's DIY chaining), esctest is GPL-2.0 (external runner only, ADR-0005),
QML embedding = QQuickRhiItem (ADR-0009), Qt pin = 6.11.1.
Still zero code, zero CMake — correct.

## Done so far

- T1 ✔ (commit c941753): repo live at https://github.com/Abcl-lyxz/krait,
  MIT LICENSE, scaffold pushed on `main`.
- T2 ✔: CMakePresets (`dev`), root CMakeLists, vcpkg.json (baseline
  9d7f79f5, 2026-07-28), SETUP.md updated. Verified: `cmake --preset dev`
  green — vcpkg installed catch2/fmt 12.2.0/utf8proc 2.11.3, MSVC 19.51
  (VS2026 18 Community), CMake 4.2.3 + Ninja bundled with VS,
  vcpkg at `C:\vcpkg` (set `VCPKG_ROOT=C:\vcpkg` in the dev shell).

- T3 ✔: `krait-core` static lib + Catch2 wiring (`ctest --preset dev` =
  1/1 passed) + standalone zero-dep proof builds without vcpkg/Qt
  (`cmake -S tests/core-standalone -B build/core-standalone -G Ninja`).
  cpp-reviewer: clean pass (watch items in `.claude/agent-memory/`).

## Next task (exactly one)

**T4 of `docs/plan/02-m0-tasks.md`**: UTF-8 decoder (incremental,
malformed→U+FFFD per WHATWG replacement rules) + corpus harness v0
(`tests/corpus/*.case` golden format: `IN` bytes / `EXPECT` events).
Verify: `ctest --preset dev -R corpus`.

## After that

T2 onward in dependency order, one task at a time, `/preflight` green after
each (until T2 lands the presets, `/preflight` = "T-verify command passes").
M0 acceptance = the block in `docs/plan/01-milestones.md` §M0, including the
QRhi spike gate (T12–T14): ≥60 fps 4K flood on dev GPU, ≥30 fps WARP,
<10 ms/frame — verdict recorded as ADR-0013.

## Open questions (non-blocking)

- Code-signing certificate: which vendor + when to buy (needed by M6, decide
  by M4).
- Reference perf machine spec for `bench/baselines/` — record machine id with
  the first committed baseline (T13).
- OpenConsole acquisition path (build from source vs repackage release
  artifact) — decided inside T15 per ADR-0011.

## Watchouts

- `src/` starts at T3, not before; follow task order — T1 is deliberately
  toolchain-free.
- Interview decisions are recorded in ADRs 0004–0008 — do not re-ask.
- `.claude/settings.local.json` and `.claude/.cache/` are machine-local;
  never commit them (T1's .gitignore covers this).
- First session on a new machine: follow `SETUP.md` (MCP approval, plugins,
  clangd) or half the tooling will be silently missing.

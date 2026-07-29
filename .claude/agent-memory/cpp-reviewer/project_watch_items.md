---
name: project-watch-items
description: Deferred/latent issues spotted in past reviews of the Krait terminal, to re-check when the relevant code changes
metadata:
  type: project
---

Watch items from past reviews (verify still true before flagging):

- T3 (M0 skeleton, reviewed clean 2026-07-29):
  - `src/core/CMakeLists.txt` exposes `src/` as PUBLIC include dir. Fine while
    core is alone; once `src/net`/`src/render` exist, linking krait-core grants
    include access to sibling trees — re-check hygiene at T11.
  - Version string "0.0.1" duplicated in root `project(VERSION)`,
    `version.cpp`, and smoke test. Drift risk if versioning becomes real.
  - `tests/core-standalone` builds with NO vcpkg toolchain and duplicates the
    root compile-flag line. It will break the moment utf8proc is linked into
    krait-core, and silently diverges if root flags change. Expect a fix
    when the parser lands.

- T4 (UTF-8 decoder + corpus harness, reviewed clean 2026-07-29):
  - `tests/corpus/harness.cpp` silently skips directive lines that don't match
    `IN `/`EXPECT ` exactly (a typo'd `EXPCT` drops a case with no failure),
    and `stoi`/`stoul` throw on non-hex in `\xNN`/`U+` specs. Acceptable while
    corpus files are repo-owned; re-check if the harness grows formats or
    parses generated/external corpora.
  - Utf8Decoder verified against WHATWG by hand (boundary table, restore loop,
    max-2-outputs bound, finish semantics) — future edits to it should re-run
    the same hand-checks: \xC0\xAF, \xED\xA0\x80, \xF4\x90\x80\x80, \xE0A.

- T5 (DEC ANSI parser machine, reviewed 2026-07-29, hand-traced against
  vt100.net + corpus, no blocking findings):
  - `oscEnd()`/`dcsUnhook()` fire identically for clean termination (ST/BEL)
    and CAN/SUB/C1 abort — sinks cannot tell a truncated OSC 52/DCS payload
    from a complete one. Flagged; if not fixed with an `aborted` flag by T6/T9
    (OSC/DCS consumers), re-flag harder — clipboard/title sinks must not act
    on aborted strings as if complete (xterm discards on CAN).
  - Parser holds no OSC/DCS buffer, so the vt-core "payload caps" obligation
    moved to every ParserEvents sink. Check each new sink (T6+) caps its
    accumulation; events.h was asked to document this contract.
  - `feedByte`'s same-state shortcut skips entry actions; only safe because
    no table row stays in Escape/CsiEntry/DcsEntry while collecting. If a
    future table edit adds a stay-with-action entry to a state that has an
    entry action, this silently breaks — re-trace on any tables.h change.
  - Harness grew a token grammar + `MODE c1` directive (sticky for rest of
    file); T4's silent-skip-of-typo'd-directives risk still present and still
    accepted (repo-owned corpus, assertion counts catch dropped cases).

**Why:** these were deliberately accepted as fine-for-now in a skeleton commit; they become defects only when later milestones touch them.
**How to apply:** when a review touches src/core deps, root flags, or T11 targets, check these first.

## T9 caps/reports (t8-grid, uncommitted at review)
- ReplyLimiter refill coverage: RESOLVED in committed T9 — harness reports test now feeds 64-byte chunks with addInput per chunk (harness.cpp ~385). Contract (addInput before each feed chunk) is documented in caps.h. Fuzz harness deliberately uses single up-front addInput => max 8 replies/iteration.
- handleReport DA1 accepts `CSI 0;1c` (only values[0] checked, count>1 not rejected). Harmless reply, noted not blocked.
- Unreachable Capabilities flags (columns132..ansiColor) + VT220 `?62` branch in da1Reply are dead until M1 — accepted because vt-core rule mandates table-generated DA. Verify flags flip only alongside real implementations.

## T10 fuzz (t9-caps, uncommitted at review)
- extract-seeds.mjs flagged: utf8-read corrupts non-ASCII IN payloads (needs latin1) + escape bound off-by-one vs harness. Verify fixed before trusting corpus-derived seeds.
- Whole clang-cl path (fuzz preset, /clang:-std=c++23, -fsanitize=fuzzer-no-link) is UNVERIFIED — clang-cl not installed on this machine; only fuzz-msvc ran. Re-check the first time someone actually configures the clang preset; also note clang branch drops /WX under a comment claiming warnings-as-errors.
- run-smoke.cmd hard-codes VS18 Community vcvars64 path — breaks on CI/other machines; fine as personal dev script, revisit when CI lands.

## T8 grid (t7-sgr, uncommitted at review)
- Grid::resize/ctor accept <=0 dims -> row/col=-1 -> cellAt UB. Flagged BLOCKING; verify fix landed before app-layer wires window resize.
- wrappedFromPrev never cleared by ED/EL full-row erase -> stale wrap flags feed M1 reflow. Re-check when reflow lands.
- scrollUp allocates a fresh Line per scrolled row (emplace_back) even when scrollback cap discards one -> recycle candidate when parser bench lands.

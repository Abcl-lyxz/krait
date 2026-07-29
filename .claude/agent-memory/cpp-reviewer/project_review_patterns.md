---
name: project-review-patterns
description: Recurring things to check when reviewing Krait VT-core diffs (T5-T7 era)
metadata:
  type: project
---

Recurring checks for src/core parser diffs:
- vt-core.md is the strictest rule: every sequence change needs, same commit: corpus cases (valid + malformed + INTERRUPTED-mid-sequence), fuzz seed, conformance.md row. Interrupted variants are the one authors forget (T6 forgot them).
- `Params` (events.h): values uint16 capped 16383 by machine.cpp, count<=32, subparam[] flags for ':'-separated parts. Handlers that ignore subparam flags silently accept malformed colon sequences (xterm ignores colons outside SGR) — check each new CSI handler.
- Private markers (0x3C-0x3F) travel in the `intermediates` span, so `intermediates.empty()` check covers both markers and true intermediates.
- Corpus harness (tests/corpus/harness.cpp): IN lines feed bytes verbatim incl. space; \xNN escapes; per-family TEST_CASE + sink class pattern.
- conformance.md updates so far honest (◐ with explicit gaps listed) — keep verifying, it's rule #5.

**How to apply:** on any src/core/parser diff, grep the new .case files for CAN/SUB/incomplete-input variants before signing off.

T7 confirmations (both recurring patterns hit again):
- handleErase (sgr.cpp) shipped ignoring subparam flags — third reminder that every non-SGR CSI handler must reject colon subparams like handleCsiCursor does. Check this FIRST on any new CSI handler.
- sgr/basic.case shipped without an interrupted-mid-SGR variant (erase.case had one). Authors remember interruption for the second family they write, not the first.
- Session-file hygiene: agent memory once got written under tests/fuzz/seeds/.claude/ — check untracked dirs for stray .claude/ junk before commit.

## T9 confirmation (checklist finally internalized)
- First diff where all three recurring asks shipped unprompted: colon-subparam rejection, interrupted-mid-sequence corpus case (CAN), conformance row honest with explicit gaps. Keep checking, but the author has the pattern now.
- New recurring check for reply-generating handlers: rate/flood guards must have a test that exercises the REFILL path, not just initial-credit exhaustion — test harness wiring (single up-front addInput) can make refill untestable by construction.

## T10 lesson (fuzz tooling)
- Node tooling that re-parses corpus .case files must mirror harness.cpp parseBytes EXACTLY: read as latin1 (not utf8 — c1.case and utf8/basic.case contain raw multibyte, charCodeAt+Buffer masks to low byte), and bound check `i + 3 < len` (mjs shipped `< len + 1`, accepts truncated 1-digit escape). Diff any new byte-parser against harness.cpp:70 line by line.
- Fuzz presets keep asserts live by overriding CMAKE_CXX_FLAGS_RELWITHDEBINFO to "/O2 /Zi" (drops /DNDEBUG). Verified correct; re-check if presets are touched.
- Harness invariant asserts verified sound vs core: intermediates cap 2 (machine.h:44), params uint16<=16383/count<=32, grid cursor always clamped (deferred wrap keeps col<cols). ReplyLimiter credits RESET to 8 per window (never accumulate), so single up-front addInput => max 8 replies/iteration.

## T8 lesson
- docs/conformance.md rows go stale when stub behavior becomes real (LF "no scroll until T8" row survived T8). Grep conformance.md for the touched controls every grid/parser diff.
- Grid behavior changes tend to ship unit tests only; vt-core rule also wants corpus cases (parser-path: wrap at margin, LF-at-bottom scroll, pendingWrap cancel) in the same commit.

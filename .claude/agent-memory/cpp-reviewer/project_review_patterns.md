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

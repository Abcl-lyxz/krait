---
name: project-review-patterns
description: Recurring things to check when reviewing Krait VT-core diffs (T5/T6 era)
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

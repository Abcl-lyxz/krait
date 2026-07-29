---
name: t7-audit-findings
description: T7 SGR/ED/EL audit (2026-07-29) — handleErase missing subparam guard (T6 pattern recurred exactly as predicted); all EXPECT lines verified correct
metadata:
  type: project
---

T7 audit (sgr.{h,cpp}, grid/cell.h, corpus sgr/{basic,erase}.case) on 2026-07-29:

1. **Deviation caught: `handleErase` has no `params.subparam[]` guard** —
   `CSI 1:2 J` executes as ED 1 instead of being ignored. The exact recurrence
   predicted in [[t6-audit-findings]]; `handleCsiCursor` (csi_cursor.cpp:53-55)
   has the guard, sgr.cpp's handleErase does not. Deviation was undeclared in
   docs/conformance.md.
2. Everything else conformant vs ctlseqs (re-fetched 2026-07-29): ED/EL
   default 0, ED0/1/2 + EL0/1/2 inclusive ranges, SGR 22 = neither bold nor
   faint, 21 doubly-underlined (declared approximation), 38/48 colon+legacy
   arity consumption correct incl. truncated `38;5` and mixed-form cases.
   All 28 corpus EXPECT lines re-derived by hand on 24x80 stub — all correct.
3. Untested-but-declared gaps flagged: DECSEL (`CSI ? K`), SGR with marker
   (`CSI > 0 m`), colon 256-color `38:5:196`. erase.case "erased cells drop
   attributes" comment is unverified — describeSgr never surfaces cell attrs.
4. T5 watch item closed: print consumer now exists (putChar); DEL-in-ground
   still untested in print path. OSC/DCS caps still open (no string consumers).

**Why:** the subparam guard keeps being forgotten because it lives per-consumer;
until a shared reject-subparams helper exists, every new CSI family will ship
without it.
**How to apply:** first check on any CSI-family audit: grep the new consumer
for `subparam`; absence = near-certain deviation. Also carry: DEL-in-ground,
OSC/DCS payload caps, cell-attr assertions in erase corpus.

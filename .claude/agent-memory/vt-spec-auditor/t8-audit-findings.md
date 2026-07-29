---
name: t8-audit-findings
description: T8 grid/deferred-wrap audit (2026-07-29) — EL/ED must clear pendingWrap (DEC STD 070 + xterm ClearRight ResetWrap); verified spec URLs for wrap-flag semantics
metadata:
  type: project
---

T8 audit (grid.{h,cpp}, line.h, damage.h, csi_cursor/sgr rewire, grid_test,
corpus) on 2026-07-29:

1. **Deviation caught: `handleErase` (EL/ED) does not clear
   `grid.pendingWrap`.** DEC STD 070 lists "Erase in line, in display (EL,
   ED)" among LCF-resetting operations; xterm `ClearRight`/`ClearInLine2`/
   `ClearScreen` all call `ResetWrap(screen)`. Real-world: GNU grep emits EL
   at line ends — wrong flag state drops characters. Pattern: **every new
   op that touches the line under the cursor must decide pendingWrap
   explicitly** — check this on ICH/DCH/IL/DL (xterm resets there too),
   DECALN, scroll region ops.
2. Minor: xterm ClearRight also does `LineClrWrapped(ld)` — severs the join
   current-row→next-row on ANY EL0/erase-to-EOL, even mid-row. Krait (flag
   stored on the FOLLOWING line as wrappedFromPrev) only clears when a whole
   row blanks, and never severs r→r+1. Reflow-visible only (M1); flagged.
3. Everything else conformant per STD 070: park-at-last-col, wrap on next
   printable, BS/HT/CR/LF/CUU-CUB/CUP/HVP/CHA/VPA all clear the flag,
   rejected sequences don't. wrappedFromPrev set on soft wrap only. Scroll
   pushes to scrollback (10k cap). Prior corpus EXPECTs all still correct.
4. T7 watch item RESOLVED: handleErase now has the subparam guard.
5. Watch for T9+: no corpus/harness token for wrappedFromPrev or scrollback
   size — wrap-flag behavior is unit-test-only, invisible to the corpus.
   HT-with-pendingWrap and wrap-scroll-at-bottom corpus cases still missing.

Verified spec URLs (add to [[spec-sources]] use):
- https://raw.githubusercontent.com/mattiase/wraptest/master/README.md —
  DEC STD 070 LCF reset list (canonical deferred-wrap reference).
- https://raw.githubusercontent.com/ThomasDickey/xterm-snapshots/master/util.c
  — ClearRight: `LineClrWrapped(ld); ResetWrap(screen);` also InsertLine/
  DeleteLine/InsertChar/DeleteChar reset.
- https://raw.githubusercontent.com/ThomasDickey/xterm-snapshots/master/cursor.c
  — CursorSet/Up/Down/Forward/CarriageReturn all `ResetWrap`.
- https://vt100.net/docs/vt510-rm/DECAWM.html — wrap outcome only, no flag
  detail; use wraptest/xterm source for flag semantics.
- charproc.c is too large for WebFetch (truncates before CASE_TAB).

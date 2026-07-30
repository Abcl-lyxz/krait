---
name: t18-audit-findings
description: T18 part-3 audit (DECOM ?6 + alt screen 1049): 1049 idempotency guard is NOT xterm behavior; CPR can emit a negative parameter; fuzz target never routes h/l or r/L/M/S/T
metadata:
  type: project
---

# T18 part 3 audit — DECOM (?6) + mode 1049 (2026-07-30)

Reviewed `csi_mode.{h,cpp}`, `Grid::cursorSet/eraseScreen/useAlternateScreen/
saveCursor/restoreCursor/resize`, margin-aware CUU/CUD, origin-relative CPR,
`csi/origin.case` (24 cases) + `sgr/mode.case` (~22 cases). All ~46 corpus
expectations recompute correctly **against this implementation**; the problems
are where the implementation itself drifts.

## Deviations caught (ranked)

1. **The 1049 idempotency guard is a divergence, not xterm fidelity.**
   `applyMode`'s `if (on == grid.onAlternateScreen()) break;` skips the whole
   action. xterm guards only the buffer swap (see [[spec-sources]] for the
   verbatim `case srm_OPT_ALTBUF_CURSOR`), so a repeat `1049h` re-clears and
   re-saves and a repeat `1049l` restores. Two corpus cases enshrine our
   behavior (`sgr/mode.case` "entering twice", `csi/origin.case` "leaving when
   we never entered"). Either drop the guard or label it a DIVERGENCE the way
   the BCE deferral is labelled — but do not attribute it to xterm.
2. **DSR 6 CPR under DECOM is unclamped** (`caps.cpp`: `grid.row - grid.scrollTop`).
   Reachable from input: `CSI ?6h` `CSI ?1049h` `CSI 20;24r` `CSI ?1049l`
   `CSI 6n` → `ESC [ -18 ; 1 R`. A `-` is not a CSI parameter byte (ECMA-48
   §5.4), so the app drops the reply and may block. xterm has the same
   subtraction but its ParmType is short-printed-as-unsigned.
3. **The fuzz target cannot reach any of T18.** `tests/fuzz/parser_fuzz.cpp`
   routes only `m`, `J/K`, `c/n`, else `handleCsiCursor` — neither `handleMode`
   nor `handleScroll` is wired, so `mode.seed` and `scroll.seed` are inert and
   the `checkCursor()` invariant never sees DECOM/1049. Check this every time a
   new CSI family lands; the corpus harness and the fuzz sink are separate
   dispatch chains that drift apart.

## Verified-correct (do not re-audit)

`cursorSet` == xterm CursorSet (incl. no top clamp, low clamp at 0 after the
offset); CUU/CUD read margins not flags; `restoreCursor`'s `sc.row - scrollTop`
then re-add is exactly CursorRestoreFlags, and `pendingWrap` after `cursorSet`
matches xterm's `/* after CursorSet/ResetWrap */`; margins/pen shared across
buffers; `sc[2]` indexed by active buffer with the save landing in slot 0;
`resize` clearing ORIGIN + margins; `useAlternateScreen`'s `front()` is safe
(`||` short-circuits and `rows >= 1` is enforced by ctor/resize).

## Recurring patterns worth checking next time

- Cases whose EXPECT equals the cursor's pre-existing position prove nothing
  about "does not move" claims (e.g. inverted DECSTBM with the cursor already
  on the top margin). Move the cursor off the candidate answer first.
- `csi/` cases only see `cur:`/`g1:`/`bell:`; `sgr/` cases see
  `pen:/region:/origin:/alt:/ul:/line:`. A mode assertion put in the wrong
  directory is silently untested.

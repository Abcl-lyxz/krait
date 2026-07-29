---
name: t6-audit-findings
description: T6 CSI-cursor/C0 audit (2026-07-29) — colon-subparam leak into cursor family caught; T5 watch items 1 and 4 still open
metadata:
  type: project
---

T6 audit (csi_cursor.{h,cpp}, corpus csi/{cursor,c0}.case) on 2026-07-29:

1. **Deviation caught: colon subparams reach cursor dispatch.** The tables.h
   0x3A deviation (kept legal for SGR 38:2) means `CSI 5:3 H` dispatches with
   subparam[1]=true, and `handleCsiCursor` never checks `params.subparam[]` —
   Krait executes it as CUP 5;3 while xterm ignores the whole sequence
   (Williams: CsiParam 0x3A → CsiIgnore). General pattern: **every non-SGR CSI
   consumer must reject when any subparam[i] is true for i < count** — re-check
   this on each new CSI family (erase/scroll, modes, DECSTBM...).
2. Everything else conformant: defaults 1 / 0→1, CUP/HVP [1,1], CHA/VPA
   absolute+clamp, edge clamps, BEL/BS/HT/LF/CR/SO/SI within declared stub
   limits. All corpus EXPECT lines correct on 24x80. Fuzz seeds present.
3. T5 watch items still open after T6: DEL-in-ground print (no print consumer
   yet) and OSC/DCS payload caps (no string consumers yet). Carry to T7+.

**Why:** item 1's pattern will recur on every CSI family until a shared
"reject subparams unless SGR" guard exists.
**How to apply:** when auditing any new CSI consumer, first grep it for
`subparam`; absence is a red flag. See [[spec-sources]] for verified defaults.

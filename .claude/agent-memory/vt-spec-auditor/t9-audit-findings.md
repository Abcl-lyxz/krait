---
name: t9-audit-findings
description: T9 (caps/DA1/DSR) audit 2026-07-29 — forms conformant; ansiColor under-claim is deliberate; missing corpus variants listed
metadata:
  type: project
---

T9 audit (caps.{h,cpp}, reports/basic.case): no wrong forms found.

Verified against ctlseqs (fetched 2026-07-29):
- DA1 `CSI ? 1 ; 2 c` = VT100 with AVO; `CSI ? 1 ; 0 c` = **VT101** with No
  Options (caps.h header comment calls it VT100 — cosmetic only).
- VT220+ extension codes 1=132col, 2=Printer, 3=ReGIS, 4=Sixel, 6=SelErase,
  22=ANSI color — da1Reply mapping matches.
- DSR 5 → `CSI 0 n`; DSR 6 → `CSI r ; c R`, 1-based. Grid cursor is 0-based
  (grid.h:26), CPR adds 1 — correct.
- Private markers `>`/`?` land in intermediates via Collect (tables.h:131),
  so the `!intermediates.empty()` guard correctly silences DA2/DECXCPR.
- The T6/T7 recurring bug (missing colon-subparam guard in new CSI consumer)
  did NOT recur — handleReport checks `params.subparam[i]` up front.

**Honesty note:** SGR 30-37/38/48 (256-color + RGB) ARE implemented (T7,
sgr.cpp cases 38/48 with ;5/;2) but `ansiColor=false`. Deliberate under-claim
(comment: code 22 needs the 62 identity, which would over-claim VT220 base).
Safe direction; revisit at M1 when the VT220 identity lands.

**Why:** the repo honesty rule forbids over-claim only; under-claim is the
sanctioned default.
**How to apply:** when M1 flips any VT220 flag, re-audit that 62-identity
implies DECSCA-class base features; also re-check `ReplyLimiter` wiring —
harness calls addInput() before feed(), real integration must feed as bytes
arrive.

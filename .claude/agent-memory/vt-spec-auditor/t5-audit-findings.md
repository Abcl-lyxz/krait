---
name: t5-audit-findings
description: T5 parser machine audit result (2026-07-29) — undocumented edge choices to re-check when T6+ builds on the machine
metadata:
  type: project
---

T5 machine (src/core/parser/) audited against vt100.net/emu/dec_ansi_parser on 2026-07-29: no hard deviations beyond the tables.h-documented list. Two undocumented edge choices to keep an eye on:

1. **DEL (0x7F) in Ground prints U+007F** — goes through the UTF-8 decoder like any GL byte. Williams diagram says 20-7F/print, but xterm-family terminals ignore DEL in ground. Whoever writes the T6+ print consumer must decide and document; there is no corpus case for bare DEL in ground.
2. **Private markers (0x3C-0x3F) share the 2-slot intermediates buffer** — marker + 2 intermediates trips ignore-dispatch. Spec says flag on ">2 intermediate characters"; whether the marker counts is ambiguous. No real sequence hits it today; revisit if one appears.
3. Behaviorally-neutral shortcuts verified safe: same-state early return skips Clear on ESC-while-in-Escape (nothing accumulates in Escape, unobservable); handleC1 fires execute after transition instead of between exit and entry (Ground entry is None, unobservable).
4. Payload caps: OSC/DCS stream byte-wise to the sink — the machine holds no buffer, so length caps are deferred to T6 consumers. Audit them there.

**Why:** these are exactly the spots a future sequence-implementation commit could silently turn from "unobservable" into "wrong".
**How to apply:** when auditing T6/T7 (C0/CSI cursor/SGR), check items 1 and 4 got resolved; see [[spec-sources]] for verified spec facts.

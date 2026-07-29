# Memory index

- [Spec sources](spec-sources.md) — verified vt100.net Williams parser URL + confirmed facts (ESC-anywhere has no action; collect/param overflow prose)
- [T5 audit findings](t5-audit-findings.md) — 2026-07-29 audit: no undocumented deviations; DEL-in-ground and marker-in-intermediates-cap flagged for T6
- [T6 audit findings](t6-audit-findings.md) — colon-subparam leak into cursor CSI caught; recheck `subparam` guard in every future CSI consumer
- [T7 audit findings](t7-audit-findings.md) — handleErase missing subparam guard (T6 pattern recurred); all EXPECT lines verified; attr-drop untested
- [T8 audit findings](t8-audit-findings.md) — EL/ED must clear pendingWrap (STD 070 + xterm ClearRight); wraptest/xterm-source URLs verified; corpus lacks wrap-flag token
- [T9 audit findings](t9-audit-findings.md) — DA1/DSR forms conformant; ?1;0c is VT101 not VT100; ansiColor=false is deliberate under-claim despite SGR color existing
- [T17 audit findings](t17-audit-findings.md) — SGR 38/48/58: Pi-by-count is correct; all 3 deviations were argument-consumption, incl. subparam-index leak

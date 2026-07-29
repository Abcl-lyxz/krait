# Memory index

- [Spec sources](spec-sources.md) — verified vt100.net Williams parser URL + confirmed facts (ESC-anywhere has no action; collect/param overflow prose)
- [T5 audit findings](t5-audit-findings.md) — 2026-07-29 audit: no undocumented deviations; DEL-in-ground and marker-in-intermediates-cap flagged for T6
- [T6 audit findings](t6-audit-findings.md) — colon-subparam leak into cursor CSI caught; recheck `subparam` guard in every future CSI consumer
- [T7 audit findings](t7-audit-findings.md) — handleErase missing subparam guard (T6 pattern recurred); all EXPECT lines verified; attr-drop untested
- [T8 audit findings](t8-audit-findings.md) — EL/ED must clear pendingWrap (STD 070 + xterm ClearRight); wraptest/xterm-source URLs verified; corpus lacks wrap-flag token

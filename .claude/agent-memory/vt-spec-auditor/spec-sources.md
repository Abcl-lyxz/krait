---
name: spec-sources
description: Verified VT spec page URLs and load-bearing facts confirmed from them (Williams parser details)
metadata:
  type: reference
---

# Verified spec sources

- **Paul Williams DEC ANSI parser**: https://vt100.net/emu/dec_ansi_parser — verified live 2026-07-29.
  Facts confirmed by fetch (do not trust summarizer tables blindly — one fetch
  wrongly claimed ESC-anywhere has an execute action):
  - Anywhere 0x1B → escape is a **pure transition, no action**; clear is the escape entry action.
  - Action ordering: exit action → transition action → entry action.
  - collect: ">2 intermediate characters → flag so dispatch becomes a null operation" (ignore-dispatch flag is spec-blessed).
  - param: spec stores max **16** params, extras "silently ignored"; digits per param unlimited.
  - OSC string exit action osc_end fires on CAN/SUB/ESC/ST exits alike; DCS passthrough exit unhook likewise.
  - GR note: 0xA0-0xFF treated as GL 0x20-0x7F (Krait deviates deliberately: UTF-8 outside machine).
  - Ground 0x7F: diagram lumps it into 20-7F/print but page notes special/ambiguous DEL handling — real terminals mostly ignore DEL in ground. Open point, see [[t5-audit-findings]].

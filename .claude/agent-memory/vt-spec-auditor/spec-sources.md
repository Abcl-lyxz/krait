---
name: spec-sources
description: Verified VT spec page URLs and load-bearing facts confirmed from them (Williams parser details)
metadata:
  type: reference
---

# Verified spec sources

- **xterm ctlseqs**: https://invisible-island.net/xterm/ctlseqs/ctlseqs.html — verified live 2026-07-29.
  Facts confirmed by fetch:
  - CUU/CUD/CUF/CUB default = 1; CUP/HVP default = [1,1]; CHA `CSI Ps G` default column 1; VPA `CSI Ps d` default row 1.
  - Colon subparameters are documented **only for SGR** (38:2:..., "colons (standard) or semicolons (legacy)"). Williams machine sends 0x3A in CsiParam → CsiIgnore, so non-SGR sequences containing a colon are ignored whole by xterm.
  - SO = LS1 (invoke G1), SI = LS0 (invoke G0). `CSI Ps SP A` is SR (scroll right), not CUU.

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

- **kitty underlines**: https://sw.kovidgoyal.net/kitty/underlines/ — verified live 2026-07-29.
  - `4:0`..`4:5` = none/straight/double/curly/dotted/dashed; `4` and `24` kept for back-compat.
  - SGR 58 "works exactly like the codes 38, 48"; 59 resets. Detection = terminfo boolean `Su`.
  - Verbatim requirement: "the underline color must remain the same under reverse video, if it has a color, if not, it should follow the foreground color."
  - xterm does NOT implement 58/59 (no `case 58:` in charproc.c, no ctlseqs entry) — cite kitty, never xterm, for underline colour.

## Reading reference implementations (cheaper + more decisive than prose)

WebFetch summarizes and will not give verbatim spec text. For SGR-level
questions, download the source with `gh api` and use the Grep tool:

    gh api repos/ThomasDickey/xterm-snapshots/contents/charproc.c --jq '.content' | base64 -d > charproc.c
    gh api repos/kovidgoyal/kitty/contents/kitty/{vt-parser.c,cursor.c} --jq '.content' | base64 -d > f.c

- xterm `parse_extended_colors()` + `extended_colors_limit()` (charproc.c ~2085-2210) is the
  authority for 38/48 argument handling; the SGR loop is ~4317.
- kitty `_parse_sgr()` (vt-parser.c ~951) groups params; `cursor_from_sgr()` + `parse_color()`
  (cursor.c ~49-145) applies them.

### SGR 38/48/58 facts confirmed from those sources (2026-07-29)

- **Pi disambiguation is by count, and both references agree.** xterm comment: "provides for the
  color space identifier by checking the number of parameters: 3 after "2" (no color space
  identifier) / 4 or more after "2" (color space identifier)"; code `get_subparam(base, 2 + n + (have > 4))`.
  kitty: `if (*i + 3 < count) (*i)++;` (issue #227). Use `>=`, not `==`, so the ITU
  `38:2:Pi:R:G:B:Pts:Pt` tolerance form still works — xterm "ignores parameters 6 (and above)".
- **xterm accepts the MIXED form** — source comments literally say `/* accept CSI 38 ; 5 : 1 m */`
  and `/* accept CSI 38 ; 2 : 1 : 2 : 3 m */`, with its own Pi rule (`have > 3`) for that branch.
  "After the first colon, colons must be used" does not mean the mixed form is rejected.
- **Out-of-range: the references disagree.** xterm rejects (`values[n] < 256` else `*colorp = -1`,
  result False) but still consumes (`item = next` happens before the switch). kitty truncates
  (`params[(*i)++] & 0xFF`). Neither clamps to 255.
- **Unknown colour kind is consumed by both.** xterm `extended_colors_limit` → need 0 → the caller's
  `++item` steps past the kind. kitty aborts the whole SGR ("unknown color type: %d ignoring the
  full code"). Nobody re-reads the kind as its own SGR.
- **SGR 21 = doubly-underlined** in both: xterm `case 21: UIntSet(xw->flags, ATR_DBL_UNDER)`,
  kitty `case 21: decoration = 2` (kitty puts bold-off at the private code 221).
- **kitty clamps `4:n` to 5**: `decoration = MIN(5, params[i])`, so `4:9` is dashed, not single.
- xterm ignores subparameters on every code except 38/48 (`item += skip; op = 9999`); kitty
  dispatches them. Divergence is expected — 4:x/58 are kitty extensions.

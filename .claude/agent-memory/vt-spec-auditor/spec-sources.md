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

- **vt100.net VT510 DECOM**: https://vt100.net/docs/vt510-rm/DECOM.html — verified live 2026-07-30.
  The page describes ONLY where "home" is, never that the cursor moves:
  set = "the home cursor position is at the upper-left corner of the screen,
  within the margins ... The cursor cannot move outside of the margins";
  reset = "...independent of the margins. The cursor can move outside of the
  margins." Do not cite this page for "DECOM homes the cursor" — that comes
  from xterm's code (and DEC STD 070), not from here.
- **vt100.net VT510 CPR**: https://vt100.net/docs/vt510-rm/CPR.html — verified 2026-07-30.
  Says only "Pl indicates what line the cursor is on" — silent on DECOM.
  The margin-relative rule is xterm's, cite charproc.c CASE_DSR.

- **kitty underlines**: https://sw.kovidgoyal.net/kitty/underlines/ — verified live 2026-07-29.
  - `4:0`..`4:5` = none/straight/double/curly/dotted/dashed; `4` and `24` kept for back-compat.
  - SGR 58 "works exactly like the codes 38, 48"; 59 resets. Detection = terminfo boolean `Su`.
  - Verbatim requirement: "the underline color must remain the same under reverse video, if it has a color, if not, it should follow the foreground color."
  - xterm does NOT implement 58/59 (no `case 58:` in charproc.c, no ctlseqs entry) — cite kitty, never xterm, for underline colour.

## OSC 133 / shell integration (verified 2026-08-01)

- **The canonical spec is UNREACHABLE.** `gitlab.freedesktop.org/Per_Bothner/
  specifications/.../semantic-prompts.md` is behind Anubis on every path
  including `-/raw` — WebFetch gets the challenge page, not the doc. Do not
  burn a fetch on it again. Use the substitutes below.
- **https://contour-terminal.org/vt-extensions/osc-133-shell-integration/** —
  fetchable, verified 2026-08-01. A = prompt start (`OSC 133 ; A [; <Key>=
  <Value>...] ST`), B = prompt end, C = command output start, D = command
  finished `[; <ExitCode>]`. Says nothing about malformed options.
- **ghostty `src/terminal/osc/parsers/semantic_prompt.zig`** (NEW path — the
  old `src/terminal/osc.zig` no longer holds the 133 parser). Authoritative
  letter table: `L` fresh_line, `A` fresh_line_new_prompt, `N` new_command,
  `P` prompt_start, `B` end_prompt_start_input, `I` end_prompt_..._terminate_eol,
  `C` end_input_start_output, `D` end_command. `PromptKind.init`: `i` initial,
  `r` right, `c` continuation, `s` secondary — **any other value returns null,
  i.e. "unspecified", NOT "secondary"**, and the file states the spec rule
  verbatim: "Any errors in the raw string will return null since the OSC133
  specification says to ignore unknown or malformed options." Exit code is
  `parseInt(i32, full, 10)` on field 1 — **accepts negatives**.
- **ghostty `src/terminal/osc/parsers/osc9.zig`** — OSC 9;4 states 0-4; percent
  read only for set/error/pause (remove + indeterminate skip it), `parseUnsigned`
  then `clamp(0,100)`. Also handles 9;1 sleep, 9;2 msgbox, 9;3 tab title,
  9;10 xterm emulation, **9;12 = mark prompt start**.
- Emitters, downloaded and read: kitty `shell-integration/{bash/kitty.bash,
  zsh/kitty-integration,fish/vendor_conf.d/kitty-shell-integration.fish}` —
  `\e]133;A\a`, `\e]133;A;k=s\a` (PS2), `\e]133;C;cmdline=%q\a`,
  `\e]133;D;$?\a` immediately followed by `\e]133;A\a`, and a kitty-private
  `\e]133;k;<name>\a`. wezterm `assets/shell-integration/wezterm.sh` (repo is
  `wez/wezterm`) — `\e]133;P;k=i`, `\e]133;P;k=s`, `\e]133;A;cl=m;aid=%s`,
  `\e]133;C;`, `\e]133;D;%s;aid=%s`. So **P and A must BOTH mean prompt start**.

## xterm facts confirmed 2026-08-01 (util.c)

- **`DeleteLine` DOES archive into scrollback.** `scroll_all_lines =
  (scrollWidget && !whichBuf && cur_row == 0)`, and that branch calls
  `ScrnDeleteLine(xw, screen->saveBuf_index, bot_marg + savelines, 0, n)` —
  the saved-lines array, from index 0. So `CSI M` at row 0 on the normal
  buffer pushes the top line into history. Krait's `capturesScrollback()`
  (`scrollTop == 0 && !m_onAlt`) plus csi_scroll's temporary `scrollTop = row`
  reproduces this. Not a bug; do not re-flag it.

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

### Cursor / margins / alt-screen facts confirmed from xterm master (2026-07-30)

Files: `charproc.c`, `cursor.c`, `screen.c`, `util.c`, `ptyx.h` (same `gh api` recipe).

- **`case srm_OPT_ALTBUF_CURSOR:` (1049), charproc.c ~7727** — verbatim:
  `if (IsSM()) { CursorSave(xw); ToAlternate(xw, True); ClearScreen(xw); }
  else { FromAlternate(xw, False); CursorRestore(xw); }`
  The `whichBuf` guard lives ONLY inside `ToAlternate`/`FromAlternate`
  (charproc.c ~9523/9542). **CursorSave, ClearScreen and CursorRestore are
  unconditional** — so in xterm a repeated `1049h` DOES re-clear the alt screen
  and re-save into `sc[1]`, and a `1049l` on the normal screen DOES restore.
  Anyone claiming "xterm's whichBuf guard makes a repeat a no-op" is wrong.
- **`CursorSet` (cursor.c 68)** — ORIGIN: `use_col += lft_marg; max_col = rgt_marg;`
  `use_row += top_marg; max_row = bot_marg;` then `use_row = (use_row < 0 ? 0 : use_row)`
  and clamp to max; ends with `ResetWrap(screen)`.
- **`CursorUp` / `CursorDown` (cursor.c ~251/271)** read the margins, never the
  flags: `min = ((cur_row < top_marg) ? 0 : top_marg)`,
  `max = (cur_row > bot_marg ? max_row : bot_marg)`.
- **`CursorRestore` (cursor.c ~462, via CursorRestoreFlags)** — restores flags
  first, then `CursorSet(screen, sc->row - top_marg, ...)` under ORIGIN (a
  no-op round trip, since CursorSet re-adds it), then
  `screen->do_wrap = sc->wrap_flag; /* after CursorSet/ResetWrap */`.
  It does **not** bail out when `!sc->saved` — a virgin slot homes the cursor
  and resets the pen; only charsets take the `sc->saved` branch.
  `CursorSave2` stores row/col/`xw->flags`/curgl/curgr/`do_wrap`/colors/gsets — no margins.
- **`DECSTBM` (charproc.c 4768)** — `top = one_if_default(0)`; Pb DEFAULT/0/>MaxRows → MaxRows;
  `if (bot > top) { set_tb_margins(...); CursorSet(screen, 0, 0, xw->flags); }` — no else.
  `use_default_value()` (charproc.c 2224) maps `result <= 0` to the default, so an
  explicit `Ps = 0` really is 1.
- **CPR (charproc.c CASE_DSR, ~4615)** — `value = cur_row; if (flags & ORIGIN) value -= top_marg;`
  then `+ 1`; column likewise `-= lft_marg`. **No clamp at 0** (only the
  status-line branch clamps: `if ((value -= LastRowNumber(screen)) < 0) value = 0;`).
  `typedef short ParmType` (ptyx.h 439) printed via `unsigned short` — xterm
  wraps where we would print a literal `-`.
- **`ClearScreen` (util.c 1915)** — `ResetWrap(screen)` + `ClearBufRows(0, max_row)`;
  never moves the cursor, ignores the margins, and `ClearCells` (screen.c ~839)
  ORs in `TERM_COLOR_FLAGS(xw)` + `xtermColorPair(xw)` → xterm's clear IS BCE.
- **`ScreenResize` (screen.c 2189)** — inactive buffer: "The non-visible buffer is
  simple, since we will not copy data to/from the saved-lines" → plain
  `Reallocate(...editBuf_index[!whichBuf]...)`. `Reallocate` (screen.c 447):
  "If the screen shrinks, remove lines off the top of the buffer if
  resizeGravity resource says to do so" (default SouthWest → top is trimmed,
  bottom preserved, for BOTH buffers). The saved-lines copy path is gated on
  `GravityIsSouthWest && delta_rows && saveBuf_index != NULL` — **not** on
  `whichBuf`, so xterm does push alternate-screen lines into scrollback on a
  shrink. Also confirmed: `resetMargins(xw)` + `UIntClr(*flags, ORIGIN)`.
- `#define SAVED_CURSORS 2` (ptyx.h 2331).

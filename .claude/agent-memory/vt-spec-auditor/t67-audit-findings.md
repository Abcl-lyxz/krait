---
name: t67-audit-findings
description: T67 OSC 9;4 + jump-to-prompt audit (2026-08-01) — viewOffsetCeiling breaks viewRows' cost bound, reports/ corpus sink silently drops OSC, WT-vs-Krait 9;4 grammar deltas
metadata:
  type: project
---

Audited on branch `t64-m4-power-tools`, uncommitted, mixed with T64-T66.

**The corpus trap worth remembering forever:** `tests/corpus/harness.cpp`'s
`CursorSink` has EMPTY `oscStart/oscPut/oscEnd`. Every case under
`tests/corpus/reports/` therefore proves only "the state machine consumed the
bytes and sent no reply" — it cannot observe any OSC dispatch at all. Only the
`shell/` category (runs a real `Session`, plus byte-at-a-time split
invariance) and the `parser/` category (`OSC[...]` tokens + split invariance)
actually exercise `OscHandler`. Any future OSC row in `docs/conformance.md`
that cites `reports/osc.case` as grammar evidence is overstating it.

**OSC 9;4 grammar deltas vs Windows Terminal's own `DoConEmuAction`** (verified
against microsoft/terminal `src/terminal/adapter/adaptDispatch.cpp` and
`Utils::StringToUint` in `src/types/utils.cpp`, main branch):
- WT parses the subcommand and the state with `StringToUint`, so `9;04;...`
  and `9;4;04;50` are ACCEPTED. Krait requires the literal `"4"` and a single
  digit 0-4 — refuses both.
- WT: an ABSENT or EMPTY state leaves `state = 0` -> clears the bar. So
  `OSC 9;4 ST` and `OSC 9;4; ST` are "remove progress" in WT; Krait refuses.
- WT refuses the WHOLE action when the progress field is non-empty and
  non-numeric; Krait dispatches the state with percent absent.
- All three of WT, ghostty and Krait CLAMP progress to 100 (not refuse).
- ghostty (`src/terminal/osc/parsers/osc9.zig`) never even looks at progress
  for states 0 and 3, and defaults state 1 to progress 0; it uses -1 for
  absent, same as Krait.

**MS vs ConEmu really do disagree on state 4** — MS "Warning", ConEmu "paused".
Confirmed verbatim from both pages. MS also says only state 3 ignores
`<progress>`; "pr optional for 2 and 4" is ConEmu's wording alone.

**`Grid::viewOffsetCeiling()` (new in T67) is the load-bearing defect.**
`Scrollback::viewRows` sizes its work as `cellBudget = (fromEnd + count + 1) *
cols` — the guard that makes a scroll step cost a screenful. Letting
`m_viewOffset` exceed `maxViewOffset()` makes that budget proportional to the
whole ring (4M cells default). It also freezes `pushToScrollback`'s
`min(m_viewOffset + 1, ceiling)` at a no-op, so the parked viewport drifts by
one row per line of new output.

**`setCommandExit`'s `m_promptOpen` bound has a hole:** ED/EL clear `marks` on
fully-blanked rows (`sgr.cpp` `clearRange`), so `A` then `CSI 2J` then `D`
opens the flag, erases the mark, then walks ALL of history finding nothing.

Verified spec URLs (both fetched 2026-08-01, both stable):
- https://learn.microsoft.com/en-us/windows/terminal/tutorials/progress-bar-sequences
- https://conemu.github.io/en/AnsiEscapeCodes.html

Real-world 9;4 emitters seen in the wild (GitHub code search): lldb
`AnsiTerminal.h` (`]9;4;0;0`, `]9;4;1;%u`, `]9;4;2;%u`, `]9;4;3;%u`),
posva/dotfiles `set-title.sh` (emits `\033]9;4;%s\007` — STATE ONLY, no
percent), tmux's `regress/input-malformed.sh` (`]9;4;5;200`, `]9;4;z`).

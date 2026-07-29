# VT spec sources — load when choosing what to implement

## Authoritative specs

| Topic | Source |
|---|---|
| The canon: CSI/OSC/DCS/SGR | invisible-island.net/xterm/ctlseqs/ctlseqs.html |
| DEC hardware semantics | vt100.net (EK-VT100-TM manuals) |
| Parser state machine | vt100.net/emu/dec_ansi_parser (Paul Williams) |
| kitty keyboard protocol | sw.kovidgoyal.net/kitty/keyboard-protocol/ |
| kitty graphics protocol | sw.kovidgoyal.net/kitty/graphics-protocol/ |
| OSC 66 text sizing | sw.kovidgoyal.net/kitty/text-sizing-protocol/ |
| Sync output (mode 2026) | contour-terminal.org/vt-extensions/synchronized-output/ |
| Grapheme mode 2027 | contour-terminal.org/vt-extensions/grapheme-cluster-support/ (+ mitchellh.com/writing/grapheme-clusters-in-terminals) |
| OSC 133 shell integration | contour-terminal.org/vt-extensions/osc-133-shell-integration/ (semantics originated in FinalTerm; iTerm2 + WezTerm docs describe extensions) |
| In-band resize (mode 2048) | the mode-2048 spec gist by rockorager (search "in-band window resize notifications terminal") |
| OSC 52 clipboard | ctlseqs + kitty's restrictions writeup |
| Sixel | vt100.net sixel docs + libsixel notes |

## Adoption reality-checks (which terminals actually do what)

- tmuxai.dev/terminal-compatibility — living matrix
- jeffquast.com "State of Terminal Emulation" surveys
- vtdn.dev — per-SGR support tables (e.g. curly underlines)

## Local ground truth

- `docs/conformance.md` — what Krait actually implements (keep honest)
- `docs/research/landscape-2026.md §3` — adoption table + Krait verdicts
  with milestone tags

## Corpus/conformance tooling

- vttest (invisible-island.net/vttest) — interactive conformance
- esctest (originally iTerm2's, GitLab mirror) — scriptable assertions;
  mine it for corpus cases rather than running it raw in CI

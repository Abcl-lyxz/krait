---
name: add-escape-sequence
description: Implement a new VT/ANSI escape sequence, control code, or terminal mode end-to-end — spec lookup, table-driven parser change, corpus tests, fuzz seed, conformance ledger, honest capability replies. Use whenever adding or modifying terminal sequence handling in src/core.
allowed-tools: "Read, Grep, Glob, Edit, Write, Bash, WebFetch"
---

The VT core is sacred (CLAUDE.md rule 1): a sequence change is not the code —
it is code + tests + ledger + honesty, in one commit. Work through this
checklist top to bottom; copy it into your working notes and tick as you go.

- [ ] **1. Spec first.** Find the authoritative definition and READ it via
  WebFetch — never from memory. Sources by family:
  - CSI/OSC/DCS classics → invisible-island.net/xterm/ctlseqs/ctlseqs.html
  - DEC semantics (what does the VT100 actually do) → vt100.net
  - kitty keyboard/graphics/OSC 66 → sw.kovidgoyal.net/kitty specs
  - modes 2026/2027/2048, OSC 133 → contour-terminal.org vt-extensions
  - Cross-terminal adoption reality → `references/vt-sources.md`
  Note: default parameter value (0 vs 1!), both terminators for OSC (BEL and
  ST), colon-vs-semicolon subparameter rules, and behavior when interrupted
  (CAN/SUB/ESC).

- [ ] **2. Table, not ifs.** Add the sequence as state-table entries +
  action functions per the parser's existing pattern. If the change seems to
  need an `if` inside the byte loop, the design is wrong — stop and rethink.

- [ ] **3. Bound it.** Payload caps for OSC/DCS, parameter count caps,
  overflow-checked numeric parsing. Remote bytes are hostile.

- [ ] **4. Corpus tests in the same commit:** the happy path, default-param
  variant, malformed variants (bad chars, overlong, out-of-range params),
  interrupted mid-sequence (CAN, SUB, ESC), and split-across-chunks feeds.

- [ ] **5. Fuzz seed:** add a representative byte string to the fuzz corpus.

- [ ] **6. Ledger:** update the row in `docs/conformance.md` (status, tests,
  notes). Partial implementations say exactly what is missing.

- [ ] **7. Honesty:** if the sequence is query-able (DECRQM, DA, XTGETTCAP),
  wire it into the capability table so replies are generated, not hardcoded.

- [ ] **8. Audit + verify:** run the `vt-spec-auditor` subagent on the diff,
  then `/preflight`. Both must be clean before this counts as done.

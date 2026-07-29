---
paths:
  - "src/core/**"
---

# VT core rules (src/core/ — the sacred zone)

- **Zero dependencies.** No Qt, no network, no rendering, no OS-specific
  includes (utf8proc and the STL are the only allowed externals). The
  standalone `krait-core` target proves it; breaking that target is breaking
  the build.
- Parser is a table-driven Paul Williams DEC state machine. New sequences are
  new table entries + actions — never ad-hoc `if` chains in the byte loop.
- **Every sequence change ships, in the same commit:** corpus test cases
  (valid + malformed + interrupted-mid-sequence variants), a fuzz seed, and
  the `docs/conformance.md` row. `/add-escape-sequence` walks the procedure.
- **Width & clustering:** grapheme clusters via utf8proc + generated width
  tables + VS15/VS16 + the East-Asian-Ambiguous setting. Calling a bare
  `wcwidth`-style per-codepoint function is banned — reviewers reject on
  sight. Mode 2027 signals the behavior; the internal model is always
  cluster-based.
- **Grid stores logical lines + wrap points.** Reflow correctness has
  dedicated tests (resize during: wrapped lines, wide chars at boundary,
  active prompt). Any grid change runs them.
- **Honest replies:** DA1/DA2/DECRQM/XTGETTCAP answers are generated from the
  capability table that gates the implementation — the two cannot disagree by
  construction. Never hardcode a "yes".
- **Hostile input:** the parser must be allocation-free per byte on the hot
  path, bounded on all lengths (OSC/DCS payload caps), and fuzz-clean.
  A parser crash from remote bytes is a security bug, not a bug.
- Sync output (mode 2026) batches honor a ~150 ms timeout so a stuck client
  can never freeze rendering.
- Performance changes to the byte loop require a before/after run of the
  parser benchmark (`perf-auditor` subagent) in the PR description.

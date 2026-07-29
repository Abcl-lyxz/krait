---
name: benign-signatures
description: Log lines that look like failures during the Krait test/bench battery but are environmental noise, not project bugs — do not triage these
metadata:
  type: project
---

Three recurring lines show up in Krait build/test/bench logs on this machine and
are **not** defects. Confirmed 2026-07-29 during the M0 acceptance battery.

1. `'vswhere.exe' is not recognized as an internal or external command`
   Emitted on **stderr by `vcvars64.bat` / `VsDevCmd.bat`** (9 bare `vswhere`
   references inside VsDevCmd.bat; vswhere is not on PATH). It appears even in
   scripts that never mention vswhere. Proof: it appeared in a plain
   `cmake --preset dev` wrapper that contains no vswhere call at all.
   It does **not** indicate a problem with `tests/fuzz/run-smoke.cmd`'s own
   vswhere discovery — that uses the full quoted `%ProgramFiles(x86)%` path and
   resolves correctly.

2. `qt.qpa.screen: "Unable to open monitor interface to \\.\DISPLAY1:" "Unknown error 0xe0000225."`
   Printed by both flood-bench runs. The bench still completes and reports.
   Correlates with a locked/non-interactive desktop session.

3. `cl : Command line warning D9025 : overriding '/std:c++17' with '/std:c++23preview'`
   Real but pre-existing and non-blocking — see [[d9025-std-override]].

**Why:** each of these cost real triage time to attribute; two of them look like
toolchain breakage at a glance.

**How to apply:** when triaging a Krait build/bench log, filter these out first
and only investigate what remains. Never report #1 as a run-smoke.cmd bug.

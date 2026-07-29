---
name: battery-harness
description: How to actually execute the Krait M0 battery from a Bash-tool agent context — the cmd.exe invocation trap that silently produces empty logs
metadata:
  type: project
---

Running the Krait battery from the Bash tool (Git Bash) has one trap that
**fails silently and produces false results**.

**The trap:** `cmd.exe /c "some command"` from Git Bash does *not* run the
command. It prints the Windows banner plus an interactive prompt and exits 0.
Environment probes written this way return nothing and look like "variable not
set". I nearly reported wrong env facts from this on 2026-07-29.

**What works:**
- `cmd //c "..."` — for short inline commands.
- `MSYS_NO_PATHCONV=1 cmd.exe /c 'C:\abs\path\script.cmd'` — **single quotes**,
  so Bash does not eat the backslashes. Double quotes strip them
  (`C:UsersKla...`); `\"` escaping also fails.

**Passing arguments is not worth fighting.** Write a self-contained `.cmd` per
step into the scratchpad (vcvars + env + the one command) and invoke it with no
arguments.

**Env the presets need**, not set in the ambient environment — a wrapper must
export them: `VCPKG_ROOT=C:\vcpkg`, `QT_ROOT=C:\Qt\6.10.3\msvc2022_64`, plus
`vcvars64.bat` from the vswhere-resolved VS install. The `fuzz`/`fuzz-msvc`
presets need neither VCPKG_ROOT nor QT_ROOT, and `flood-report.cmd` defaults
QT_ROOT itself — only the `dev` preset needs both.

**Why:** a silently-empty log is worse than a failure; it reads as a pass.

**How to apply:** before trusting any `cmd`-mediated output, confirm the log is
non-empty and contains an expected marker line. See [[benign-signatures]] for
lines to ignore once it does run.

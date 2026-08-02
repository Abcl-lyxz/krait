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

4. **A leading `\` where a `//` comment belongs, in Grep tool output.** The Grep
   tool sometimes renders `// comment` as `\ comment` (seen 2026-07-30 on
   `src/core/grid/scrollback.cpp:36` and `:87`). This reads like a syntax error
   that could not possibly compile. Confirm with `sed -n '36p' file | cat -A`
   before reporting; in that case the file was clean and the build was green.
   Never report a compile error the build did not actually emit.

5. **An incremental build that prints only 2-4 AUTOMOC lines and exits 0.** This
   is an up-to-date no-op, not a full build — and it proves nothing about
   warnings, since `/W4 /WX` diagnostics only appear when objects actually
   compile. If the implementing session already built the branch, force the
   evidence: `ninja -C build\dev -t clean <new-targets>` then rebuild. A no-op
   build reported as "zero warnings" is a false pass.

6. **`build/dev/tests/unit/krait-render-tests.exe` (and its `*_tests.cmake`),
   frozen at 2026-07-30.** That target no longer exists in any `CMakeLists.txt`
   and `build/dev/tests/unit/CTestTestfile.cmake` includes only the `core` and
   `qt` test binaries, so ctest never touches it. It is an orphaned artifact of
   the test-binary split, not a suite that stopped running. Only the two
   binaries listed in that CTestTestfile carry signal; running the orphan by
   hand yields days-old results.

7. **`ninja: no work to do.` when the working tree has modified sources.** Not
   automatically the stale-binary trap from
   [[krait-stale-test-binary-false-green]] — it is genuine if the implementing
   session already built. Distinguish by mtime: every registered test exe in
   `build/dev/tests/unit/` must be *newer* than the newest file it compiles,
   and each new `TEST_CASE` name must appear in the ctest log. Both true = the
   binary is current. Note that a failed build never prints this line, since
   the broken object stays out of date.

**Why:** each of these cost real triage time to attribute; two of them look like
toolchain breakage at a glance.

**How to apply:** when triaging a Krait build/bench log, filter these out first
and only investigate what remains. Never report #1 as a run-smoke.cmd bug.

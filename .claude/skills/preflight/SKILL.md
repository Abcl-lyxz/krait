---
name: preflight
description: Configure, build, test, and lint Krait, then report a pass/fail table. Use before claiming any change works, before every commit, and after dependency or CMake changes. The project's definition of "verified".
allowed-tools: "Bash, Read, Grep, Glob"
---

Run the full verification gate and report honestly. "It should work" is not a
result — this is.

Steps, in order (stop at the first hard failure, report what passed):

1. **Phase check** — if `CMakePresets.json` does not exist, the project is
   pre-M0: report `preflight n/a (pre-scaffold phase)` and stop. Do not
   improvise a build.
2. **Configure** — `cmake --preset dev` only if the build dir is missing or
   CMake files changed since last configure; otherwise skip and say so.
3. **Build** — `cmake --build --preset dev`. On failure: report the FIRST
   error with file:line, stop.
4. **Tests** — if the working tree touched more than ~5 files, delegate to
   the `test-triager` subagent and use its summary; otherwise run
   `ctest --preset dev --output-on-failure` directly.
5. **Format** — `git diff --name-only HEAD` → for changed C++ files, run
   `clang-format --dry-run --Werror <files>`. (The post-edit hook should have
   kept this clean; a failure here means a file was edited outside the hooks.)
6. **Lint** — `clang-tidy` on changed C++ files only (full-repo tidy is CI's
   job), using the build dir's `compile_commands.json`.

Report format — exactly this table, then one line of verdict:

| Gate | Result | Detail |
|---|---|---|
| configure | ✔ / ✗ / skipped | ... |
| build | ✔ / ✗ | first error if any |
| tests | ✔ n passed / ✗ k failed | triage summary line |
| format | ✔ / ✗ | offending files |
| tidy | ✔ / ✗ | count + worst finding |

Verdict line: `PREFLIGHT GREEN` only when every gate passed. Anything else is
`PREFLIGHT RED — <shortest reason>`. Never report GREEN with a caveat; a
caveat means RED.

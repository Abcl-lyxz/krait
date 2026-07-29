---
name: d9025-std-override
description: Open issue — 14x D9025 /std:c++17 override warnings in krait-app; /WX does not catch D-series warnings so the build still passes
metadata:
  type: project
---

The Krait tree is `/W4 /WX`, but the M0 build is **not** warning-free:
14x `cl : Command line warning D9025 : overriding '/std:c++17' with
'/std:c++23preview'`. First measured 2026-07-29 on branch `t16-ci`.

Key fact: **`/WX` only promotes C-series (compilation) warnings to errors, not
D-series (command-line) warnings.** So this passes the build while violating
"clean build, no warnings". Do not assume exit 0 means warning-free — grep the
log for `warning` explicitly.

Scope: `krait-app` only (14/14). `krait-core` has zero. Cause is
directory-scoped: `qt_standard_project_setup()` in `src/app/CMakeLists.txt`
defaults the C++ standard to 17, so CMake emits `-std:c++17` (dash form — a
`/std:` grep will miss it) ahead of the global
`add_compile_options(/std:c++23preview ...)` in the root `CMakeLists.txt`.
`CMAKE_CXX_STANDARD` is a normal, not cached, variable — it is absent from
`CMakeCache.txt`; check `build/dev/build.ninja` for the flag instead.

**Why:** it was mistaken for a clean build once already; the dash-form flag and
the /WX blind spot both hide it.

**How to apply:** count `D9025` in every full build log and report it as a known
open item rather than re-triaging from scratch. Verify against current source
before recommending a fix — this may already be resolved. See
[[benign-signatures]].

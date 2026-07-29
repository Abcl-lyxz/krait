---
name: project-ci-batch-patterns
description: Review checklist for .github/workflows/*.yml and tests/**/*.cmd in Krait — green-but-blind gate holes, cmd.exe parsing facts verified on this machine
metadata:
  type: project
---

Recurring defect classes in Krait's CI gate and batch gate scripts.

**Why:** ADR-0008 makes `fast-gate` the required merge check and calls the
corpus tests "the moat". A gate that goes green while blind is worse than no
gate, so every review of `.github/workflows/` hunts silent-pass paths first.

**How to apply:** when a diff touches `.github/workflows/` or a `*.cmd` gate
script, walk this list before anything else.

## GitHub Actions `pwsh` steps — where failures get swallowed
- GitHub wraps `pwsh` steps with `$ErrorActionPreference='stop'` + a trailing
  `exit $LASTEXITCODE`. `$PSNativeCommandUseErrorActionPreference` defaults to
  `$false` (PS 7.4/7.5), so **native non-zero exits do NOT throw mid-script** —
  only the LAST native command's code gates.
- Krait's steps mostly survive this by luck: the meaningful check happens to be
  the last native command in each step. The real hole is the pattern
  `$x = git ...` → `if (-not $x) { exit 0 }`: a swallowed `git` failure becomes
  a green pass with zero files checked. Check every early `exit 0`.
- Cmdlet/CommandNotFound errors DO throw (ErrorActionPreference=stop). A step
  ending in `Write-Host` leaves `$LASTEXITCODE` from an earlier native call.

## Other green-but-blind traps seen here
- `ctest --preset dev` exits 0 when zero tests are registered (default
  `--no-tests=legacy`). Root `CMakeLists.txt` skips `tests/unit` + `tests/corpus`
  when `KRAIT_FUZZ=ON`, so this is one cache variable away from real.
- The `krait-core standalone (zero-dep proof)` step claims "no Qt on the path"
  but `install-qt-action` still exports `Qt6_DIR`/`QT_ROOT`; a future
  `find_package(Qt6)` in `src/core` would resolve and the sacred-rule-1 proof
  would pass green. See [[project_review_patterns]].

## cmd.exe facts verified empirically on this machine (do not re-litigate)
- `if <cond> cmd1 & cmd2` binds the WHOLE remainder to the if body — `& exit /b 1`
  is conditional, not unconditional. Verified both branches.
- `%ProgramFiles(x86)%` is safe on a plain `set` line; only needs hoisting when
  moved inside a parenthesised `if (...)`/`for (...)` block.
- `for /f "usebackq" ... in (\`"%VSWHERE%" -latest ...\`)` with the embedded
  quoted path is correct (Microsoft's documented vswhere form) — no quote
  mangling, no stderr.
- `if defined VCINSTALLDIR` does NOT prove x64: `vcvars32.bat` sets
  `VCINSTALLDIR` too, plus `VSCMD_ARG_TGT_ARCH=x86`, `Platform=x86`, and
  Hostx86/x86 `cl` on PATH. Gate on `VSCMD_ARG_TGT_ARCH`, never `VCINSTALLDIR`.

## Facts about this repo's runner
- `runs-on: windows-2025-vs2026` resolves (jobs start); no self-hosted runners.
- ADR-0008 says `windows-latest` is already Server 2025 + VS2026 18.4, which
  contradicts the ci.yml comment claiming it is "still VS2022 17.14" — one of
  the two is stale whenever you read them.

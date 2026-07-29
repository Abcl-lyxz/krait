# ADR-0010: Fuzzing = clang-cl libFuzzer primary; MSVC sanitizers nightly

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner), verification 2026-07-29

## Context

The parser fuzz target is mandatory from M0 (ADR-0003). Verified: MSVC's
`/fsanitize=fuzzer` is officially *experimental* (VS2022 17.0+); MSVC ASan is
supported but incompatible with /RTC and incremental linking, and
fuzzer+ASan must be passed as separate flags. clang-cl (LLVM for Windows)
provides mature `-fsanitize=fuzzer,address` and is the documented community
path for serious Windows fuzzing.

## Decision

- Primary fuzz build: **clang-cl** preset (`fuzz`) with
  `-fsanitize=fuzzer,address` on `krait-core` only (no Qt in the target).
- Per-commit: 60 s smoke + replay of `tests/fuzz/regressions/`.
- Nightly: 30 min clang-cl run + MSVC ASan test-suite pass + MSVC
  `/fsanitize=fuzzer` variant (informational, tracks MSVC maturity).
- Seeds = every corpus `IN` payload; every crash becomes a committed
  regression seed.

## Alternatives considered

- MSVC-only fuzzing → experimental status, weaker mutation performance.
- WSL/Linux fuzzing of krait-core → viable extra coverage later (core is
  platform-clean), but MSVC-compiled behavior is what ships; not primary.

## Consequences

Two compilers in CI (MSVC + LLVM); clang-cl must consume the same CMake
targets — kept honest by the standalone krait-core target. Revisit trigger:
MSVC libFuzzer leaving experimental status (collapse to one toolchain), or
OSS-Fuzz acceptance (add Linux builds).

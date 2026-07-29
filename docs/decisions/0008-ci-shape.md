# ADR-0008: CI = GitHub Actions, fast merge gate + heavy nightly

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner), plan-session interview + verification

## Context

`windows-latest` is Windows Server 2025 with VS2026 18.4 (verified
runner-images #14017, 2026-06). Windows runners are slow; a merge gate that
takes an hour gets bypassed under pressure, which is worse than a smaller
gate. Qt installs are fast with install-qt-action caching.

## Decision

- **fast-gate (required for merge, target <15 min):** configure + build
  (MSVC), unit + corpus + conpty-contract + golden-image tests, 60 s fuzz
  smoke, clang-format check, clang-tidy on changed files, krait-core
  standalone proof.
- **nightly-heavy:** MSVC ASan suite, 30 min clang-cl fuzz, bench vs
  baselines (>5% regression fails), full clang-tidy, informational esctest run.
- Branch protection: fast-gate required; main is always releasable.
- Matrix detail lives in `docs/plan/03-test-strategy.md` §7.

## Alternatives considered

- Everything per-commit → 45–60 min merges; discipline erosion.
- Minimal build-only gate → corpus tests are the moat; they must gate.

## Consequences

A regression introduced during the day can land and be caught at night —
accepted; nightly failures are next-morning P1s. Revisit trigger: fast-gate
exceeding ~20 min (split or cache harder) or team growth changing merge
patterns.

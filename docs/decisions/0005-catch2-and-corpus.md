# ADR-0005: Catch2 v3 test framework; corpus format; esctest GPL boundary

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner), plan-session interview + verification

## Context

The test load is dominated by data-driven VT corpus cases plus ordinary unit
tests; mocking needs are minimal (backend fakes are hand-written). Catch2
v3.15.3 and GoogleTest 1.17.0 are both current in vcpkg (verified 2026-07-29);
Catch2's SECTION/GENERATE fits data-driven cases, and `catch_discover_tests`
registers into CTest. License check on corpus sources: vttest is BSD;
esctest is GPL-2.0 (github.com/gnachman/esctest LICENSE).

## Decision

- Catch2 v3 via vcpkg; all tests discovered into CTest (`ctest --preset dev`).
- Corpus format: `tests/corpus/**/*.case` golden files (`IN` bytes →
  `EXPECT` events/state), defined in T4 and documented in
  `docs/plan/03-test-strategy.md`.
- vttest-derived cases are allowed (BSD). **esctest-derived cases are
  forbidden**: esctest runs only as an external conformance harness against a
  built binary; its code and cases never enter `tests/`.

## Alternatives considered

- GoogleTest + gmock → fine, slightly clunkier parameterized cases; heavier
  mocking we don't need.
- doctest → smaller community, no advantage over Catch2 here.

## Consequences

One framework everywhere; corpus cases stay MIT-clean. If heavy mocking ever
appears, hand-written fakes first. Revisit trigger: compile-time cost of
Catch2 measurably hurts the fast gate (then split test binaries).

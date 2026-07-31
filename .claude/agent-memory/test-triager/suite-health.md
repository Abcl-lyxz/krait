---
name: suite-health
description: Krait suite stability baseline — no flaky tests known as of 2026-07-30; ctest counts per task, plus the scrollback test that eats 94% of suite runtime
metadata:
  type: project
---

**No flaky tests known.** Through 2026-07-30 (branch `t23-shaper`) no Krait test
has been observed failing intermittently. If one does, record it here with
evidence and a ticket rather than re-running until green.

## ctest `dev` count history

| Point | Tests | Wall time |
|---|---|---|
| T16 (`t16-ci`, 2026-07-29) | 22 | 0.48 s |
| pre-T23 baseline | 89 | — |
| T23 (`t23-shaper`, 2026-07-30) | 100 | 44.01 s |
| T36 (`t36-backend-seam`) | 278 | — |
| T52 (`t52-backend-factory`, 2026-07-31) | 284 | 38.00 s |

T52 added exactly 5 `[factory]` tests; the 6th of the 278→284 delta came from
commits landed between t36 and HEAD, not from T52 — don't attribute the whole
delta to the branch under test. `grep -c TEST_CASE tests/**.cpp` reads 285 vs
ctest's 284 because of the `[.]`-hidden bench case below.

T23 added exactly 11 registered tests from `tests/unit/shaper_test.cpp`. A 12th
`TEST_CASE("shaping throughput", "[.][bench]")` is **hidden** by the `[.]` tag,
so `catch_discover_tests` never registers it — **ctest provides zero perf
coverage for the shaper**. Its numbers live in `bench/baselines/t23-shaper.json`
and only a manual run or `perf-auditor` touches them. Do not report 100 vs 12
TEST_CASEs as a missing test.

## The 41-second test (not a failure, not T23) — now ~29 s

`scrollback: a window read does not rewrap an endless line`
(`tests/unit/scrollback_test.cpp:103`) takes **41.4 s of the 44 s total — 94% of
suite runtime**. It passes.

Root cause is `shrink_to_fit()` in the continuation-append hot path at
`src/core/grid/scrollback.cpp:47`: once the glued logical line reaches the
200,000-cell cap, *every* later `push()` does `resize(cap)` + `shrink_to_fit()`,
reallocating and copying the whole ~4 MB vector (20 B/Cell). The test pushes
30,001 rows x 10 cells, so the cap is hit near push ~20,000 and the remaining
~10,000 pushes memcpy roughly 40 GB. Introduced with T21 (733b4c3); confirmed
pre-existing, not a T23 regression, via an empty `git diff HEAD -- src/core/`.

Re-measured 2026-07-31 (T52): **29.36 s of a 38.00 s total — 77%**. Still the
single dominant test by a wide margin (next slowest is 0.77 s). The absolute
drop from 41 s is unexplained by any `src/core/` change; treat 29-42 s for this
one test as the normal band rather than evidence of a fix or a regression.

**How to apply:** expect ~38-44 s for a green run and do not treat it as a hang.
If total time jumps well past that, check this test first. The `shrink_to_fit`
call is deliberate (it bounds memory, not just the accounting) so any fix must
keep the memory bound — amortize it, don't delete it.

**Flood bench variance** (600 frames, 4K, 240x63 grid — grid dims are compile-time
constants in `src/render/spike/grid_item.h`, unrelated to the `80x22` ConPTY line
in the log): dev GPU 180.0 / 180.1 fps (vsync-capped at a 180 Hz display), WARP
152.4 / 148.2 fps. Treat a WARP swing beyond roughly +/-5 fps as signal.
WARP measures ~1.9x faster than `bench/baselines/m0-spike.json` (80.7 fps)
reproducibly, so that baseline is stale — do not silently re-record it.

**Fuzz smoke** (`tests/fuzz/run-smoke.cmd`): selects `fuzz-msvc`, never `fuzz`,
because **clang-cl is not installed on this machine**. ADR-0010's primary
clang-cl path is therefore *unexercised* here. Two 60 s runs: 261,881 execs
@ 4,293/s and 267,776 @ 4,389/s, zero crashes. `tests/fuzz/regressions/` holds
only README.md, so the regression-replay block is skipped.

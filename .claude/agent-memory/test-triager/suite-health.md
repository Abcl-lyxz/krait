---
name: suite-health
description: Krait suite stability baseline — no flaky tests known as of 2026-07-29; measured run-to-run variance for ctest, fuzz and the flood bench
metadata:
  type: project
---

Stability baseline measured 2026-07-29, branch `t16-ci`, M0 acceptance battery.

**ctest `dev`: no flaky tests known.** 22/22 passed, 0 failed, 0.48 s total.
No test has yet been observed failing intermittently. If one does, record it
here with evidence and a ticket rather than re-running until green.

**Fuzz smoke** (`tests/fuzz/run-smoke.cmd`): selects `fuzz-msvc`, never `fuzz`,
because **clang-cl is not installed on this machine** — absent from PATH even
after vcvars. ADR-0010's primary clang-cl path is therefore *unexercised* here;
only the MSVC `/fsanitize=fuzzer` fallback is being tested. Two 60 s runs:
261,881 execs @ 4,293/s and 267,776 execs @ 4,389/s, zero crashes both times.
`tests/fuzz/regressions/` holds only README.md, so the regression-replay block
is skipped (`if exist *.bin` is false).

**Flood bench variance** (600 frames, 4K, 240x63 grid — grid dims are compile-time
constants in `src/render/spike/grid_item.h`, unrelated to the `80x22` ConPTY line
in the log): dev GPU 180.0 / 180.1 fps (vsync-capped at a 180 Hz display), WARP
152.4 / 148.2 fps. Run-to-run spread is small; treat a WARP swing beyond roughly
+/-5 fps as signal.

Note: WARP measures ~1.9x faster than `bench/baselines/m0-spike.json` (80.7 fps
recorded). Reproducible across runs, so it is not noise — the baseline was
recorded under different display/vsync conditions. Favorable, so it does not
block the gate, but the baseline is stale.

**How to apply:** use these numbers as the comparison point for the next run;
flag regressions against them, and do not silently re-record the baseline.

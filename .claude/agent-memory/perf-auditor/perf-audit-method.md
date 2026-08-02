---
name: perf-audit-method
description: How perf audits must be run here — release preset only, never touch baselines, never improvise a substitute bench, and attribute via an in-session A/B against main
metadata:
  type: feedback
---

Standing rules for every perf audit in this repo.

**Measure with the `release` preset, never `dev`.**
**Why:** `rules/render.md` makes the budgets release gates, and a Debug flood
measures the compiler — T25's Debug leg read 51 fps against code that does 140
optimised.
**How to apply:** `cmake --preset release && cmake --build --preset release`,
then run out of `build\release\`.

**Never re-record or edit `bench/baselines/*.json`.** Recommending a
re-record (and only after the change merges) is the most that is allowed.

**Never improvise a substitute benchmark.** If the bench infrastructure for a
baseline does not exist or cannot run, say so plainly and stop — a made-up
micro-benchmark is worse than a gap, because it reads like evidence.

**Attribute every delta with an in-session A/B against `main`.**
**Why:** the committed baselines are days old and this machine moves ~36%
between sessions ([[machine-homecenter-baselines]]), so a raw
baseline-vs-branch delta cannot tell "the branch got slower" from "the machine
did".
**How to apply:** `git worktree add <scratch> main`, build it release, run both
binaries interleaved, and report the in-session delta as the verdict number
while still showing the raw baseline delta. Use a worktree rather than
stash/checkout — this repo often has concurrent agents committing to the branch
mid-audit (during this audit an uncommitted `frame_builder.cpp` change was
committed out from under the working tree), and touching the shared tree risks
their work.

**Never call a regression "probably noise" without showing the 3-run medians.**
Three runs minimum, median reported; on this machine prefer ~6 warm runs.

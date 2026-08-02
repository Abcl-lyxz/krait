---
name: regression-history
description: Past perf verdicts and what actually caused each apparent regression — so far every one has been environment, not code
metadata:
  type: project
---

Audited verdicts, newest first. Keep the *mechanism*, not the number.

**T84 (graphics: sixel/kitty/OSC 66), branch `t84-m6-platform`, 2026-08-02 —
PASS.**
Raw comparison against `m2-wrap.json` looked catastrophic (cpu_avg 5.63 →
7.843 ms, +39%; fps 177.6 → 127.5). It was **not the branch**: `main` built
and run in the same hour measured 7.652 ms, +36% against the same baseline.
In-session interleaved A/B, 11 runs each, gave cpu_avg +2.50% all-runs and
−1.85% warm-only — inside the 5% gate.
The five suspect paths all proved free on a text-only frame, as designed:
`appendImages()` early-returns before any allocation (the flood has zero
placements); `buildRow`'s `shaped.scale` branch is per-RUN, not per-glyph, and
the flood makes ~1 run/row; the `m_sizedGlyphs` third flatten inserts empty
ranges; `run_splitter`'s `cell.attr.scale()` is a shift+mask on an `attr` word
already loaded for `shapingBits()`; `GpuResources::sync`'s new blocks are
behind `.empty()` guards. Undocumented extra: `terminal_item.cpp` runs a
`std::ranges::any_of` over every run each frame until sized text first appears
— linear in run count, measured free at this scale, but it is per-frame work
that never stops on a session that never uses OSC 66.

**Pattern to carry forward:** the tell that an apparent regression is
environmental is a *GPU-time* move on a workload where the diff adds no GPU
work. T84's `gpu_avg_ms` rose 24–29% against baseline on a text-only flood
that its changes cannot touch — that was the first clue, and the `main` A/B
confirmed it. See [[machine-homecenter-baselines]] and [[perf-audit-method]].

---
name: machine-homecenter-baselines
description: HomeCenter perf quirks — the WARP flood baselines are vsync-pinned ceilings, the machine drifts ~36% between sessions, and the hardware D3D11 leg still cannot be measured
metadata:
  type: project
---

Per-machine baseline quirks for **HomeCenter** (i7-14700F, RTX 4060, Win 11
10.0.26200, Qt 6.10.3, MSVC 19.51). All of these change how
`bench/baselines/*.json` must be read.

**1. `cpu_avg_ms` in the flood harness is frame WALL time, not CPU work.**
It equals `1000/fps` in every run I have checked: baseline `m2-wrap.json` has
fps 177.6 and cpu_avg 5.63 (1000/177.6 = 5.631); a 2026-08-02 run had fps
127.5 and cpu_avg 7.843 (1000/127.5 = 7.843).
**Why:** the number includes vsync pacing, which the baseline files say
themselves. So when a run is vsync-bound the metric measures the panel, not
the renderer.
**How to apply:** if fps is at/near the refresh rate, `cpu_avg_ms` is a
CEILING and a "regression" against it is meaningless. m1-wrap (180.0 fps) and
m2-wrap (177.6 fps) on a 179–180 Hz panel are both pinned — neither records
what the renderer actually cost.

**2. The machine drifts a lot between sessions — always A/B in-session.**
On 2026-08-02 an unmodified `main` measured 7.652 ms median on the identical
flood that `m2-wrap.json` recorded at 5.63 ms on 2026-07-31: **+36% with zero
code change.** Related: point 1 — the baseline day was vsync-capped and this
day was compute-bound at ~127 fps, so the two were never measuring the same
thing.
**How to apply:** never attribute a delta to a branch from the committed
baseline alone. Build `main` into a scratch `git worktree`, run both
interleaved, and report the in-session delta as the gate number. See
[[perf-audit-method]].

**3. The hardware D3D11 leg still cannot be measured (as of 2026-08-02).**
`KRAIT_GPU=hardware` exits 2 (the 60 s watchdog) with no frames on BOTH
`main` and the feature branch — and now it does so even with a 179 Hz display
attached, which was the explanation m1/m2 recorded for the same failure.
**Why:** whatever blocks the present is not "no monitor attached". Unknown.
**How to apply:** it is pre-existing, not a branch regression — verify on
`main` before reporting it as one. The last real hardware number is still
T25's 140.7 fps, now four milestones stale.

**4. First runs of a freshly linked binary are slow — discard or interleave.**
A newly built `krait-app.exe` ran 8.678 ms on its first flood and converged to
~7.55 ms by the sixth. The shaper `[bench]` is worse: first run 2.8 ms,
warm ~0.85 ms (3x).
**How to apply:** budget ~6 runs per binary and compare warm-state medians, or
interleave the two binaries round-robin so warm-up and thermal drift hit both
equally. Three runs straight off a fresh link is not enough on this machine.

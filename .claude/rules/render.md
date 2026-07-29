---
paths:
  - "src/render/**"
---

# Renderer rules (src/render/ — the performance zone)

- **Damage-tracked always.** Render only dirty cells/rows; a full-frame
  redraw outside resize/theme-change is a defect. Never render per parsed
  byte — parse chunks, coalesce damage, present at vsync.
- **Budgets (release gates, checked by `perf-auditor` against
  `bench/baselines/`):** 60 fps under `cat`-flood on the reference machine;
  renderer share of input latency < 10 ms; >5% regression on any baseline
  fails.
- **Glyph atlas:** LRU eviction + growth caps; eviction under pressure must
  be tested. Shaping runs on the worker pool, cached per run — the render
  thread never calls HarfBuzz.
- **Survive the GPU:** D3D device-lost/reset recovery path is mandatory and
  tested (fake-lost harness); WARP/software fallback for RDP and VMs;
  per-monitor DPI change mid-session without restart or blur.
- Shaders live in `src/render/shaders/` and compile via Qt's qsb pipeline;
  no inline shader strings.
- Color management: everything internal is linear-light; convert once at the
  edges. Theme colors are data, never literals in render code.
- IME overlay: composition string + candidate window positioning comes from
  the renderer's cell metrics — any metrics change re-runs the IME
  positioning tests (Thai + Japanese).

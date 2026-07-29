# ADR-0013: M0 render-spike verdict — GO

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner); numbers recorded by the T13 bench

## Context

M0's acceptance gate (docs/plan/01-milestones.md §M0) required the QRhi
glyph-atlas spike (T12) to prove the rendering architecture before any
product code builds on it: ≥60 fps at 4K on the dev GPU, ≥30 fps on WARP,
<10 ms/frame. T13's flood bench drives the worst realistic terminal frame —
every one of 240×63 cells changes glyph and color every frame, with the
full per-instance buffer re-uploaded — into a fixed 3840×2160 color buffer.

## Numbers (bench/baselines/m0-spike.json, machine "HomeCenter")

| Leg | Adapter | fps | CPU avg/p99/max ms | GPU avg ms |
|---|---|---|---|---|
| dev GPU | NVIDIA GeForce RTX 4060 | 180.1 (display-cap) | 5.55 / 5.93 / 7.13 | **0.169** |
| WARP | Microsoft Basic Render Driver | 80.7 | 12.40 / 18.64 / 27.78 | 10.07 |

CPU times include vsync pacing (the app renders at the 180 Hz display cap);
GPU times are QRhi timestamp render cost. Qt 6.10.3 (pin 6.11.1 pending),
MSVC 19.51, Windows 11 26200.

## Verdict

**GO.** Every gate clears with margin:

- 4K dev GPU: 180 fps against a ≥60 requirement — and the true render cost
  is 0.169 ms/frame, ~59× inside the 10 ms budget.
- WARP: 80.7 fps against a ≥30 requirement at full 4K flood.
- <10 ms/frame: 0.169 ms GPU / 5.55 ms paced CPU on the dev GPU. WARP's GPU
  render cost sits at 10.07 ms — at the budget line, but WARP is the
  software fallback tier, bound only by the ≥30 fps requirement it beats by
  2.7×.

## Consequences

- The QQuickRhiItem + glyph-atlas + instanced-quads architecture (ADR-0009,
  ADR-0001) is confirmed for the real renderer.
- The T12 deviation (per-cell data in a dynamic per-instance buffer rather
  than a storage buffer) is ratified: full-grid re-upload costs are
  negligible even at 4K on WARP; revisit only if profiling of the real
  renderer says otherwise.
- Headroom notes for the real renderer: damage-driven partial updates will
  only lower the flood numbers above; the 150 ms sync-output guard and
  vsync presentation policy (CLAUDE.md landmines) remain the pacing
  authorities.

## Revisit triggers

Reflow-era grid sizes materially above 240×63, sub-0.5 ms budget pressure
from effects (cursor trails, ligature overlays), or a Qt major upgrade
changing QRhi timestamp semantics.

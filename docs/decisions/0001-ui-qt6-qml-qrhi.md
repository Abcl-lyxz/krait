# ADR-0001: Qt 6 QML chrome + custom QRhi glyph-atlas terminal renderer

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner), research in `docs/research/landscape-2026.md §4`

## Context

The product promises a beautiful, deeply customizable UI (searchable settings,
theme editor, session tree) *and* GPU-class terminal rendering with correct
Thai/CJK/emoji shaping and IME. Building all chrome by hand (Windows Terminal
approach) costs months before the first settings page; Electron-class stacks
are disqualified by RAM/latency (see Tabby complaints). Qt 6's QRhi is a
public GPU abstraction (D3D11/12, Vulkan, Metal, GL) since 6.6, and Qt proved
GPU text on QRhi with Canvas Painter (6.11 tech preview). No shipping terminal
uses QRhi yet — first-mover risk is ours.

## Decision

- Qt 6, **latest open-source release line** (6.10/6.11 at time of writing) —
  not 6.8 LTS, whose patch releases are commercial-only.
- Chrome (tabs, panes, palette, settings, dialogs) in **QML**; view-models in
  C++. Business logic never in QML.
- The terminal view is a **custom QRhi renderer** (glyph atlas + damage
  tracking, D3D11 primary, the AtlasEngine pattern), embedded in the QML
  scene. Text shaping via **HarfBuzz + FreeType**, with **DirectWrite used
  only to enumerate system fonts / build the fallback chain** (WezTerm's
  recipe — best proven path for Thai + emoji + Nerd Fonts, stays portable).
- Qt installed via aqtinstall; LGPL compliance via dynamic linking.

## Alternatives considered

- Pure Win32 + D3D11 + DirectWrite (WT clone) → maximal control, but months
  of chrome work, weak settings-UI velocity, locks shaping into Windows.
- Dear ImGui → fast iteration, but shaping/IME/Thai and polished settings UX
  are all DIY; product would look like a dev tool.
- QPainter/QWidget rendering → simplest, but CPU-bound at 4K/144 Hz and can't
  hit flood-throughput budgets.
- Electron/Tauri webview → RAM/latency reputation is exactly what we're
  positioning against; and the project is C++ by charter.

## Consequences

- M0 must include a **renderer spike with a 2-week budget gate**: glyph atlas
  + damage rects + `cat` flood benchmark on QRhi/D3D11. Fallback if the gate
  fails: QPainter path for correctness while the renderer is optimized —
  correctness tests must not depend on the GPU path.
- We own device-lost recovery, per-monitor DPI, WARP fallback (rules/render.md).
- IME integration goes through Qt's QInputMethod — verify composition
  positioning APIs against qt-docs MCP during planning.
- Revisit trigger: QRhi proves unable to hit latency/throughput budgets in
  the M0 spike, or Qt licensing terms change materially.

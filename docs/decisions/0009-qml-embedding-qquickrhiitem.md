# ADR-0009: Terminal view embeds via QQuickRhiItem; WARP is the software path

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner), qt-docs verification 2026-07-29

## Context

The custom QRhi renderer (ADR-0001) must live inside the QML scene. Verified
against Qt 6.11 docs: **QQuickRhiItem** (since 6.7) is the supported
graphics-API-agnostic path — `createRenderer()` returns a
`QQuickRhiItemRenderer` whose `initialize/synchronize/render` run on the
scene-graph render thread (`synchronize()` with the GUI thread blocked).
QQuickFramebufferObject is OpenGL-only. The RhiWindow example is a standalone
QWindow, not embeddable. QSGRenderNode renders inline in the scenegraph pass
(no extra render target) but carries documented restrictions (no depth
writes, no resource copies in render(), render-target changes force pipeline
rebuilds). QQuickRhiItem is documented as **not functional on Qt's software
scenegraph backend**.

## Decision

- Terminal view = QQuickRhiItem subclass; all QRhi resources owned by its
  renderer; item↔renderer state crosses only in `synchronize()`.
- Graphics API pinned D3D11 via `QQuickWindow::setGraphicsApi()`.
- Software fallback = **D3D11 WARP adapter**, never Qt's software backend.
- QSGRenderNode is the recorded escape hatch if the M0 spike shows the
  texture-pass cost breaking the <10 ms budget.

## Alternatives considered

- QSGRenderNode → faster fill path, but restriction-laden; only on evidence.
- QQuickFramebufferObject → GL-only; disqualified.
- Separate QRhiWindow → loses QML chrome composition; disqualified.

## Consequences

Renderer inherits Qt's render-thread model (thread table in
docs/plan/00-architecture.md §2). QRhi has no source/binary compat guarantee →
Qt version pinned (6.11.1), bumps are deliberate PRs. Revisit trigger: spike
numbers failing gate (→ QSGRenderNode or QPainter per ADR-0001), or Qt
promoting a better embed API.

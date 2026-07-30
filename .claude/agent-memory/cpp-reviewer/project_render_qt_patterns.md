---
name: project-render-qt-patterns
description: src/render + src/app QRhi/QQuickRhiItem review checklist for Krait — recurring defect shapes, verified Qt 6.10 facts, hot files
metadata:
  type: project
---

Checklist for any diff touching `src/render/` or `src/app/terminal_item.*`.

**Why:** the same four defect shapes have now shown up across T11/T12/T13/T25/T26.
**How to apply:** run this list before reading the diff's own comments — the
comments in this codebase are long and persuasive and will anchor you.

## Recurring defect shapes (seen more than once)

1. **Item lifecycle hooks fire before geometry exists.** `itemChange`,
   `ItemSceneChange`, `componentComplete` all run while `width()/height()==0`
   (anchors apply at componentComplete, after parent/window is set). Anything
   that derives cols/rows and then calls `ensureStarted()` will spawn the
   ConPTY at the `std::max(2, ...)` floor — 2x2. Guard `updateGrid()` on real
   geometry, not on the hook.
2. **Two independent computations of the same number.** The item derives the
   colour-buffer size by hand while `render()` uses
   `renderTarget()->pixelSize()`. Any divergence mis-scales the shader divisor
   vs the viewport => blur. Qt already exposes
   `QQuickRhiItem::effectiveColorBufferSize()` + `effectiveColorBufferSizeChanged`
   (verified in qquickrhiitem.h:91,104, Qt 6.10.3). Prefer it over re-derivation.
3. **Sticky "needs upload" flags cleared on a partial upload.** The atlas
   handover condition in `TerminalRenderer::synchronize` keys on
   grew/dirty/hasAtlas, but `rebuildFrame()` runs many times per presented
   frame and `GlyphAtlas::takeGrew()` CONSUMES the flag — so the frame the
   render thread sees can report "not grown, not dirty" after a growth. Any
   clamp that silently shortens an upload must leave the flag set.
4. **`m_failed`-style sticky error state** with no path back except a device
   change. Repeated since T11/T12; check every new one.

## Verified Qt 6.10.3 facts (from the installed headers, not memory)

- `QQuickItem::ItemDevicePixelRatioHasChanged` exists; payload is
  `value.realValue`. `ItemSceneChange` payload is `value.window`.
  (qquickitem.h:145-172)
- `QQuickRhiItemNode` owns the renderer: `std::unique_ptr<QQuickRhiItemRenderer>
  m_renderer` (qquickrhiitem_p.h:69), and `QQuickRhiItem::releaseResources()` /
  `invalidateSceneGraph()` drop the node. **Consequence: on a real D3D
  device-lost the whole `QQuickRhiItemRenderer` subclass is destroyed and
  rebuilt** — a "QRhi pointer changed" branch inside a surviving resource object
  is NOT the production recovery path, whatever the docs' "be prepared that the
  QRhi may change" line suggests. Say so when someone claims render.md's
  device-lost requirement is tested.
- Threading: `synchronize()`, `initialize()` and `render()` all run on the
  render thread; only `synchronize()` has the GUI thread blocked. So a member
  touched by all three needs no lock, but anything the GUI thread also touches
  must be confined to `synchronize()`.
- Project convention for `windows.h`: `#define WIN32_LEAN_AND_MEAN` +
  `#define NOMINMAX` first (fontdb.cpp, shape_pool.cpp). `main.cpp` and
  `conpty_backend.h` do not follow it.

## Hot files

`src/app/terminal_item.cpp` (largest, most-reviewed, mixes GUI-thread grid
logic with render-thread code in one file), `src/render/gpu_resources.cpp`,
`src/render/atlas/glyph_atlas.cpp` (width is FIXED at 2048, height doubles;
`takeGrew()` is consuming), `src/app/main.cpp`.

Related: [[project-watch-items]], [[project-review-patterns]].

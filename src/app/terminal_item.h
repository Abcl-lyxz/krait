#pragma once

#include "../net/conpty/conpty_backend.h"
#include "core/terminal/session.h"
#include "input/ime.h"
#include "input/mouse.h"
#include "input/paste.h"
#include "render/atlas/glyph_atlas.h"
#include "render/frame_builder.h"
#include "render/gpu_resources.h"
#include "render/ime_metrics.h"
#include "render/shaper/fontdb.h"
#include "render/shaper/shape_pool.h"
#include <rhi/qrhi.h>

#include <QElapsedTimer>
#include <QQuickRhiItem>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace krait::app {

// The real terminal view (plan T25): ConPTY -> core Session -> run splitting ->
// the HarfBuzz shaper pool -> the glyph atlas -> two instanced QRhi draws.
// Replaces the M0 spike path, which rendered ASCII from a fixed 95-cell strip
// and could only show one CJK character as '?'.
//
// Two pipelines, not one: solid rectangles (backgrounds, selection, cursor,
// underlines) and textured glyph quads. A glyph is NOT a cell — it carries the
// shaper's offsets and the font's bearings — so the spike's
// one-instance-per-cell model could never place a Thai mark or a ligature.
class TerminalItem : public QQuickRhiItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(TerminalView)

  public:
    TerminalItem();
    ~TerminalItem() override;

    QQuickRhiItemRenderer* createRenderer() override;

    // Snapshot handed to the renderer in synchronize(). Copied rather than
    // shared: the render thread must not walk the grid while the GUI thread is
    // feeding bytes into it.
    const render::FrameData& frame() const { return m_frame; }

    // The atlas pixels, for the renderer to upload. Borrowed, valid until the
    // next rebuildFrame() on the GUI thread — synchronize() runs with the GUI
    // thread blocked, which is the whole reason that is safe.
    const std::vector<std::uint8_t>* atlasPixels() const;

    int benchFrames() const { return m_benchFrames; }

    // Called (queued) from the renderer when a flood bench completes.
    Q_INVOKABLE void finishBench(const QString& reportJson);

    // One bench frame's worth of grid churn, driven from the renderer so the
    // flood runs at presentation rate — the same way the M0 baseline was taken.
    Q_INVOKABLE void stepBench(int frame);

    // Dumps the atlas to a PNG for the golden-image gate.
    Q_INVOKABLE void dumpAtlas(const QString& path) const;

    // Paste, guarded (T28). Reads the clipboard, sanitises it, and either sends
    // it or raises pasteConfirmRequested. QML calls this from the paste action.
    Q_INVOKABLE void paste();

    // Answers a pending confirmation. `allow` false discards the paste.
    Q_INVOKABLE void resolvePaste(bool allow);

  signals:
    // rules/ui.md: a per-tab banner, never an app-modal dialog. `detail` is the
    // first line of what would be sent, so the user can see what they are
    // agreeing to without leaving the terminal.
    void pasteConfirmRequested(const QString& message, const QString& detail);

  protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    // ItemDevicePixelRatioHasChanged: the only hook Qt gives for a per-monitor
    // DPI change (there is no QWindow::devicePixelRatioChanged signal).
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    // T29. inputMethodQuery answers WHERE the candidate window goes; without it
    // the IME guesses, and on Windows that means the top-left of the screen.
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

  private:
    void handleOutput(const QByteArray& bytes);
    void ensureStarted();
    bool ensureFont();
    void rebuildFrame();
    // Re-rasterises the font stack at `dpr` and reflows the grid. A DPI change
    // is a font change: the glyphs are baked at a fixed pixel size, so anything
    // short of re-rasterising them is a scaled bitmap, i.e. blur.
    void applyDevicePixelRatio(qreal dpr);
    // Re-derives the grid from the colour buffer size and the cell metrics.
    // Shared by a resize and a DPI change, which differ only in what moved.
    void updateGrid();
    // The colour buffer's size in device pixels — what the shaders divide by.
    int bufferWidth() const;
    int bufferHeight() const;
    // Viewport row/col under a widget-space point, clamped into the grid.
    void cellAt(const QPointF& pos, int& row, int& col) const;
    // Sends a mouse report if the application asked for one. Returns whether it
    // did — false means the event is still OURS (selection, viewport scroll).
    bool reportMouse(input::MouseAction action, Qt::MouseButton button,
                     Qt::MouseButtons buttonsDown, Qt::KeyboardModifiers mods, const QPointF& pos,
                     int wheelSteps);
    // Puts the current selection on the clipboard. No-op without one.
    void copySelection();
    // Sends already-sanitised paste bytes and snaps the viewport back.
    void sendPaste(const QByteArray& bytes);
    // Appends the in-flight composition to the frame. A preedit is not grid
    // content — it belongs to the IME until it commits — so it is drawn OVER
    // the frame rather than written into the grid.
    void appendComposition();

    std::unique_ptr<render::FontDb> m_fonts;
    std::unique_ptr<render::ShapePool> m_pool;
    std::unique_ptr<render::GlyphAtlas> m_atlas;
    std::unique_ptr<render::FrameBuilder> m_builder;
    std::unique_ptr<core::vt::Session> m_session;
    net::ConptyBackend* m_backend = nullptr;  // owned by this (QObject parent)

    render::RasterFn m_raster;
    std::string m_family;
    std::uint32_t m_primaryFace = 0;
    // Logical (DPI-independent) font size; m_pxHeight is that scaled to the
    // current device pixel ratio, and is what FreeType actually rasterises at.
    int m_basePxHeight = 20;
    int m_pxHeight = 20;
    qreal m_dpr = 1.0;
    bool m_ligatures = false;

    // Scratch reused across frames so a steady-state frame allocates little.
    std::vector<render::Run> m_runs;
    std::vector<render::ShapedRun> m_shaped;
    std::vector<std::uint32_t> m_faces;
    std::vector<core::vt::Line> m_viewport;
    // Which slice of m_runs belongs to each viewport row: {offset, count}. Rows
    // that were not rebuilt this frame keep {0, 0}.
    std::vector<std::pair<std::size_t, std::size_t>> m_rowRanges;

    render::FrameData m_frame;
    render::Selection m_selection;
    input::Composition m_composition;
    // A paste held back pending confirmation. Already sanitised — what is
    // stored is exactly what will be sent, so an "allow" cannot re-run the
    // guard against different text than the banner described.
    QByteArray m_pendingPaste;
    bool m_dragging = false;
    int m_cols = 0;
    int m_rows = 0;
    bool m_started = false;
    int m_benchFrames = 0;
    int m_benchSteps = 0;
    // Set only by a 4K bench run, to pin the grid to the M0 baseline's 240x63.
    int m_benchCols = 0;
    int m_benchRows = 0;
};

class TerminalRenderer : public QQuickRhiItemRenderer {
  protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

  private:
    void reportBench();

    // Every GPU object lives here (T26), so the device-lost reset is one
    // testable place rather than a block inside a class Qt Quick constructs.
    render::GpuResources m_gpu;
    bool m_shadersLoaded = false;

    render::FrameData m_frame;

    TerminalItem* m_item = nullptr;  // borrowed via synchronize; outlives us
    int m_benchFrames = 0;
    int m_frameIndex = 0;
    bool m_benchDone = false;
    QElapsedTimer m_timer;
    std::vector<double> m_cpuMs;
    std::vector<double> m_gpuMs;
};

}  // namespace krait::app

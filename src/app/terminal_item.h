#pragma once

#include "../net/conpty/conpty_backend.h"
#include "core/terminal/session.h"
#include "render/atlas/glyph_atlas.h"
#include "render/frame_builder.h"
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
    struct Frame {
        std::vector<render::SolidInstance> solids;
        std::vector<render::GlyphInstance> glyphs;
        int atlasWidth = 0;
        int atlasHeight = 0;
        int atlasDirtyTop = 0;
        int atlasDirtyBottom = 0;
        bool atlasGrew = false;
        int pixelWidth = 1;
        int pixelHeight = 1;
        float clearR = 0;
        float clearG = 0;
        float clearB = 0;
    };

    const Frame& frame() const { return m_frame; }

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

  protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    void handleOutput(const QByteArray& bytes);
    void ensureStarted();
    bool ensureFont();
    void rebuildFrame();
    // Viewport row/col under a widget-space point, clamped into the grid.
    void cellAt(const QPointF& pos, int& row, int& col) const;

    std::unique_ptr<render::FontDb> m_fonts;
    std::unique_ptr<render::ShapePool> m_pool;
    std::unique_ptr<render::GlyphAtlas> m_atlas;
    std::unique_ptr<render::FrameBuilder> m_builder;
    std::unique_ptr<core::vt::Session> m_session;
    net::ConptyBackend* m_backend = nullptr;  // owned by this (QObject parent)

    render::RasterFn m_raster;
    std::string m_family;
    std::uint32_t m_primaryFace = 0;
    int m_pxHeight = 20;
    bool m_ligatures = false;

    // Scratch reused across frames so a steady-state frame allocates little.
    std::vector<render::Run> m_runs;
    std::vector<render::ShapedRun> m_shaped;
    std::vector<std::uint32_t> m_faces;
    std::vector<core::vt::Line> m_viewport;
    // Which slice of m_runs belongs to each viewport row: {offset, count}. Rows
    // that were not rebuilt this frame keep {0, 0}.
    std::vector<std::pair<std::size_t, std::size_t>> m_rowRanges;

    Frame m_frame;
    render::Selection m_selection;
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
    void ensureResources(QRhiCommandBuffer* cb);
    void reportBench();

    QRhi* m_rhi = nullptr;  // borrowed; detects device change
    bool m_failed = false;

    TerminalItem::Frame m_frame;
    std::vector<std::uint8_t> m_atlasPixels;  // copied in synchronize
    bool m_atlasNeedsUpload = false;
    int m_texWidth = 0;
    int m_texHeight = 0;

    TerminalItem* m_item = nullptr;  // borrowed via synchronize; outlives us
    int m_benchFrames = 0;
    int m_frameIndex = 0;
    bool m_benchDone = false;
    QElapsedTimer m_timer;
    std::vector<double> m_cpuMs;
    std::vector<double> m_gpuMs;

    std::unique_ptr<QRhiTexture> m_atlasTex;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiBuffer> m_cornerBuf;
    std::unique_ptr<QRhiBuffer> m_solidBuf;
    std::unique_ptr<QRhiBuffer> m_glyphBuf;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_solidSrb;
    std::unique_ptr<QRhiShaderResourceBindings> m_glyphSrb;
    std::unique_ptr<QRhiGraphicsPipeline> m_solidPipeline;
    std::unique_ptr<QRhiGraphicsPipeline> m_glyphPipeline;
    quint32 m_solidCapacity = 0;
    quint32 m_glyphCapacity = 0;
};

}  // namespace krait::app

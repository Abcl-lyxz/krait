#pragma once

#include "../net/conpty/conpty_backend.h"
#include "../render/spike/glyph_atlas.h"
#include "core/terminal/session.h"
#include <rhi/qrhi.h>

#include <QElapsedTimer>
#include <QQuickRhiItem>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

namespace krait::app {

// The M0 terminal (plan T15): ConPTY backend -> core Session -> the spike
// atlas renderer, plus minimal keyboard input. One hardcoded local
// PowerShell session; tabs/profiles are M1+.
class TerminalItem : public QQuickRhiItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(TerminalView)

  public:
    TerminalItem();
    ~TerminalItem() override;

    QQuickRhiItemRenderer* createRenderer() override;

    const render::GlyphAtlas& atlas() const { return m_atlas; }

    const std::vector<float>& instances() const { return m_instances; }

    int cols() const { return m_cols; }

    int rows() const { return m_rows; }

  protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    void handleOutput(const QByteArray& bytes);
    void rebuildInstances();
    void ensureStarted();

    render::GlyphAtlas m_atlas;
    std::unique_ptr<core::vt::Session> m_session;
    net::ConptyBackend* m_backend = nullptr;  // owned by this (QObject parent)
    std::vector<float> m_instances;
    int m_cols = 0;
    int m_rows = 0;
    bool m_started = false;
};

class TerminalRenderer : public QQuickRhiItemRenderer {
  protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

  private:
    void ensureResources(QRhiCommandBuffer* cb);

    QRhi* m_rhi = nullptr;  // borrowed; detects device change
    bool m_failed = false;
    render::GlyphAtlas m_atlas;
    std::vector<float> m_instances;  // copied from the item each sync
    int m_cols = 0;
    int m_rows = 0;
    std::unique_ptr<QRhiTexture> m_atlasTex;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiBuffer> m_cornerBuf;
    std::unique_ptr<QRhiBuffer> m_instanceBuf;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
};

}  // namespace krait::app

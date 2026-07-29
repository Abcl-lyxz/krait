#pragma once

#include <rhi/qrhi.h>

#include <QQuickRhiItem>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace krait::render {

// M0 spike (plan T11): proves QQuickRhiItem + the qsb shader pipeline on
// the D3D11 backend by drawing one colored triangle. Not product code —
// T12 replaces the pipeline with the glyph-atlas renderer.
class TriangleItem : public QQuickRhiItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(Triangle)

  public:
    QQuickRhiItemRenderer* createRenderer() override;
};

class TriangleRenderer : public QQuickRhiItemRenderer {
  protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

  private:
    QRhi* m_rhi = nullptr;  // borrowed; detects device change
    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
};

}  // namespace krait::render

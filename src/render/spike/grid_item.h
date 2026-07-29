#pragma once

#include "glyph_atlas.h"
#include <rhi/qrhi.h>

#include <QQuickRhiItem>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

namespace krait::render {

// M0 spike (plan T12): 240x63 grid of instanced glyph quads sampling an R8
// FreeType atlas, per-cell fg/bg riding a dynamic per-instance buffer (the
// bench-equivalent of the storage-buffer wording; the real renderer decides
// after T13). Verify: a grid of glyphs is visible.
class GridSpikeItem : public QQuickRhiItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(SpikeGrid)

  public:
    static constexpr int kCols = 240;
    static constexpr int kRows = 63;

    GridSpikeItem();

    QQuickRhiItemRenderer* createRenderer() override;

    const GlyphAtlas& atlas() const { return m_atlas; }

  private:
    GlyphAtlas m_atlas;
};

class GridSpikeRenderer : public QQuickRhiItemRenderer {
  protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

  private:
    void fillInstances();
    // The first initialize() runs before the first synchronize() delivers
    // the atlas, so resource creation is lazy and re-attempted from render().
    void ensureResources(QRhiCommandBuffer* cb);

    QRhi* m_rhi = nullptr;  // borrowed; detects device change
    bool m_failed = false;  // creation failed; retry only on device change
    GlyphAtlas m_atlas;     // copied in synchronize (QImage is COW)
    std::vector<float> m_instances;
    std::unique_ptr<QRhiTexture> m_atlasTex;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiBuffer> m_cornerBuf;
    std::unique_ptr<QRhiBuffer> m_instanceBuf;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
};

}  // namespace krait::render

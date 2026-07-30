#include "render/gpu_resources.h"

#include <algorithm>
#include <array>

namespace krait::render {
namespace {

constexpr std::array<float, 8> kCorners{
    0.0F, 0.0F,  //
    1.0F, 0.0F,  //
    0.0F, 1.0F,  //
    1.0F, 1.0F,  //
};

}  // namespace

GpuResources::GpuResources() = default;
GpuResources::~GpuResources() = default;

void GpuResources::setShaders(Shaders shaders) {
    m_shaders = std::move(shaders);
    // New shaders mean new pipelines; the SRBs and buffers are unaffected.
    m_solidPipeline.reset();
    m_glyphPipeline.reset();
    m_failed = false;
}

void GpuResources::setAtlasPixels(std::span<const std::uint8_t> pixels) {
    m_atlasPixels.assign(pixels.begin(), pixels.end());
    m_atlasNeedsUpload = true;
}

void GpuResources::reset() {
    m_solidPipeline.reset();
    m_glyphPipeline.reset();
    m_solidSrb.reset();
    m_glyphSrb.reset();
    m_ubuf.reset();
    m_solidBuf.reset();
    m_glyphBuf.reset();
    m_cornerBuf.reset();
    m_sampler.reset();
    m_atlasTex.reset();
    m_solidCapacity = 0;
    m_glyphCapacity = 0;
    m_texWidth = 0;
    m_texHeight = 0;
    // The new device's texture is empty, so the atlas has to go up again even
    // though not one glyph changed. Losing this is how a recovered frame comes
    // back blank.
    m_atlasNeedsUpload = true;
    m_failed = false;
}

bool GpuResources::sync(QRhi* rhi, QRhiRenderPassDescriptor* renderPass, int sampleCount,
                        QRhiCommandBuffer* cb, const FrameData& frame, QSize outputSize) {
    const bool deviceChanged = m_rhi != rhi;
    if (deviceChanged) {
        reset();
        m_rhi = rhi;
        ++m_deviceResets;
    }
    if (m_failed || m_rhi == nullptr || cb == nullptr || frame.atlasWidth == 0) {
        return false;
    }

    QRhiResourceUpdateBatch* batch = m_rhi->nextResourceUpdateBatch();

    // The atlas texture is recreated only when the atlas GREW; otherwise the
    // dirty row range is uploaded into the existing one.
    const bool newTexture =
        !m_atlasTex || m_texWidth != frame.atlasWidth || m_texHeight != frame.atlasHeight;
    if (newTexture) {
        m_atlasTex.reset(
            m_rhi->newTexture(QRhiTexture::R8, QSize(frame.atlasWidth, frame.atlasHeight), 1));
        if (!m_atlasTex->create()) {
            qWarning("render: atlas texture create() failed");
            m_atlasTex.reset();
            m_failed = true;
            return false;
        }
        m_texWidth = frame.atlasWidth;
        m_texHeight = frame.atlasHeight;
        m_atlasNeedsUpload = true;
        m_glyphSrb.reset();  // the binding points at the old texture
    }

    if (m_atlasNeedsUpload && !m_atlasPixels.empty()) {
        // A grown atlas, or a texture that did not exist a moment ago, holds
        // nothing — upload all of it. Only a steady frame can get away with the
        // dirty range.
        const bool full = frame.atlasGrew || newTexture;
        const int top = full ? 0 : std::max(0, frame.atlasDirtyTop);
        const int wanted = full ? m_texHeight : std::min(m_texHeight, frame.atlasDirtyBottom);
        // The item's atlas may lag this frame's extents by one rebuild; never
        // read past what was actually handed over.
        const auto rowsHeld = static_cast<int>(m_atlasPixels.size() /
                                               static_cast<std::size_t>(std::max(1, m_texWidth)));
        const int bottom = std::min(wanted, rowsHeld);
        // A SHORT upload must stay pending. The atlas can grow twice between
        // two presented frames — takeGrew() consumes the flag on the first
        // rebuild — so the frame that finally reaches the render thread reports
        // the new height with atlasGrew already false, and the pixels handed
        // over are still the old, smaller buffer. Clearing the flag here left
        // every glyph in the new upper half rendering blank for the rest of the
        // session, with nothing to trigger a retry.
        m_atlasNeedsUpload = bottom < wanted;
        if (bottom > top) {
            const auto offset = static_cast<qsizetype>(top) * m_texWidth;
            const auto length = static_cast<qsizetype>(bottom - top) * m_texWidth;
            QRhiTextureSubresourceUploadDescription sub(
                reinterpret_cast<const char*>(m_atlasPixels.data()) + offset, length);
            sub.setDataStride(static_cast<quint32>(m_texWidth));
            sub.setDestinationTopLeft(QPoint(0, top));
            sub.setSourceSize(QSize(m_texWidth, bottom - top));
            batch->uploadTexture(m_atlasTex.get(), QRhiTextureUploadDescription({0, 0, sub}));
        }
    }

    if (!m_sampler) {
        // Nearest: glyphs are placed at integer pixels, and blurring them is the
        // single most common way a terminal ends up looking wrong.
        m_sampler.reset(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                          QRhiSampler::None, QRhiSampler::ClampToEdge,
                                          QRhiSampler::ClampToEdge));
        m_sampler->create();
    }
    if (!m_cornerBuf) {
        m_cornerBuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kCorners)));
        m_cornerBuf->create();
        batch->uploadStaticBuffer(m_cornerBuf.get(), kCorners.data());
    }
    if (!m_ubuf) {
        m_ubuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 4 * sizeof(float)));
        m_ubuf->create();
    }
    const std::array<float, 4> ubufData{static_cast<float>(std::max(1, outputSize.width())),
                                        static_cast<float>(std::max(1, outputSize.height())), 0.0F,
                                        0.0F};
    batch->updateDynamicBuffer(m_ubuf.get(), 0, sizeof(ubufData), ubufData.data());

    // Instance buffers grow but never shrink: a terminal's instance count
    // oscillates every frame and reallocating on each dip would churn.
    const auto solidBytes =
        static_cast<quint32>(std::max<std::size_t>(1, frame.solids.size()) * sizeof(SolidInstance));
    if (!m_solidBuf || m_solidCapacity < solidBytes) {
        m_solidCapacity = solidBytes * 2;
        m_solidBuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, m_solidCapacity));
        m_solidBuf->create();
    }
    if (!frame.solids.empty()) {
        batch->updateDynamicBuffer(m_solidBuf.get(), 0, solidBytes, frame.solids.data());
    }

    const auto glyphBytes =
        static_cast<quint32>(std::max<std::size_t>(1, frame.glyphs.size()) * sizeof(GlyphInstance));
    if (!m_glyphBuf || m_glyphCapacity < glyphBytes) {
        m_glyphCapacity = glyphBytes * 2;
        m_glyphBuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, m_glyphCapacity));
        m_glyphBuf->create();
    }
    if (!frame.glyphs.empty()) {
        batch->updateDynamicBuffer(m_glyphBuf.get(), 0, glyphBytes, frame.glyphs.data());
    }

    cb->resourceUpdate(batch);

    if (!m_solidSrb) {
        m_solidSrb.reset(m_rhi->newShaderResourceBindings());
        m_solidSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage,
                                                     m_ubuf.get()),
        });
        m_solidSrb->create();
        m_solidPipeline.reset();
    }
    if (!m_glyphSrb) {
        m_glyphSrb.reset(m_rhi->newShaderResourceBindings());
        m_glyphSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage,
                                                     m_ubuf.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      m_atlasTex.get(), m_sampler.get()),
        });
        m_glyphSrb->create();
        m_glyphPipeline.reset();
    }

    // Premultiplied source: the shaders output colour * alpha, so the blend is
    // One / OneMinusSrcAlpha rather than SrcAlpha / OneMinusSrcAlpha.
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

    if (!m_solidPipeline) {
        m_solidPipeline.reset(m_rhi->newGraphicsPipeline());
        m_solidPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_solidPipeline->setTargetBlends({blend});
        m_solidPipeline->setShaderStages({
            {QRhiShaderStage::Vertex, m_shaders.solidVert},
            {QRhiShaderStage::Fragment, m_shaders.solidFrag},
        });
        QRhiVertexInputLayout layout;
        layout.setBindings({
            {2 * sizeof(float)},
            {sizeof(SolidInstance), QRhiVertexInputBinding::PerInstance},
        });
        layout.setAttributes({
            {0, 0, QRhiVertexInputAttribute::Float2, 0},
            {1, 1, QRhiVertexInputAttribute::Float4, 0},
            {1, 2, QRhiVertexInputAttribute::Float4, 4 * sizeof(float)},
        });
        m_solidPipeline->setVertexInputLayout(layout);
        m_solidPipeline->setShaderResourceBindings(m_solidSrb.get());
        m_solidPipeline->setRenderPassDescriptor(renderPass);
        m_solidPipeline->setSampleCount(sampleCount);
        if (!m_solidPipeline->create()) {
            qWarning("render: solid pipeline create() FAILED");
            m_solidPipeline.reset();
            m_failed = true;
            return false;
        }
    }
    if (!m_glyphPipeline) {
        m_glyphPipeline.reset(m_rhi->newGraphicsPipeline());
        m_glyphPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_glyphPipeline->setTargetBlends({blend});
        m_glyphPipeline->setShaderStages({
            {QRhiShaderStage::Vertex, m_shaders.glyphVert},
            {QRhiShaderStage::Fragment, m_shaders.glyphFrag},
        });
        QRhiVertexInputLayout layout;
        layout.setBindings({
            {2 * sizeof(float)},
            {sizeof(GlyphInstance), QRhiVertexInputBinding::PerInstance},
        });
        layout.setAttributes({
            {0, 0, QRhiVertexInputAttribute::Float2, 0},
            {1, 1, QRhiVertexInputAttribute::Float4, 0},
            {1, 2, QRhiVertexInputAttribute::Float4, 4 * sizeof(float)},
            {1, 3, QRhiVertexInputAttribute::Float4, 8 * sizeof(float)},
        });
        m_glyphPipeline->setVertexInputLayout(layout);
        m_glyphPipeline->setShaderResourceBindings(m_glyphSrb.get());
        m_glyphPipeline->setRenderPassDescriptor(renderPass);
        m_glyphPipeline->setSampleCount(sampleCount);
        if (!m_glyphPipeline->create()) {
            qWarning("render: glyph pipeline create() FAILED");
            m_glyphPipeline.reset();
            m_failed = true;
            return false;
        }
        qInfo("render: pipelines ready (atlas %dx%d, adapter %s, device #%d)", frame.atlasWidth,
              frame.atlasHeight, m_rhi->driverInfo().deviceName.constData(), m_deviceResets);
    }
    return true;
}

void GpuResources::draw(QRhiCommandBuffer* cb, const FrameData& frame) const {
    if (!ready()) {
        return;
    }
    // Backgrounds, selection and the cursor first, then glyphs over them.
    if (!frame.solids.empty()) {
        cb->setGraphicsPipeline(m_solidPipeline.get());
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput inputs[] = {
            {m_cornerBuf.get(), 0},
            {m_solidBuf.get(), 0},
        };
        cb->setVertexInput(0, 2, inputs);
        cb->draw(4, static_cast<quint32>(frame.solids.size()));
    }
    if (!frame.glyphs.empty()) {
        cb->setGraphicsPipeline(m_glyphPipeline.get());
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput inputs[] = {
            {m_cornerBuf.get(), 0},
            {m_glyphBuf.get(), 0},
        };
        cb->setVertexInput(0, 2, inputs);
        cb->draw(4, static_cast<quint32>(frame.glyphs.size()));
    }
}

}  // namespace krait::render

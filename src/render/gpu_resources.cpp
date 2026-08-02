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
    m_imagePipeline.reset();
    m_failed = false;
}

void GpuResources::setAtlasPixels(std::span<const std::uint8_t> pixels) {
    m_atlasPixels.assign(pixels.begin(), pixels.end());
    m_atlasNeedsUpload = true;
}

void GpuResources::setSizedAtlasPixels(std::span<const std::uint8_t> pixels, int width,
                                       int height) {
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return;
    }
    if (width != m_sizedWidth || height != m_sizedHeight) {
        // A different shape needs a different texture; the binding named the
        // old one.
        m_sizedTex.reset();
        m_sizedSrb.reset();
        m_sizedWidth = width;
        m_sizedHeight = height;
    }
    m_sizedPixels.assign(pixels.begin(), pixels.end());
    m_sizedNeedsUpload = true;
}

void GpuResources::setImagePixels(std::uint32_t id, int width, int height,
                                  std::span<const std::uint32_t> pixels, std::uint64_t sequence) {
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return;  // a short buffer would upload past its own end
    }
    ImageResource& image = m_images[id];
    // A re-transmitted id keeps its slot but must rebuild: the size may differ,
    // and the old texture is then the wrong shape.
    image.texture.reset();
    image.srb.reset();
    image.pixels.assign(pixels.begin(), pixels.end());
    image.width = width;
    image.height = height;
    image.sequence = sequence;
    image.lastFrame = m_frameCounter;
    image.needsUpload = true;
    evictImages();
}

bool GpuResources::hasImage(std::uint32_t id) const {
    return m_images.contains(id);
}

std::uint64_t GpuResources::imageSequence(std::uint32_t id) const {
    const auto found = m_images.find(id);
    return found == m_images.end() ? 0 : found->second.sequence;
}

void GpuResources::evictImages() {
    while (m_images.size() > kMaxGpuImages) {
        // Least recently DRAWN, not least recently transmitted: an image that
        // is still on screen must survive a burst of new ones scrolling past.
        auto oldest = m_images.begin();
        for (auto it = m_images.begin(); it != m_images.end(); ++it) {
            if (it->second.lastFrame < oldest->second.lastFrame) {
                oldest = it;
            }
        }
        const std::uint32_t id = oldest->first;
        dropImage(id);
        // Defensive: dropImage is a no-op for an id that is not there, and a
        // loop that failed to shrink the map would spin forever.
        if (m_images.contains(id)) {
            break;
        }
    }
}

std::vector<std::uint32_t> GpuResources::imageIds() const {
    std::vector<std::uint32_t> ids;
    ids.reserve(m_images.size());
    for (const auto& [id, image] : m_images) {
        ids.push_back(id);
    }
    return ids;
}

void GpuResources::dropImage(std::uint32_t id) {
    const auto found = m_images.find(id);
    if (found == m_images.end()) {
        return;
    }
    // deleteLater(), not the unique_ptr deleter: a frame already recorded may
    // still reference these, and QRhiResource documents destroying such a
    // resource before endFrame() as undefined.
    if (QRhiShaderResourceBindings* srb = found->second.srb.release()) {
        srb->deleteLater();
    }
    if (QRhiTexture* texture = found->second.texture.release()) {
        texture->deleteLater();
    }
    m_images.erase(found);
}

bool GpuResources::ensureImage(ImageResource& image, QRhiResourceUpdateBatch* batch) {
    if (image.width <= 0 || image.height <= 0) {
        return false;
    }
    if (!image.texture) {
        // RGBA8 is the only format QRhi documents as always supported; the
        // shader swizzles our BGRA byte order rather than asking for BGRA8 and
        // carrying a fallback path nothing would exercise.
        image.texture.reset(
            m_rhi->newTexture(QRhiTexture::RGBA8, QSize(image.width, image.height), 1));
        if (!image.texture->create()) {
            qWarning("render: image texture create() failed (%dx%d)", image.width, image.height);
            image.texture.reset();
            return false;
        }
        image.needsUpload = true;
        image.srb.reset();  // the binding named the texture that just went away
    }
    if (image.needsUpload) {
        // Tightly packed, so no setDataStride(): QRhi requires stride ==
        // width * pixelSize when it is left unset, which is exactly our layout.
        const auto bytes = static_cast<quint32>(image.pixels.size() * sizeof(std::uint32_t));
        QRhiTextureSubresourceUploadDescription sub(image.pixels.data(), bytes);
        batch->uploadTexture(image.texture.get(), QRhiTextureUploadDescription({0, 0, sub}));
        image.needsUpload = false;
    }
    if (!image.srb) {
        image.srb.reset(m_rhi->newShaderResourceBindings());
        // Layout-identical to the glyph SRB on purpose: QRhi allows
        // setShaderResources() with any layout-compatible bindings, which is
        // what lets one pipeline draw every image with its own texture.
        image.srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage,
                                                     m_ubuf.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      image.texture.get(), m_imageSampler.get()),
        });
        if (!image.srb->create()) {
            qWarning("render: image shader resource bindings create() failed");
            image.srb.reset();
            return false;
        }
    }
    return true;
}

void GpuResources::reset() {
    m_solidPipeline.reset();
    m_glyphPipeline.reset();
    m_imagePipeline.reset();
    m_solidSrb.reset();
    m_glyphSrb.reset();
    m_ubuf.reset();
    m_solidBuf.reset();
    m_glyphBuf.reset();
    m_imageBuf.reset();
    m_cornerBuf.reset();
    m_sampler.reset();
    m_imageSampler.reset();
    m_atlasTex.reset();
    m_sizedTex.reset();
    m_sizedSrb.reset();
    m_sizedBuf.reset();
    m_sizedCapacity = 0;
    // Same bargain as the atlas above: the PIXELS survive so the new device can
    // be refilled without the item re-rasterising every sized glyph.
    m_sizedNeedsUpload = true;
    m_solidCapacity = 0;
    m_glyphCapacity = 0;
    m_imageCapacity = 0;
    m_texWidth = 0;
    m_texHeight = 0;
    // The device is gone, so every image texture went with it — but the PIXELS
    // stay, which is the whole reason they are copied here. Dropping the GPU
    // objects and re-arming the upload is what makes a recovered frame show the
    // same pictures rather than blank rectangles. These are plain resets, not
    // deleteLater(): the device that owned them no longer exists, so there is
    // no in-flight frame left to reference them.
    for (auto& [id, image] : m_images) {
        image.texture.reset();
        image.srb.reset();
        image.needsUpload = true;
    }
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
    ++m_frameCounter;

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

    // Every create() below is checked. Only the pipelines and the texture used
    // to be, and after a device-lost rebuild under memory pressure an uncreated
    // buffer still reached updateDynamicBuffer and setVertexInput, and an
    // uncreated SRB reached setShaderResources. Recovery is exactly when
    // allocation fails, so the recovery path is the wrong place to assume it
    // cannot.
    const auto created = [this](auto& resource, const char* what) {
        if (!resource->create()) {
            qWarning("render: %s create() failed", what);
            resource.reset();
            m_failed = true;
            return false;
        }
        return true;
    };

    if (!m_sampler) {
        // Nearest: glyphs are placed at integer pixels, and blurring them is the
        // single most common way a terminal ends up looking wrong.
        m_sampler.reset(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                          QRhiSampler::None, QRhiSampler::ClampToEdge,
                                          QRhiSampler::ClampToEdge));
        if (!created(m_sampler, "sampler")) {
            return false;
        }
    }
    if (!m_imageSampler) {
        // Linear, unlike the glyph sampler. A placement is sized in CELLS, so
        // the image is almost never at its native pixel size — Nearest there
        // would alias every photo and plot into a mess of jagged edges, which
        // is the opposite of the reason it is right for glyphs.
        m_imageSampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                               QRhiSampler::None, QRhiSampler::ClampToEdge,
                                               QRhiSampler::ClampToEdge));
        if (!created(m_imageSampler, "image sampler")) {
            return false;
        }
    }
    if (!m_cornerBuf) {
        m_cornerBuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kCorners)));
        if (!created(m_cornerBuf, "corner buffer")) {
            return false;
        }
        batch->uploadStaticBuffer(m_cornerBuf.get(), kCorners.data());
    }
    if (!m_ubuf) {
        m_ubuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 4 * sizeof(float)));
        if (!created(m_ubuf, "uniform buffer")) {
            return false;
        }
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
        if (!created(m_solidBuf, "solid instance buffer")) {
            m_solidCapacity = 0;
            return false;
        }
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
        if (!created(m_glyphBuf, "glyph instance buffer")) {
            m_glyphCapacity = 0;
            return false;
        }
    }
    if (!frame.glyphs.empty()) {
        batch->updateDynamicBuffer(m_glyphBuf.get(), 0, glyphBytes, frame.glyphs.data());
    }

    if (!frame.sizedGlyphs.empty() && !m_sizedPixels.empty()) {
        if (!m_sizedTex) {
            m_sizedTex.reset(
                m_rhi->newTexture(QRhiTexture::R8, QSize(m_sizedWidth, m_sizedHeight), 1));
            if (!m_sizedTex->create()) {
                qWarning("render: sized atlas texture create() failed");
                m_sizedTex.reset();
            } else {
                m_sizedNeedsUpload = true;  // a new texture holds nothing
                m_sizedSrb.reset();
            }
        }
        if (m_sizedTex && m_sizedNeedsUpload) {
            const auto rows = static_cast<int>(m_sizedPixels.size() /
                                               static_cast<std::size_t>(std::max(1, m_sizedWidth)));
            const int height = std::min(m_sizedHeight, rows);
            if (height > 0) {
                QRhiTextureSubresourceUploadDescription sub(
                    reinterpret_cast<const char*>(m_sizedPixels.data()),
                    static_cast<quint32>(m_sizedWidth) * static_cast<quint32>(height));
                sub.setDataStride(static_cast<quint32>(m_sizedWidth));
                sub.setSourceSize(QSize(m_sizedWidth, height));
                batch->uploadTexture(m_sizedTex.get(), QRhiTextureUploadDescription({0, 0, sub}));
                m_sizedNeedsUpload = false;
            }
        }
        if (m_sizedTex && !m_sizedSrb) {
            m_sizedSrb.reset(m_rhi->newShaderResourceBindings());
            // Layout-identical to the glyph SRB, so the glyph pipeline draws it.
            m_sizedSrb->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage,
                                                         m_ubuf.get()),
                QRhiShaderResourceBinding::sampledTexture(
                    1, QRhiShaderResourceBinding::FragmentStage, m_sizedTex.get(), m_sampler.get()),
            });
            if (!m_sizedSrb->create()) {
                qWarning("render: sized atlas shader resource bindings create() failed");
                m_sizedSrb.reset();
            }
        }
        const auto sizedBytes =
            static_cast<quint32>(frame.sizedGlyphs.size() * sizeof(GlyphInstance));
        if (!m_sizedBuf || m_sizedCapacity < sizedBytes) {
            m_sizedCapacity = sizedBytes * 2;
            m_sizedBuf.reset(
                m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, m_sizedCapacity));
            if (!created(m_sizedBuf, "sized glyph instance buffer")) {
                m_sizedCapacity = 0;
                return false;
            }
        }
        batch->updateDynamicBuffer(m_sizedBuf.get(), 0, sizedBytes, frame.sizedGlyphs.data());
    }

    if (!frame.images.empty()) {
        const auto imageBytes = static_cast<quint32>(frame.images.size() * sizeof(ImageInstance));
        if (!m_imageBuf || m_imageCapacity < imageBytes) {
            m_imageCapacity = imageBytes * 2;
            m_imageBuf.reset(
                m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, m_imageCapacity));
            if (!created(m_imageBuf, "image instance buffer")) {
                m_imageCapacity = 0;
                return false;
            }
        }
        batch->updateDynamicBuffer(m_imageBuf.get(), 0, imageBytes, frame.images.data());
        // Textures and bindings for every image this frame names. A failure
        // here is per-image: draw() skips a batch whose SRB never appeared,
        // because one oversized picture must not blank the whole terminal.
        for (const ImageBatch& imageBatch : frame.imageBatches) {
            const auto found = m_images.find(imageBatch.imageId);
            if (found != m_images.end()) {
                // Stamped whether or not the upload succeeds: an image this
                // frame draws is in use, and eviction goes by last USE.
                found->second.lastFrame = m_frameCounter;
                ensureImage(found->second, batch);
            }
        }
    }

    cb->resourceUpdate(batch);

    if (!m_solidSrb) {
        m_solidSrb.reset(m_rhi->newShaderResourceBindings());
        m_solidSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage,
                                                     m_ubuf.get()),
        });
        if (!created(m_solidSrb, "solid shader resource bindings")) {
            return false;
        }
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
        if (!created(m_glyphSrb, "glyph shader resource bindings")) {
            return false;
        }
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
    // Built lazily: a session that never receives a sixel or a kitty image
    // never pays for the pipeline, and imageFrag may legitimately be absent in
    // a test that only drives text.
    if (!m_imagePipeline && !frame.images.empty() && m_shaders.imageFrag.isValid()) {
        m_imagePipeline.reset(m_rhi->newGraphicsPipeline());
        m_imagePipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_imagePipeline->setTargetBlends({blend});
        m_imagePipeline->setShaderStages({
            {QRhiShaderStage::Vertex, m_shaders.glyphVert},
            {QRhiShaderStage::Fragment, m_shaders.imageFrag},
        });
        // Identical to the glyph layout — that is why images reuse glyph.vert.
        QRhiVertexInputLayout layout;
        layout.setBindings({
            {2 * sizeof(float)},
            {sizeof(ImageInstance), QRhiVertexInputBinding::PerInstance},
        });
        layout.setAttributes({
            {0, 0, QRhiVertexInputAttribute::Float2, 0},
            {1, 1, QRhiVertexInputAttribute::Float4, 0},
            {1, 2, QRhiVertexInputAttribute::Float4, 4 * sizeof(float)},
            {1, 3, QRhiVertexInputAttribute::Float4, 8 * sizeof(float)},
        });
        m_imagePipeline->setVertexInputLayout(layout);
        // Any layout-compatible SRB works here; the per-image ones are what
        // actually bind at draw time.
        m_imagePipeline->setShaderResourceBindings(m_glyphSrb.get());
        m_imagePipeline->setRenderPassDescriptor(renderPass);
        m_imagePipeline->setSampleCount(sampleCount);
        if (!m_imagePipeline->create()) {
            qWarning("render: image pipeline create() FAILED");
            m_imagePipeline.reset();
            // NOT m_failed: text must keep drawing when only the image pipeline
            // is unavailable.
        }
    }
    return true;
}

void GpuResources::drawImageBatches(QRhiCommandBuffer* cb, const FrameData& frame,
                                    std::uint32_t first, std::uint32_t last) const {
    if (!m_imagePipeline || !m_imageBuf) {
        return;
    }
    cb->setGraphicsPipeline(m_imagePipeline.get());
    for (std::uint32_t i = first; i < last && i < frame.imageBatches.size(); ++i) {
        const ImageBatch& imageBatch = frame.imageBatches[i];
        const auto found = m_images.find(imageBatch.imageId);
        if (found == m_images.end() || !found->second.srb || imageBatch.count == 0) {
            continue;  // its texture never came up; skip the batch, keep the frame
        }
        cb->setShaderResources(found->second.srb.get());
        // The instance OFFSET rather than a firstInstance argument: a non-zero
        // firstInstance is not portable across every QRhi backend, while a
        // vertex-buffer byte offset is.
        const QRhiCommandBuffer::VertexInput inputs[] = {
            {m_cornerBuf.get(), 0},
            {m_imageBuf.get(), static_cast<quint32>(imageBatch.first * sizeof(ImageInstance))},
        };
        cb->setVertexInput(0, 2, inputs);
        cb->draw(4, imageBatch.count);
    }
}

void GpuResources::draw(QRhiCommandBuffer* cb, const FrameData& frame) const {
    if (!ready()) {
        return;
    }
    // Backgrounds, selection and the cursor first, then images that sit UNDER
    // the text, then glyphs, then images that sit over them. A negative zIndex
    // is how kitty spells "watermark", and drawing those after the glyphs would
    // hide the very text they are meant to sit behind.
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
    drawImageBatches(cb, frame, 0, frame.belowBatchCount);
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
    // OSC 66's scaled glyphs: the same pipeline, a different atlas. Layout
    // compatibility is what makes a second pipeline unnecessary.
    if (!frame.sizedGlyphs.empty() && m_sizedSrb && m_sizedBuf) {
        cb->setGraphicsPipeline(m_glyphPipeline.get());
        cb->setShaderResources(m_sizedSrb.get());
        const QRhiCommandBuffer::VertexInput inputs[] = {
            {m_cornerBuf.get(), 0},
            {m_sizedBuf.get(), 0},
        };
        cb->setVertexInput(0, 2, inputs);
        cb->draw(4, static_cast<quint32>(frame.sizedGlyphs.size()));
    }
    drawImageBatches(cb, frame, frame.belowBatchCount,
                     static_cast<std::uint32_t>(frame.imageBatches.size()));
}

}  // namespace krait::render

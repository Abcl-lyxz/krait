#pragma once

#include "render/frame_builder.h"
#include <rhi/qrhi.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace krait::render {

// One frame's worth of GPU input. Produced on the GUI thread by the item and
// copied to the render thread in synchronize() — copied rather than shared
// because the render thread must never walk the grid the GUI thread is feeding.
struct FrameData {
    std::vector<SolidInstance> solids;
    std::vector<GlyphInstance> glyphs;
    int atlasWidth = 0;
    int atlasHeight = 0;
    int atlasDirtyTop = 0;
    int atlasDirtyBottom = 0;
    bool atlasGrew = false;
    float clearR = 0;
    float clearG = 0;
    float clearB = 0;
};

// The GPU-side resource set for the terminal: two pipelines (solid rects, then
// textured glyph quads), the atlas texture, and the instance buffers.
//
// Split out of TerminalRenderer for plan T26. rules/render.md makes D3D
// device-lost recovery mandatory AND tested, and the recovery path is exactly
// "the QRhi changed, so drop every resource and rebuild lazily". That is
// untestable while it lives inside a QQuickRhiItemRenderer, whose rhi() and
// renderTarget() are supplied by Qt Quick and cannot be faked. Here the QRhi is
// a parameter, so a test can hand it two different ones.
//
// What that models honestly: QQuickRhiItemNode OWNS the renderer, and
// invalidating the scene graph — which is what a real D3D device loss does —
// destroys it, so in the running app recovery means a FRESH GpuResources rather
// than the rhi-changed branch below. The branch still has to exist, because
// QQuickRhiItemRenderer::initialize() is documented to "be prepared that the
// QRhi object ... may change between invocations", and the test file covers
// both paths for that reason.
class GpuResources {
  public:
    GpuResources();
    ~GpuResources();
    GpuResources(const GpuResources&) = delete;
    GpuResources& operator=(const GpuResources&) = delete;

    // The four compiled shaders. Injected rather than loaded here so this class
    // never touches the filesystem or the Qt resource system.
    struct Shaders {
        QShader solidVert;
        QShader solidFrag;
        QShader glyphVert;
        QShader glyphFrag;
    };

    void setShaders(Shaders shaders);

    // Hands over the atlas bitmap. Copied: the source belongs to the GUI thread
    // and is only valid while synchronize() has that thread blocked.
    void setAtlasPixels(std::span<const std::uint8_t> pixels);

    // Whether a bitmap has ever been handed over. The copy outlives a lost
    // device on purpose: the new texture is empty and has to be refilled from
    // something, and re-rasterising the atlas would be a far bigger hammer.
    bool hasAtlas() const { return !m_atlasPixels.empty(); }

    // How many bytes of atlas we are holding. The caller compares this against
    // its own atlas: a mismatch means our copy is a stale, smaller buffer and
    // has to be handed over again even though no glyph changed.
    std::size_t atlasBytes() const { return m_atlasPixels.size(); }

    // Creates or updates everything this frame needs and queues the uploads on
    // `cb`. Must be called with no render pass active. Returns false when the
    // frame cannot be drawn — the caller should still begin and end a pass so
    // the target is cleared rather than showing garbage.
    //
    // Passing a different `rhi` than last time IS the device-lost path.
    // `outputSize` is the colour buffer in DEVICE pixels and must come from
    // renderTarget()->pixelSize(), never from the item's logical size. The
    // shaders divide by it to reach clip space, and the render pass viewport is
    // set from the same number — taking it from the render target is what makes
    // the two agree by construction. A logical value stretches the whole grid
    // on any display above 100% scaling, which is the blur render.md forbids.
    bool sync(QRhi* rhi, QRhiRenderPassDescriptor* renderPass, int sampleCount,
              QRhiCommandBuffer* cb, const FrameData& frame, QSize outputSize);

    // Issues the two instanced draws. Only valid inside a render pass, and only
    // after sync() returned true.
    void draw(QRhiCommandBuffer* cb, const FrameData& frame) const;

    bool ready() const { return m_solidPipeline && m_glyphPipeline; }

    // How many times the device changed under us: 1 after the first sync(),
    // then one more per lost device. The fake-lost test asserts it climbs and
    // that the frame still draws afterwards.
    int deviceResets() const { return m_deviceResets; }

  private:
    // Drops every device-owned object. Called when the QRhi changes.
    void reset();

    QRhi* m_rhi = nullptr;  // borrowed; an identity change means device lost
    Shaders m_shaders;
    bool m_failed = false;
    int m_deviceResets = 0;

    std::vector<std::uint8_t> m_atlasPixels;
    bool m_atlasNeedsUpload = false;
    int m_texWidth = 0;
    int m_texHeight = 0;

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

}  // namespace krait::render

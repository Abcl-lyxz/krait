#pragma once

#include "render/frame_builder.h"
#include <rhi/qrhi.h>

#include <cstdint>
#include <map>
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
    // T78. The background image shows through THIS, not through an image drawn
    // on top at low opacity — that version washes out the text as well as the
    // background, which is how the feature makes a terminal unreadable. 1 when
    // there is no image, so the default install asks for no blending at all.
    float clearA = 1;

    // Graphics (T84). One instance buffer for every image quad, sliced into
    // per-texture runs; the first `belowBatchCount` runs draw before the text.
    std::vector<ImageInstance> images;
    std::vector<ImageBatch> imageBatches;
    std::uint32_t belowBatchCount = 0;

    // OSC 66's scaled glyphs and the atlas they sample (T84). A second atlas
    // because the main one's slots are one line tall; see FrameParams.
    std::vector<GlyphInstance> sizedGlyphs;
    int sizedAtlasWidth = 0;
    int sizedAtlasHeight = 0;
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
        // Images reuse glyphVert verbatim — same vertex layout, same clip-space
        // maths — so only the fragment stage is new (frame_builder.h).
        QShader imageFrag;
    };

    void setShaders(Shaders shaders);

    // Hands over the atlas bitmap. Copied: the source belongs to the GUI thread
    // and is only valid while synchronize() has that thread blocked.
    void setAtlasPixels(std::span<const std::uint8_t> pixels);

    // Whether a bitmap has ever been handed over. The copy outlives a lost
    // device on purpose: the new texture is empty and has to be refilled from
    // something, and re-rasterising the atlas would be a far bigger hammer.
    bool hasAtlas() const { return !m_atlasPixels.empty(); }

    // The sized-glyph atlas (T84). Uploaded WHOLE whenever it changes rather
    // than by dirty range: sized text is a handful of glyphs on the rare line
    // that uses it, so the incremental bookkeeping the main atlas needs would
    // be more code than the upload it saves.
    void setSizedAtlasPixels(std::span<const std::uint8_t> pixels, int width, int height);

    // --- Decoded images (T84) ---
    //
    // The PIXELS are copied here, exactly as the atlas bitmap is and for the
    // same reason: a lost device leaves an empty texture that has to be refilled
    // from something, and asking the core to re-hand-over every image would put
    // a recovery obligation on a layer that has no idea a device exists. The
    // cost is a second copy of what ImageStore already caps at 64 MiB.
    // `sequence` is ImageStore's insertion ordinal for those pixels. It is what
    // makes a RETRANSMITTED id work: put() replaces an image in place, so the
    // id alone still answers "already have it" while the pixels behind it have
    // changed — and a plot refreshing itself under `i=1` would show its first
    // frame forever.
    void setImagePixels(std::uint32_t id, int width, int height,
                        std::span<const std::uint32_t> pixels, std::uint64_t sequence);

    // Whether this id's pixels are already held, so a caller can hand each
    // image over exactly once.
    bool hasImage(std::uint32_t id) const;

    // The sequence held for `id`, or 0 when we hold nothing. A caller compares
    // this against its store's to decide whether a re-handover is needed.
    std::uint64_t imageSequence(std::uint32_t id) const;

    // Drops one image's pixels and its GPU objects. The objects go through
    // deleteLater(): a frame recorded moments ago may still reference them, and
    // QRhiResource documents destroying such a resource before endFrame() as
    // undefined.
    void dropImage(std::uint32_t id);

    std::size_t imageCount() const { return m_images.size(); }

    // The ids whose pixels we hold, so a caller can drop the ones its own store
    // has since evicted. Small by construction: ImageStore caps the whole set
    // at 64 MiB of pixels, and a terminal showing more than a few at once is
    // already unusual.
    std::vector<std::uint32_t> imageIds() const;

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

    // Draws image batches [first, last), each with its own texture's bindings.
    void drawImageBatches(QRhiCommandBuffer* cb, const FrameData& frame, std::uint32_t first,
                          std::uint32_t last) const;

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

    // One decoded image: its pixels, its texture, and the bindings that name
    // that texture. The SRB is per-image because a binding names a concrete
    // texture; it is created ONCE and reused, never per frame — QRhi documents
    // create() as possibly allocating native resources.
    struct ImageResource {
        std::vector<std::uint32_t> pixels;  // 0xAARRGGBB, kept for device loss
        int width = 0;
        int height = 0;
        bool needsUpload = true;
        std::uint64_t sequence = 0;   // ImageStore's ordinal for these pixels
        std::uint64_t lastFrame = 0;  // for the eviction below
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiShaderResourceBindings> srb;
    };

    // How many decoded images may hold GPU objects at once.
    //
    // ImageStore bounds itself in BYTES and has no count cap, so a thousand 1x1
    // images cost it 4 KB and are never evicted — while here each one is a
    // texture plus a bindings object, and a stream that transmits and places a
    // fresh id in a loop would accumulate them for the life of the session.
    // Remote input picking that workload is exactly what rules/net.md is about.
    static constexpr std::size_t kMaxGpuImages = 64;

    // Drops the least recently DRAWN images until the cap is met.
    void evictImages();

    // Queues `image`'s texture + SRB creation and its upload on `batch`.
    // False when the device refused, in which case the image is skipped for the
    // frame rather than failing it.
    bool ensureImage(ImageResource& image, QRhiResourceUpdateBatch* batch);

    std::vector<std::uint8_t> m_sizedPixels;
    bool m_sizedNeedsUpload = false;
    int m_sizedWidth = 0;
    int m_sizedHeight = 0;
    std::unique_ptr<QRhiTexture> m_sizedTex;
    std::unique_ptr<QRhiShaderResourceBindings> m_sizedSrb;
    std::unique_ptr<QRhiBuffer> m_sizedBuf;
    quint32 m_sizedCapacity = 0;

    std::map<std::uint32_t, ImageResource> m_images;
    std::uint64_t m_frameCounter = 0;
    std::unique_ptr<QRhiSampler> m_imageSampler;
    std::unique_ptr<QRhiBuffer> m_imageBuf;
    std::unique_ptr<QRhiGraphicsPipeline> m_imagePipeline;
    quint32 m_imageCapacity = 0;
};

}  // namespace krait::render

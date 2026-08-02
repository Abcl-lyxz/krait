// T26 — the fake device-lost harness rules/render.md requires.
//
// The Null QRhi backend "does not issue any graphics calls and creates no
// resources. All QRhi operations will succeed as normal", so it is a real QRhi
// with real object lifetimes and no GPU. Destroying one and handing GpuResources
// a second is exactly what a lost device looks like from the renderer's side:
// Qt tears the scene graph down on FrameOpDeviceLost and comes back with a NEW
// QRhi, which is why QQuickRhiItemRenderer::initialize() is documented to "be
// prepared that the QRhi object ... may change between invocations".
//
// What this proves that a running app cannot: recovery happens with no stale
// handle surviving, and the atlas goes back up rather than the new device
// drawing every glyph from an empty texture.

#include "app/gpu_policy.h"
#include "render/gpu_resources.h"
#include <catch2/catch_test_macros.hpp>
#include <rhi/qrhi.h>

#include <QFile>

#include <cstdint>
#include <memory>
#include <vector>

using krait::render::FrameData;
using krait::render::GlyphInstance;
using krait::render::GpuResources;
using krait::render::SolidInstance;

namespace {

// The same four shaders the app uses, compiled into this binary by CMake.
// QRhi's frontend rejects an empty QShader before any backend sees it, so a
// stub would not get past pipeline creation.
QShader loadShader(const QString& path) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return QShader::fromSerialized(file.readAll());
}

GpuResources::Shaders realShaders() {
    return {
        .solidVert = loadShader(":/shaders/cell.vert.qsb"),
        .solidFrag = loadShader(":/shaders/cell.frag.qsb"),
        .glyphVert = loadShader(":/shaders/glyph.vert.qsb"),
        .glyphFrag = loadShader(":/shaders/glyph.frag.qsb"),
        .imageFrag = loadShader(":/shaders/image.frag.qsb"),
    };
}

std::unique_ptr<QRhi> makeRhi() {
    QRhiNullInitParams params;
    return std::unique_ptr<QRhi>(QRhi::create(QRhi::Null, &params));
}

// A frame with one of each instance and a small atlas — enough that every
// resource path (texture, both instance buffers, both pipelines) is exercised.
FrameData makeFrame() {
    FrameData frame;
    frame.solids.push_back(SolidInstance{.x = 0, .y = 0, .w = 8, .h = 16});
    frame.glyphs.push_back(GlyphInstance{.x = 0, .y = 0, .w = 8, .h = 16});
    frame.atlasWidth = 64;
    frame.atlasHeight = 64;
    frame.atlasDirtyTop = 0;
    frame.atlasDirtyBottom = 64;
    frame.atlasGrew = true;
    return frame;
}

// The offscreen target a headless QRhi renders into, plus the pass descriptor
// the pipelines are built against. Kept together because the descriptor has to
// outlive the pipelines that reference it.
struct Target {
    std::unique_ptr<QRhiTexture> color;
    std::unique_ptr<QRhiTextureRenderTarget> rt;
    std::unique_ptr<QRhiRenderPassDescriptor> rp;
};

Target makeTarget(QRhi& rhi) {
    Target target;
    target.color.reset(
        rhi.newTexture(QRhiTexture::RGBA8, QSize(800, 600), 1, QRhiTexture::RenderTarget));
    REQUIRE(target.color->create());
    target.rt.reset(rhi.newTextureRenderTarget({{target.color.get()}}));
    target.rp.reset(target.rt->newCompatibleRenderPassDescriptor());
    target.rt->setRenderPassDescriptor(target.rp.get());
    REQUIRE(target.rt->create());
    return target;
}

// One frame end to end. Returns what sync() said, so a caller can assert the
// frame was actually drawable rather than silently skipped.
bool drawOneFrame(QRhi& rhi, GpuResources& gpu, Target& target, const FrameData& frame) {
    QRhiCommandBuffer* cb = nullptr;
    if (rhi.beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) {
        return false;
    }
    const bool ok = gpu.sync(&rhi, target.rp.get(), 1, cb, frame, QSize(800, 600));
    cb->beginPass(target.rt.get(), QColor::fromRgbF(0, 0, 0), {1.0F, 0});
    gpu.draw(cb, frame);
    cb->endPass();
    rhi.endOffscreenFrame();
    return ok;
}

}  // namespace

TEST_CASE("the null backend is available for the device-lost harness", "[render][device]") {
    const auto rhi = makeRhi();
    REQUIRE(rhi != nullptr);
    CHECK_FALSE(rhi->isDeviceLost());
}

TEST_CASE("a fresh GpuResources builds every resource once", "[render][device]") {
    const auto rhi = makeRhi();
    REQUIRE(rhi != nullptr);
    Target target = makeTarget(*rhi);

    GpuResources gpu;
    gpu.setShaders(realShaders());
    const std::vector<std::uint8_t> atlas(std::size_t{64} * 64, 0x7F);
    gpu.setAtlasPixels(atlas);

    CHECK(gpu.deviceResets() == 0);
    CHECK_FALSE(gpu.ready());
    CHECK(drawOneFrame(*rhi, gpu, target, makeFrame()));
    CHECK(gpu.ready());
    // The first device counts as a reset: there was no device before it.
    CHECK(gpu.deviceResets() == 1);
}

TEST_CASE("a lost device is recovered on the next frame", "[render][device]") {
    // Both devices stay alive for the whole case. That is not a convenience:
    // Qt's contract is "release all QRhi resources, THEN destroy the QRhi", and
    // an object outliving its device is undefined enough that doing it here
    // made this test fail intermittently rather than reproducibly. What is
    // under test is the documented scenario — QQuickRhiItemRenderer::initialize
    // warns that "the QRhi object ... may change between invocations" — and
    // that is a SWAP, not a destruction.
    const auto first = makeRhi();
    const auto second = makeRhi();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    Target firstTarget = makeTarget(*first);
    Target secondTarget = makeTarget(*second);

    // Declared last so it is destroyed FIRST, before either device.
    GpuResources gpu;
    gpu.setShaders(realShaders());
    gpu.setAtlasPixels(std::vector<std::uint8_t>(std::size_t{64} * 64, 0x7F));
    const FrameData frame = makeFrame();

    REQUIRE(drawOneFrame(*first, gpu, firstTarget, frame));
    REQUIRE(gpu.ready());
    REQUIRE(gpu.hasAtlas());

    // THE FAKE LOSS: the next frame arrives on a different device, with no
    // re-setup, no setShaders and no setAtlasPixels — recovery has to be
    // automatic, because Qt gives the renderer no other hook. The atlas copy
    // outliving the device is what makes the recovered frame show text rather
    // than come back blank.
    CHECK(drawOneFrame(*second, gpu, secondTarget, frame));
    CHECK(gpu.ready());
    CHECK(gpu.hasAtlas());
    CHECK(gpu.deviceResets() == 2);

    // ...and back again, because a driver that resets once usually resets twice
    // and the second recovery must not reuse anything from the first.
    CHECK(drawOneFrame(*first, gpu, firstTarget, frame));
    CHECK(gpu.deviceResets() == 3);
}

TEST_CASE("recovery survives repeated device loss", "[render][device]") {
    // Every device outlives the resources built on it, for the reason above.
    constexpr int kDevices = 5;
    std::vector<std::unique_ptr<QRhi>> devices;
    std::vector<Target> targets;
    for (int i = 0; i < kDevices; ++i) {
        auto rhi = makeRhi();
        REQUIRE(rhi != nullptr);
        targets.push_back(makeTarget(*rhi));
        devices.push_back(std::move(rhi));
    }

    GpuResources gpu;  // destroyed before `devices`, which is what makes this safe
    gpu.setShaders(realShaders());
    gpu.setAtlasPixels(std::vector<std::uint8_t>(std::size_t{64} * 64, 0x7F));
    const FrameData frame = makeFrame();

    // A driver resetting in a loop is the realistic failure, not one clean
    // loss: a recovery path that half-rebuilds shows up here and not above.
    for (int i = 0; i < kDevices; ++i) {
        CHECK(drawOneFrame(*devices[static_cast<std::size_t>(i)], gpu,
                           targets[static_cast<std::size_t>(i)], frame));
        CHECK(gpu.ready());
    }
    CHECK(gpu.deviceResets() == kDevices);
}

TEST_CASE("a frame with no atlas is skipped rather than drawn", "[render][device]") {
    const auto rhi = makeRhi();
    REQUIRE(rhi != nullptr);
    Target target = makeTarget(*rhi);

    GpuResources gpu;
    gpu.setShaders(realShaders());
    FrameData frame = makeFrame();
    frame.atlasWidth = 0;  // the font stack has not resolved yet
    frame.atlasHeight = 0;
    CHECK_FALSE(drawOneFrame(*rhi, gpu, target, frame));
    CHECK_FALSE(gpu.ready());
}

TEST_CASE("adapter selection prefers software only when it should", "[render][device]") {
    using krait::app::preferSoftwareDevice;
    // Local machine, no override: the GPU is the whole point.
    CHECK_FALSE(preferSoftwareDevice("", false));
    // RDP: rules/render.md's "WARP/software fallback for RDP and VMs".
    CHECK(preferSoftwareDevice("", true));
    CHECK(preferSoftwareDevice("warp", false));
    CHECK(preferSoftwareDevice("software", false));
    // The escape hatch has to win over the probe, or a host that misreports
    // itself as remote could never use its GPU.
    CHECK_FALSE(preferSoftwareDevice("hardware", true));
    // An unknown value is not an override; it falls through to auto.
    CHECK_FALSE(preferSoftwareDevice("d3d12", false));
    CHECK(preferSoftwareDevice("d3d12", true));
}

TEST_CASE("a grown atlas is fully re-uploaded even after atlasGrew was consumed",
          "[render][device]") {
    // GlyphAtlas::takeGrew() CONSUMES the flag, and rebuildFrame() runs several
    // times per presented frame, so the frame that actually reaches the render
    // thread can report a taller atlas with atlasGrew already false. Recreating
    // the texture without re-uploading all of it left every glyph in the new
    // upper half blank for the rest of the session.
    const auto rhi = makeRhi();
    REQUIRE(rhi != nullptr);
    Target target = makeTarget(*rhi);

    GpuResources gpu;
    gpu.setShaders(realShaders());
    gpu.setAtlasPixels(std::vector<std::uint8_t>(std::size_t{64} * 64, 0x7F));
    REQUIRE(drawOneFrame(*rhi, gpu, target, makeFrame()));

    FrameData grown = makeFrame();
    grown.atlasHeight = 128;
    grown.atlasGrew = false;  // already consumed by an earlier rebuild
    grown.atlasDirtyTop = 0;
    grown.atlasDirtyBottom = 0;  // and no row is reported dirty either

    // The pixels are still the OLD 64-row buffer: the upload has to stay
    // pending rather than report itself done against a half-empty texture.
    CHECK(drawOneFrame(*rhi, gpu, target, grown));
    CHECK(gpu.atlasBytes() == std::size_t{64} * 64);

    // Once the real pixels arrive the upload completes and stops repeating.
    gpu.setAtlasPixels(std::vector<std::uint8_t>(std::size_t{64} * 128, 0x40));
    CHECK(drawOneFrame(*rhi, gpu, target, grown));
    CHECK(gpu.atlasBytes() == std::size_t{64} * 128);
}

TEST_CASE("the path production actually takes after a device loss", "[render][device]") {
    // Qt Quick owns the renderer through the scene graph node and destroys it
    // when the graph is invalidated, so a real D3D device loss gives Krait a
    // FRESH GpuResources rather than exercising the rhi-changed branch above.
    // That path deserves its own case: nothing draws until the item hands the
    // atlas over, and one handover is enough.
    const auto rhi = makeRhi();
    REQUIRE(rhi != nullptr);
    Target target = makeTarget(*rhi);

    GpuResources gpu;  // as if just constructed by a new renderer
    gpu.setShaders(realShaders());
    CHECK_FALSE(gpu.hasAtlas());

    const FrameData frame = makeFrame();
    // Pipelines still come up — but with nothing in the texture, which is
    // exactly why the item must not skip the handover on a fresh renderer.
    CHECK(drawOneFrame(*rhi, gpu, target, frame));
    CHECK_FALSE(gpu.hasAtlas());

    gpu.setAtlasPixels(std::vector<std::uint8_t>(std::size_t{64} * 64, 0x7F));
    CHECK(drawOneFrame(*rhi, gpu, target, frame));
    CHECK(gpu.hasAtlas());
    CHECK(gpu.deviceResets() == 1);
}

// --- T84: the image pipeline ---

namespace {

// The frame above plus one image quad in one batch, drawn over the text.
FrameData makeImageFrame(std::uint32_t id) {
    FrameData frame = makeFrame();
    frame.images.push_back(krait::render::ImageInstance{
        .x = 0, .y = 0, .w = 32, .h = 32, .u0 = 0, .v0 = 0, .u1 = 1, .v1 = 1});
    frame.imageBatches.push_back(krait::render::ImageBatch{.imageId = id, .first = 0, .count = 1});
    frame.belowBatchCount = 0;
    return frame;
}

std::vector<std::uint32_t> solidPixels(int width, int height) {
    return std::vector<std::uint32_t>(static_cast<std::size_t>(width) * height, 0xFF3366CCU);
}

}  // namespace

TEST_CASE("an image gets a texture and draws", "[render][device][t84]") {
    const auto rhi = makeRhi();
    REQUIRE(rhi != nullptr);
    Target target = makeTarget(*rhi);

    GpuResources gpu;
    gpu.setShaders(realShaders());
    gpu.setAtlasPixels(std::vector<std::uint8_t>(std::size_t{64} * 64, 0x7F));
    CHECK_FALSE(gpu.hasImage(11));

    gpu.setImagePixels(11, 8, 8, solidPixels(8, 8), 1);
    CHECK(gpu.hasImage(11));
    CHECK(gpu.imageCount() == 1);

    CHECK(drawOneFrame(*rhi, gpu, target, makeImageFrame(11)));
    // Still held after the frame: the pixels are kept precisely so a lost
    // device can refill the texture without the core handing them over again.
    CHECK(gpu.hasImage(11));
}

TEST_CASE("a batch naming an image we never received is skipped, not fatal",
          "[render][device][t84]") {
    const auto rhi = makeRhi();
    REQUIRE(rhi != nullptr);
    Target target = makeTarget(*rhi);

    GpuResources gpu;
    gpu.setShaders(realShaders());
    gpu.setAtlasPixels(std::vector<std::uint8_t>(std::size_t{64} * 64, 0x7F));

    // No setImagePixels at all: the frame still has to draw its text.
    CHECK(drawOneFrame(*rhi, gpu, target, makeImageFrame(404)));
    CHECK(gpu.imageCount() == 0);
    CHECK(gpu.ready());
}

TEST_CASE("images survive a lost device", "[render][device][t84]") {
    // BOTH devices alive at once, as the atlas device-lost test above does.
    // Destroying the first and allocating the second is not a substitute: the
    // new QRhi lands on the freed address often enough to be reliable, and
    // GpuResources detects a lost device by POINTER IDENTITY — so the switch
    // silently does not register and the test asserts against its own setup.
    const auto first = makeRhi();
    const auto second = makeRhi();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    Target firstTarget = makeTarget(*first);
    Target secondTarget = makeTarget(*second);

    // Declared last so it is destroyed FIRST, before either device.
    GpuResources gpu;
    gpu.setShaders(realShaders());
    gpu.setAtlasPixels(std::vector<std::uint8_t>(std::size_t{64} * 64, 0x7F));
    gpu.setImagePixels(3, 8, 8, solidPixels(8, 8), 1);
    const FrameData frame = makeImageFrame(3);

    REQUIRE(drawOneFrame(*first, gpu, firstTarget, frame));
    CHECK(gpu.deviceResets() == 1);

    // No setImagePixels on the recovery path — that is the whole point. The
    // pixel copy GpuResources kept is what refills the new device's texture.
    CHECK(drawOneFrame(*second, gpu, secondTarget, frame));
    CHECK(gpu.deviceResets() == 2);
    CHECK(gpu.hasImage(3));
    CHECK(gpu.ready());
}

TEST_CASE("dropping an image releases it", "[render][device][t84]") {
    const auto rhi = makeRhi();
    REQUIRE(rhi != nullptr);
    Target target = makeTarget(*rhi);

    GpuResources gpu;
    gpu.setShaders(realShaders());
    gpu.setAtlasPixels(std::vector<std::uint8_t>(std::size_t{64} * 64, 0x7F));
    gpu.setImagePixels(1, 8, 8, solidPixels(8, 8), 1);
    gpu.setImagePixels(2, 8, 8, solidPixels(8, 8), 1);
    REQUIRE(drawOneFrame(*rhi, gpu, target, makeImageFrame(1)));
    REQUIRE(gpu.imageCount() == 2);

    const std::vector<std::uint32_t> ids = gpu.imageIds();
    CHECK(ids.size() == 2);

    gpu.dropImage(1);
    CHECK(gpu.imageCount() == 1);
    CHECK_FALSE(gpu.hasImage(1));
    CHECK(gpu.hasImage(2));
    // A frame naming the dropped id still draws its text.
    CHECK(drawOneFrame(*rhi, gpu, target, makeImageFrame(1)));
}

TEST_CASE("a retransmitted image id replaces its pixels", "[render][device][t84]") {
    const auto rhi = makeRhi();
    REQUIRE(rhi != nullptr);
    Target target = makeTarget(*rhi);

    GpuResources gpu;
    gpu.setShaders(realShaders());
    gpu.setAtlasPixels(std::vector<std::uint8_t>(std::size_t{64} * 64, 0x7F));

    // Sequence 4: the store's ordinal for the first transmission of id 1.
    gpu.setImagePixels(1, 8, 8, solidPixels(8, 8), 4);
    REQUIRE(drawOneFrame(*rhi, gpu, target, makeImageFrame(1)));
    CHECK(gpu.imageSequence(1) == 4);

    // kitty's refresh path: the same id, new pixels, a new store ordinal. The
    // caller compares sequences, so it must see a DIFFERENT number here — a
    // renderer keyed on the id alone shows the first picture forever.
    gpu.setImagePixels(1, 8, 8, solidPixels(8, 8), 9);
    CHECK(gpu.imageSequence(1) == 9);
    CHECK(gpu.imageCount() == 1);
    CHECK(drawOneFrame(*rhi, gpu, target, makeImageFrame(1)));
}

TEST_CASE("the GPU image set is bounded under a flood of ids", "[render][device][t84]") {
    const auto rhi = makeRhi();
    REQUIRE(rhi != nullptr);
    Target target = makeTarget(*rhi);

    GpuResources gpu;
    gpu.setShaders(realShaders());
    gpu.setAtlasPixels(std::vector<std::uint8_t>(std::size_t{64} * 64, 0x7F));

    // ImageStore evicts by BYTES and has no count cap, so a thousand 1x1
    // images live there forever — while each one costs a texture plus a
    // bindings object here. A stream transmitting a fresh id in a loop is the
    // hostile shape rules/net.md exists for.
    for (std::uint32_t id = 1; id <= 500; ++id) {
        gpu.setImagePixels(id, 1, 1, std::vector<std::uint32_t>(1, 0xFFFFFFFFU), id);
        REQUIRE(drawOneFrame(*rhi, gpu, target, makeImageFrame(id)));
    }
    CHECK(gpu.imageCount() <= 64);
    // Still drawing, not wedged.
    CHECK(gpu.ready());
}

TEST_CASE("a short pixel buffer is refused rather than uploaded", "[render][device][t84]") {
    GpuResources gpu;
    // 8x8 declared, 4 pixels supplied — the shape a truncated decode would
    // have, and an upload sized from the declaration would read off the end.
    gpu.setImagePixels(1, 8, 8, std::vector<std::uint32_t>(4, 0), 1);
    CHECK_FALSE(gpu.hasImage(1));
    CHECK(gpu.imageCount() == 0);
}

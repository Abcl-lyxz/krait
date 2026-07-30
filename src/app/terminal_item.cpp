#include "terminal_item.h"

#include "render/shaper/run_splitter.h"

#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <numeric>
#include <span>
#include <string_view>

namespace krait::app {
namespace {

constexpr std::array<float, 8> kCorners{
    0.0F, 0.0F,  //
    1.0F, 0.0F,  //
    0.0F, 1.0F,  //
    1.0F, 1.0F,  //
};

// Tried in order. The first that DirectWrite reports installed wins, so nothing
// is hardcoded to a font that may be absent (T31 makes this a setting).
constexpr std::array<std::string_view, 5> kFontCandidates{
    "Cascadia Mono", "Cascadia Code", "Consolas", "Lucida Console", "Courier New"};

QShader loadShader(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

}  // namespace

TerminalItem::TerminalItem() {
    setAcceptedMouseButtons(Qt::AllButtons);
    setFlag(ItemIsFocusScope, false);
    setFocus(true);

    bool ok = false;
    const int frames = qEnvironmentVariableIntValue("KRAIT_TERM_BENCH", &ok);
    if (ok && frames > 0) {
        m_benchFrames = frames;
        // Watchdog: a failed pipeline would otherwise idle forever and stall an
        // unattended bench run.
        QTimer::singleShot(60000, this, [] {
            qWarning("bench: timed out");
            QCoreApplication::exit(2);
        });
        if (qEnvironmentVariableIsSet("KRAIT_BENCH_4K")) {
            // Match the M0 baseline exactly, or "flood >= M0" compares nothing:
            // that run used a fixed 4K colour buffer and a 240x63 grid, both
            // independent of the window size.
            setFixedColorBufferWidth(3840);
            setFixedColorBufferHeight(2160);
            m_benchCols = 240;
            m_benchRows = 63;
        }
    }

    m_backend = new net::ConptyBackend(this);  // owned by this
    connect(m_backend, &net::ConptyBackend::outputReceived, this, &TerminalItem::handleOutput);
    connect(m_backend, &net::ConptyBackend::errorOccurred, this,
            [](const QString& code, const QString& message) {
                // Per-tab banner UI is T33; until then the log is the banner.
                qWarning("backend error [%s]: %s", qPrintable(code), qPrintable(message));
            });
    connect(m_backend, &net::ConptyBackend::exited, this,
            [](int exitCode) { qInfo("shell exited (%d)", exitCode); });
}

TerminalItem::~TerminalItem() {
    if (m_backend != nullptr) {
        m_backend->stop();
    }
}

bool TerminalItem::ensureFont() {
    if (m_builder) {
        return true;
    }
    m_fonts = std::make_unique<render::FontDb>();
    if (!m_fonts->valid()) {
        qWarning("render: DirectWrite unavailable; cannot resolve a font");
        return false;
    }
    const auto family = m_fonts->firstInstalled(kFontCandidates);
    if (!family.has_value()) {
        qWarning("render: none of the candidate monospace families are installed");
        return false;
    }
    m_family = *family;

    const auto spec = m_fonts->resolve(m_family, false, false, m_pxHeight);
    if (!spec.has_value()) {
        qWarning("render: '%s' resolved to no usable font file", m_family.c_str());
        return false;
    }
    m_pool = std::make_unique<render::ShapePool>();
    const auto faceId = m_pool->registerFace(*spec);
    if (!faceId.has_value()) {
        qWarning("render: FreeType could not open %s", spec->path.c_str());
        return false;
    }
    m_primaryFace = *faceId;

    const auto metrics = m_pool->metrics(m_primaryFace);
    if (!metrics.has_value() || metrics->cellWidth <= 0 || metrics->lineHeight <= 0) {
        qWarning("render: degenerate font metrics");
        return false;
    }
    m_atlas = std::make_unique<render::GlyphAtlas>(metrics->cellWidth, metrics->lineHeight);
    m_builder = std::make_unique<render::FrameBuilder>(*metrics, render::Theme{});
    m_raster = [this](std::uint32_t face, std::uint32_t glyph, render::GlyphBitmap& out) {
        return m_pool->rasterize(face, glyph, out);
    };
    qInfo("render: %s %dpx, cell %dx%d, %u shaping workers", m_family.c_str(), m_pxHeight,
          metrics->cellWidth, metrics->lineHeight, m_pool->workerCount());
    return true;
}

void TerminalItem::ensureStarted() {
    if (m_started || m_cols <= 0 || m_rows <= 0 || !m_builder) {
        return;
    }
    m_session = std::make_unique<core::vt::Session>(m_rows, m_cols);
    m_session->onReply = [this](const std::string& reply) {
        m_backend->writeInput(QByteArray(reply.data(), static_cast<qsizetype>(reply.size())));
    };
    if (m_benchFrames > 0) {
        // A bench run needs no shell: the flood is synthesised so the numbers
        // are reproducible rather than at the mercy of PowerShell's output.
        m_started = true;
        qInfo("bench: synthetic flood, %dx%d", m_cols, m_rows);
        // The churn is driven from the GUI thread, by a timer, and NOT from the
        // renderer. Two reasons, both learned the hard way: mutating the grid
        // from render() races the GUI thread (it crashed in release, where the
        // timing stopped hiding it), and QQuickRhiItemRenderer::update() asks
        // only for a re-RENDER, so a renderer-driven flood re-drew one static
        // frame forever and reported a fast number that measured nothing.
        auto* pump = new QTimer(this);  // owned by this
        pump->setInterval(0);
        connect(pump, &QTimer::timeout, this, [this, pump] {
            if (m_benchSteps >= m_benchFrames + 400) {
                pump->stop();  // the renderer reports well before this
                return;
            }
            update();  // QQuickItem::update: marks dirty so synchronize() runs
        });
        pump->start();
        rebuildFrame();
        return;
    }
    if (m_backend->start(m_cols, m_rows)) {
        m_started = true;
        qInfo("terminal started: %dx%d (powershell)", m_cols, m_rows);
        const QByteArray inject = qgetenv("KRAIT_TERM_INJECT");
        if (!inject.isEmpty()) {
            QTimer::singleShot(1200, this,
                               [this, inject] { m_backend->writeInput(inject + "\r"); });
        }
    }
}

void TerminalItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickRhiItem::geometryChange(newGeometry, oldGeometry);
    if (!ensureFont()) {
        return;
    }
    const render::FaceMetrics& metrics = m_builder->metrics();
    // A 4K bench run pins the grid so the workload matches the M0 baseline
    // rather than whatever the window happens to be.
    const int cols = m_benchCols > 0
                         ? m_benchCols
                         : std::max(2, static_cast<int>(newGeometry.width()) / metrics.cellWidth);
    const int rows =
        m_benchRows > 0
            ? m_benchRows
            : std::max(2, static_cast<int>(newGeometry.height()) / m_builder->cellHeight());
    if (cols == m_cols && rows == m_rows) {
        return;
    }
    m_cols = cols;
    m_rows = rows;
    if (m_started) {
        m_session->grid().resize(rows, cols);
        if (m_benchFrames == 0) {
            m_backend->resize(cols, rows);
        }
        // A resize is one of the three cases where a full rebuild is correct.
        m_builder->invalidate();
    } else {
        ensureStarted();
    }
    rebuildFrame();
    update();
}

void TerminalItem::handleOutput(const QByteArray& bytes) {
    if (!m_session) {
        return;
    }
    m_session->feed({reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                     static_cast<std::size_t>(bytes.size())});
    rebuildFrame();
    update();
}

void TerminalItem::rebuildFrame() {
    if (!m_session || !m_builder) {
        return;
    }
    core::vt::Grid& grid = m_session->grid();

    // viewportRows() is the scrolled-back-aware view (T21), NOT the raw screen:
    // a reader scrolled up must see history, not the live bottom.
    m_viewport = grid.viewportRows();

    render::FrameParams params;
    params.cols = grid.cols;
    params.selection = m_selection;
    params.cursor.visible = grid.viewOffset() == 0;  // hidden while scrolled back
    params.cursor.focused = hasActiveFocus();
    params.cursor.row = grid.row;
    params.cursor.col = grid.col;
    params.cursor.style = render::CursorStyle::Block;

    // ONE shaping batch for the whole frame, not one per row. FrameBuilder's
    // callback is per row, so shaping inside it would mean a separate blocking
    // round trip to the worker pool for every damaged row — 63 waits a frame on
    // a full screen, which is what "coalesce damage" in rules/render.md exists
    // to prevent. So: split every row that will be rebuilt, shape the lot in one
    // call, then hand build() a slice per row.
    m_runs.clear();
    m_rowRanges.assign(m_viewport.size(), {0, 0});
    for (int row = 0; row < static_cast<int>(m_viewport.size()); ++row) {
        if (!m_builder->rowNeedsRebuild(row, grid.damage)) {
            continue;
        }
        const auto begin = m_runs.size();
        render::splitRow(m_viewport[static_cast<std::size_t>(row)].cells, grid.clusters(), row,
                         m_runs);
        m_rowRanges[static_cast<std::size_t>(row)] = {begin, m_runs.size() - begin};
    }
    const auto shapeTimeout =
        m_benchFrames > 0 ? std::chrono::milliseconds{1000} : std::chrono::milliseconds{8};
    m_faces = render::shapeWithFallback(*m_pool, *m_fonts, m_runs, m_primaryFace, m_family,
                                        m_pxHeight, m_ligatures, m_shaped, shapeTimeout);

    m_builder->build(m_viewport, grid.damage, params, m_raster, *m_atlas, [&](int row) {
        const auto range = m_rowRanges[static_cast<std::size_t>(row)];
        return render::FrameBuilder::RowRuns{
            .runs = std::span(m_runs).subspan(range.first, range.second),
            .shaped = std::span(m_shaped).subspan(range.first, range.second),
            .faces = std::span(m_faces).subspan(range.first, range.second),
        };
    });
    grid.damage.clear();

    m_frame.solids.assign(m_builder->solids().begin(), m_builder->solids().end());
    m_frame.glyphs.assign(m_builder->glyphs().begin(), m_builder->glyphs().end());
    m_frame.atlasWidth = m_atlas->width();
    m_frame.atlasHeight = m_atlas->height();
    m_frame.atlasDirtyTop = m_atlas->dirtyTop();
    m_frame.atlasDirtyBottom = m_atlas->dirtyBottom();
    m_frame.atlasGrew = m_atlas->takeGrew();
    m_frame.pixelWidth = std::max(1, static_cast<int>(width()));
    m_frame.pixelHeight = std::max(1, static_cast<int>(height()));
    render::unpackColor(m_builder->theme().bg, m_frame.clearR, m_frame.clearG, m_frame.clearB);
    m_atlas->clearDirty();
}

const std::vector<std::uint8_t>* TerminalItem::atlasPixels() const {
    return m_atlas ? &m_atlas->pixels() : nullptr;
}

void TerminalItem::stepBench(int frame) {
    if (!m_session) {
        return;
    }
    ++m_benchSteps;
    if (m_benchSteps % 100 == 0) {
        // Evidence that the flood actually ran, and how much of it the shaped-run
        // cache absorbed. A bench whose churn silently stopped would otherwise
        // report a fast frame that measured nothing.
        qInfo("bench: step %d, rows %d, cache hits %llu misses %llu, rowsRebuilt %d", m_benchSteps,
              m_session->grid().rows, static_cast<unsigned long long>(m_pool->cacheHits()),
              static_cast<unsigned long long>(m_pool->cacheMisses()), m_builder->rowsRebuilt());
    }
    core::vt::Grid& grid = m_session->grid();
    // Every cell changes every frame: the worst realistic frame a terminal
    // produces, and the same workload shape the M0 baseline used.
    for (int row = 0; row < grid.rows; ++row) {
        grid.cursorSet(row, 0);
        for (int col = 0; col < grid.cols; ++col) {
            grid.putChar(static_cast<char32_t>(0x21 + ((row + col + frame) % 94)));
        }
    }
    grid.damage.markAll();
    rebuildFrame();
}

void TerminalItem::dumpAtlas(const QString& path) const {
    if (!m_atlas) {
        return;
    }
    const QImage image(m_atlas->pixels().data(), m_atlas->width(), m_atlas->height(),
                       m_atlas->width(), QImage::Format_Grayscale8);
    if (!image.copy().save(path)) {
        qWarning("render: cannot write atlas dump %s", qPrintable(path));
    }
}

void TerminalItem::finishBench(const QString& reportJson) {
    qInfo("bench: %s", qPrintable(reportJson));
    const QByteArray out = qgetenv("KRAIT_BENCH_OUT");
    if (!out.isEmpty()) {
        QFile file(QString::fromLocal8Bit(out));
        if (!file.open(QIODevice::WriteOnly)) {
            qWarning("bench: cannot write %s", out.constData());
            QCoreApplication::exit(1);  // never let stale JSON pass as fresh
            return;
        }
        file.write(reportJson.toUtf8());
    }
    const QByteArray dump = qgetenv("KRAIT_ATLAS_DUMP");
    if (!dump.isEmpty()) {
        dumpAtlas(QString::fromLocal8Bit(dump));
    }
    QCoreApplication::quit();
}

void TerminalItem::focusInEvent(QFocusEvent* event) {
    QQuickRhiItem::focusInEvent(event);
    rebuildFrame();  // the cursor changes from an outline to a filled block
    update();
}

void TerminalItem::focusOutEvent(QFocusEvent* event) {
    QQuickRhiItem::focusOutEvent(event);
    rebuildFrame();
    update();
}

void TerminalItem::cellAt(const QPointF& pos, int& row, int& col) const {
    if (!m_builder) {
        row = 0;
        col = 0;
        return;
    }
    const int cellW = std::max(1, m_builder->metrics().cellWidth);
    const int cellH = std::max(1, m_builder->cellHeight());
    col = std::clamp(static_cast<int>(pos.x()) / cellW, 0, std::max(0, m_cols - 1));
    row = std::clamp(static_cast<int>(pos.y()) / cellH, 0, std::max(0, m_rows - 1));
}

void TerminalItem::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QQuickRhiItem::mousePressEvent(event);
        return;
    }
    int row = 0;
    int col = 0;
    cellAt(event->position(), row, col);
    m_selection = {
        .active = false, .anchorRow = row, .anchorCol = col, .cursorRow = row, .cursorCol = col};
    m_dragging = true;
    forceActiveFocus();
    rebuildFrame();
    update();
    event->accept();
}

void TerminalItem::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging) {
        QQuickRhiItem::mouseMoveEvent(event);
        return;
    }
    int row = 0;
    int col = 0;
    cellAt(event->position(), row, col);
    // A press alone is not a selection — only a drag off the anchor is, or a
    // single click would wipe the previous selection and highlight one cell.
    m_selection.cursorRow = row;
    m_selection.cursorCol = col;
    m_selection.active = row != m_selection.anchorRow || col != m_selection.anchorCol;
    rebuildFrame();
    update();
    event->accept();
}

void TerminalItem::mouseReleaseEvent(QMouseEvent* event) {
    m_dragging = false;
    // Clipboard copy is T27's job; the rects are what T25 owes.
    QQuickRhiItem::mouseReleaseEvent(event);
}

void TerminalItem::keyPressEvent(QKeyEvent* event) {
    QByteArray bytes;
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        bytes = "\r";
        break;
    case Qt::Key_Backspace:
        bytes = "\x7f";
        break;
    case Qt::Key_Tab:
        bytes = "\t";
        break;
    case Qt::Key_Escape:
        bytes = "\x1b";
        break;
    case Qt::Key_Up:
        bytes = "\x1b[A";
        break;
    case Qt::Key_Down:
        bytes = "\x1b[B";
        break;
    case Qt::Key_Right:
        bytes = "\x1b[C";
        break;
    case Qt::Key_Left:
        bytes = "\x1b[D";
        break;
    default:
        if ((event->modifiers() & Qt::ControlModifier) != 0 && event->key() >= Qt::Key_A &&
            event->key() <= Qt::Key_Z) {
            bytes = QByteArray(1, static_cast<char>(event->key() - Qt::Key_A + 1));
        } else {
            bytes = event->text().toUtf8();  // printable text incl. Thai IME
        }
        break;
    }
    if (!bytes.isEmpty() && m_started) {
        // Any keypress snaps the viewport back to the live screen and drops the
        // selection, which is what every terminal does.
        if (m_session) {
            m_session->grid().scrollViewToBottom();
        }
        m_selection.active = false;
        m_backend->writeInput(bytes);
        rebuildFrame();
        update();
        event->accept();
        return;
    }
    QQuickRhiItem::keyPressEvent(event);
}

QQuickRhiItemRenderer* TerminalItem::createRenderer() {
    return new TerminalRenderer;  // owned by the scene graph node
}

void TerminalRenderer::synchronize(QQuickRhiItem* item) {
    auto* term = static_cast<TerminalItem*>(item);
    m_item = term;
    m_benchFrames = term->benchFrames();
    // Churn HERE, not in render(): synchronize() is the one phase Qt runs with
    // the GUI thread blocked, so this cannot race it — and it runs exactly once
    // per frame the GUI thread actually produced, which is what makes the frame
    // count and the churn count the same number.
    if (m_benchFrames > 0 && !m_benchDone) {
        term->stepBench(m_frameIndex);
        constexpr int kWarmup = 60;
        if (m_frameIndex == kWarmup) {
            m_timer.start();
        } else if (m_frameIndex > kWarmup) {
            m_cpuMs.push_back(static_cast<double>(m_timer.nsecsElapsed()) / 1e6);
            m_timer.restart();
        }
        ++m_frameIndex;
    }
    m_frame = term->frame();  // a copy: the render thread must not walk the grid
    if (const std::vector<std::uint8_t>* pixels = term->atlasPixels()) {
        // Copy only when something actually changed. A steady-state frame
        // touches no new glyph and so uploads nothing.
        if (m_frame.atlasGrew || m_frame.atlasDirtyBottom > m_frame.atlasDirtyTop ||
            m_atlasPixels.empty()) {
            m_atlasPixels = *pixels;
            m_atlasNeedsUpload = true;
        }
    }
}

void TerminalRenderer::initialize(QRhiCommandBuffer* cb) {
    ensureResources(cb);
}

void TerminalRenderer::ensureResources(QRhiCommandBuffer* cb) {
    if (m_rhi != rhi()) {
        // A device change invalidates every resource. T26 turns this into a
        // tested device-lost path; the reset itself already belongs here.
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
        m_atlasNeedsUpload = true;
        m_failed = false;
        m_rhi = rhi();
    }
    if (m_failed || m_rhi == nullptr || m_frame.atlasWidth == 0) {
        return;
    }

    QRhiResourceUpdateBatch* batch = m_rhi->nextResourceUpdateBatch();

    // The atlas texture is recreated only when the atlas GREW; otherwise the
    // dirty row range is uploaded into the existing one.
    if (!m_atlasTex || m_texWidth != m_frame.atlasWidth || m_texHeight != m_frame.atlasHeight) {
        m_atlasTex.reset(
            m_rhi->newTexture(QRhiTexture::R8, QSize(m_frame.atlasWidth, m_frame.atlasHeight), 1));
        if (!m_atlasTex->create()) {
            qWarning("render: atlas texture create() failed");
            m_atlasTex.reset();
            m_failed = true;
            return;
        }
        m_texWidth = m_frame.atlasWidth;
        m_texHeight = m_frame.atlasHeight;
        m_atlasNeedsUpload = true;
        m_glyphSrb.reset();  // the binding points at the old texture
    }

    if (m_atlasNeedsUpload && !m_atlasPixels.empty()) {
        const int top = m_frame.atlasGrew ? 0 : std::max(0, m_frame.atlasDirtyTop);
        const int bottom =
            m_frame.atlasGrew ? m_texHeight : std::min(m_texHeight, m_frame.atlasDirtyBottom);
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
        m_atlasNeedsUpload = false;
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
    const std::array<float, 4> ubufData{static_cast<float>(m_frame.pixelWidth),
                                        static_cast<float>(m_frame.pixelHeight), 0.0F, 0.0F};
    batch->updateDynamicBuffer(m_ubuf.get(), 0, sizeof(ubufData), ubufData.data());

    // Instance buffers grow but never shrink: a terminal's instance count
    // oscillates every frame and reallocating on each dip would churn.
    const auto solidBytes = static_cast<quint32>(std::max<std::size_t>(1, m_frame.solids.size()) *
                                                 sizeof(render::SolidInstance));
    if (!m_solidBuf || m_solidCapacity < solidBytes) {
        m_solidCapacity = solidBytes * 2;
        m_solidBuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, m_solidCapacity));
        m_solidBuf->create();
    }
    if (!m_frame.solids.empty()) {
        batch->updateDynamicBuffer(m_solidBuf.get(), 0, solidBytes, m_frame.solids.data());
    }

    const auto glyphBytes = static_cast<quint32>(std::max<std::size_t>(1, m_frame.glyphs.size()) *
                                                 sizeof(render::GlyphInstance));
    if (!m_glyphBuf || m_glyphCapacity < glyphBytes) {
        m_glyphCapacity = glyphBytes * 2;
        m_glyphBuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, m_glyphCapacity));
        m_glyphBuf->create();
    }
    if (!m_frame.glyphs.empty()) {
        batch->updateDynamicBuffer(m_glyphBuf.get(), 0, glyphBytes, m_frame.glyphs.data());
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
            {QRhiShaderStage::Vertex, loadShader(":/shaders/cell.vert.qsb")},
            {QRhiShaderStage::Fragment, loadShader(":/shaders/cell.frag.qsb")},
        });
        QRhiVertexInputLayout layout;
        layout.setBindings({
            {2 * sizeof(float)},
            {sizeof(render::SolidInstance), QRhiVertexInputBinding::PerInstance},
        });
        layout.setAttributes({
            {0, 0, QRhiVertexInputAttribute::Float2, 0},
            {1, 1, QRhiVertexInputAttribute::Float4, 0},
            {1, 2, QRhiVertexInputAttribute::Float4, 4 * sizeof(float)},
        });
        m_solidPipeline->setVertexInputLayout(layout);
        m_solidPipeline->setShaderResourceBindings(m_solidSrb.get());
        m_solidPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        m_solidPipeline->setSampleCount(renderTarget()->sampleCount());
        if (!m_solidPipeline->create()) {
            qWarning("render: solid pipeline create() FAILED");
            m_solidPipeline.reset();
            m_failed = true;
            return;
        }
    }
    if (!m_glyphPipeline) {
        m_glyphPipeline.reset(m_rhi->newGraphicsPipeline());
        m_glyphPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_glyphPipeline->setTargetBlends({blend});
        m_glyphPipeline->setShaderStages({
            {QRhiShaderStage::Vertex, loadShader(":/shaders/glyph.vert.qsb")},
            {QRhiShaderStage::Fragment, loadShader(":/shaders/glyph.frag.qsb")},
        });
        QRhiVertexInputLayout layout;
        layout.setBindings({
            {2 * sizeof(float)},
            {sizeof(render::GlyphInstance), QRhiVertexInputBinding::PerInstance},
        });
        layout.setAttributes({
            {0, 0, QRhiVertexInputAttribute::Float2, 0},
            {1, 1, QRhiVertexInputAttribute::Float4, 0},
            {1, 2, QRhiVertexInputAttribute::Float4, 4 * sizeof(float)},
            {1, 3, QRhiVertexInputAttribute::Float4, 8 * sizeof(float)},
        });
        m_glyphPipeline->setVertexInputLayout(layout);
        m_glyphPipeline->setShaderResourceBindings(m_glyphSrb.get());
        m_glyphPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        m_glyphPipeline->setSampleCount(renderTarget()->sampleCount());
        if (!m_glyphPipeline->create()) {
            qWarning("render: glyph pipeline create() FAILED");
            m_glyphPipeline.reset();
            m_failed = true;
            return;
        }
        qInfo("render: pipelines ready (atlas %dx%d, adapter %s)", m_frame.atlasWidth,
              m_frame.atlasHeight, m_rhi->driverInfo().deviceName.constData());
    }
}

void TerminalRenderer::render(QRhiCommandBuffer* cb) {
    ensureResources(cb);  // no active pass yet at this point
    const QColor clear = QColor::fromRgbF(m_frame.clearR, m_frame.clearG, m_frame.clearB);
    if (!m_solidPipeline || !m_glyphPipeline) {
        cb->beginPass(renderTarget(), clear, {1.0F, 0});
        cb->endPass();
        return;
    }

    if (m_benchFrames > 0 && !m_benchDone) {
        const double gpuSeconds = cb->lastCompletedGpuTime();
        if (gpuSeconds > 0.0) {
            m_gpuMs.push_back(gpuSeconds * 1000.0);
        }
        if (static_cast<int>(m_cpuMs.size()) >= m_benchFrames) {
            m_benchDone = true;
            reportBench();
        }
    }

    const QSize outputSize = renderTarget()->pixelSize();
    cb->beginPass(renderTarget(), clear, {1.0F, 0});
    cb->setViewport({0.0F, 0.0F, static_cast<float>(outputSize.width()),
                     static_cast<float>(outputSize.height())});

    // Backgrounds, selection and the cursor first, then glyphs over them.
    if (!m_frame.solids.empty()) {
        cb->setGraphicsPipeline(m_solidPipeline.get());
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput inputs[] = {
            {m_cornerBuf.get(), 0},
            {m_solidBuf.get(), 0},
        };
        cb->setVertexInput(0, 2, inputs);
        cb->draw(4, static_cast<quint32>(m_frame.solids.size()));
    }
    if (!m_frame.glyphs.empty()) {
        cb->setGraphicsPipeline(m_glyphPipeline.get());
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput inputs[] = {
            {m_cornerBuf.get(), 0},
            {m_glyphBuf.get(), 0},
        };
        cb->setVertexInput(0, 2, inputs);
        cb->draw(4, static_cast<quint32>(m_frame.glyphs.size()));
    }
    cb->endPass();
    // Deliberately NO update() here for the bench. The renderer driving itself
    // advances render() without a synchronize(), so the flood would re-draw one
    // static frame at thousands of fps and report a number that measured
    // nothing. The GUI-thread pump in ensureStarted() is the only driver.
}

void TerminalRenderer::reportBench() {
    std::vector<double> cpu = m_cpuMs;
    std::ranges::sort(cpu);
    const double avg =
        std::accumulate(cpu.begin(), cpu.end(), 0.0) / static_cast<double>(cpu.size());
    const double p99 = cpu[cpu.size() * 99 / 100];
    const double worst = cpu.back();
    const double fps = avg > 0.0 ? 1000.0 / avg : 0.0;
    double gpuAvg = 0.0;
    if (!m_gpuMs.empty()) {
        gpuAvg = std::accumulate(m_gpuMs.begin(), m_gpuMs.end(), 0.0) /
                 static_cast<double>(m_gpuMs.size());
    }
    const QSize size = renderTarget()->pixelSize();
    const QString json = QString::asprintf(
        "{\"target\":\"%dx%d\",\"frames\":%d,\"cpu_avg_ms\":%.3f,\"cpu_p99_ms\":%.3f,"
        "\"cpu_max_ms\":%.3f,\"fps\":%.1f,\"gpu_avg_ms\":%.3f,\"gpu_samples\":%d,"
        "\"solids\":%d,\"glyphs\":%d}",
        size.width(), size.height(), static_cast<int>(m_cpuMs.size()), avg, p99, worst, fps, gpuAvg,
        static_cast<int>(m_gpuMs.size()), static_cast<int>(m_frame.solids.size()),
        static_cast<int>(m_frame.glyphs.size()));
    // Queued: the item lives on the GUI thread and outlives the renderer.
    QMetaObject::invokeMethod(m_item, "finishBench", Qt::QueuedConnection, Q_ARG(QString, json));
}

}  // namespace krait::app

#include "terminal_item.h"

#include <QFile>
#include <QKeyEvent>
#include <QTimer>

#include <algorithm>

namespace krait::app {

namespace {

constexpr int kInstanceFloats = 9;
constexpr int kInstanceStride = kInstanceFloats * sizeof(float);

constexpr float kCorners[] = {
    0.0F, 0.0F,  //
    1.0F, 0.0F,  //
    0.0F, 1.0F,  //
    1.0F, 1.0F,  //
};

// Standard 16-color ANSI palette (VGA-ish), rgb 0..1.
constexpr float kPalette[16][3] = {
    {0.10F, 0.10F, 0.10F}, {0.80F, 0.25F, 0.25F}, {0.30F, 0.75F, 0.35F}, {0.80F, 0.70F, 0.30F},
    {0.30F, 0.45F, 0.85F}, {0.70F, 0.40F, 0.80F}, {0.30F, 0.70F, 0.75F}, {0.85F, 0.85F, 0.85F},
    {0.45F, 0.45F, 0.45F}, {1.00F, 0.45F, 0.45F}, {0.45F, 0.95F, 0.55F}, {1.00F, 0.90F, 0.50F},
    {0.50F, 0.65F, 1.00F}, {0.90F, 0.60F, 1.00F}, {0.50F, 0.90F, 0.95F}, {1.00F, 1.00F, 1.00F},
};

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

    m_atlas = render::buildAsciiAtlas(
        {
            "C:/Windows/Fonts/CascadiaMono.ttf",
            "C:/Windows/Fonts/consola.ttf",
        },
        20);
    m_backend = new net::ConptyBackend(this);  // owned by this
    connect(m_backend, &net::ConptyBackend::outputReceived, this, &TerminalItem::handleOutput);
    connect(m_backend, &net::ConptyBackend::errorOccurred, this,
            [](const QString& code, const QString& message) {
                // Per-tab banner UI is M1; for M0 the log is the banner.
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

void TerminalItem::ensureStarted() {
    if (m_started || m_cols <= 0 || m_rows <= 0 || !m_atlas.valid()) {
        return;
    }
    m_session = std::make_unique<core::vt::Session>(m_rows, m_cols);
    m_session->onReply = [this](const std::string& reply) {
        m_backend->writeInput(QByteArray(reply.data(), static_cast<qsizetype>(reply.size())));
    };
    if (m_backend->start(m_cols, m_rows)) {
        m_started = true;
        qInfo("terminal started: %dx%d (powershell)", m_cols, m_rows);
        // Unattended verification: KRAIT_TERM_INJECT types a command (CR
        // appended) once the shell has settled — pairs with the screenshot
        // hook for T15 evidence. The plan's manual gate stays manual.
        const QByteArray inject = qgetenv("KRAIT_TERM_INJECT");
        if (!inject.isEmpty()) {
            QTimer::singleShot(1200, this,
                               [this, inject] { m_backend->writeInput(inject + "\r"); });
        }
    }
}

void TerminalItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickRhiItem::geometryChange(newGeometry, oldGeometry);
    if (!m_atlas.valid()) {
        return;
    }
    const int cols = std::max(2, static_cast<int>(newGeometry.width()) / m_atlas.cellWidth);
    const int rows = std::max(2, static_cast<int>(newGeometry.height()) / m_atlas.cellHeight);
    if (cols == m_cols && rows == m_rows) {
        return;
    }
    m_cols = cols;
    m_rows = rows;
    if (m_started) {
        m_session->grid().resize(rows, cols);
        m_backend->resize(cols, rows);
    } else {
        ensureStarted();
    }
    rebuildInstances();
    update();
}

void TerminalItem::handleOutput(const QByteArray& bytes) {
    if (!m_session) {
        return;
    }
    m_session->feed({reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                     static_cast<std::size_t>(bytes.size())});
    rebuildInstances();
    update();
}

void TerminalItem::rebuildInstances() {
    if (!m_session) {
        return;
    }
    const auto& grid = m_session->grid();
    const std::size_t count =
        static_cast<std::size_t>(grid.rows) * static_cast<std::size_t>(grid.cols);
    m_instances.resize(count * kInstanceFloats);
    for (int r = 0; r < grid.rows; ++r) {
        for (int c = 0; c < grid.cols; ++c) {
            const auto& cell = grid.cellAt(r, c);
            float* inst = m_instances.data() +
                          (static_cast<std::size_t>(r) * grid.cols + c) * kInstanceFloats;
            const char32_t ch = cell.ch;
            inst[0] = (ch >= 0x20 && ch <= 0x7E)
                          ? static_cast<float>(ch - 0x20)
                          : (ch == 0 ? 0.0F : static_cast<float>('?' - 0x20));
            const auto& attr = cell.attr;
            float fg[3] = {0.86F, 0.87F, 0.89F};  // default fg
            float bg[3] = {0.05F, 0.06F, 0.09F};  // default bg
            // The spike palette only holds the classic 16. Since T17 made
            // 256-index and truecolor reachable, masking with & 0x0F would
            // render index 196 as a confidently WRONG colour — worse than the
            // default it used to fall back to. Leave anything this palette
            // cannot represent at the default until the real renderer (T25).
            if (attr.fg.kind == core::vt::Color::Kind::Indexed && attr.fg.index < 16) {
                int idx = attr.fg.index;
                if ((attr.flags & core::vt::Attr::kBold) != 0 && idx < 8) {
                    idx += 8;  // bold brightens the classic 8
                }
                fg[0] = kPalette[idx][0];
                fg[1] = kPalette[idx][1];
                fg[2] = kPalette[idx][2];
            }
            if (attr.bg.kind == core::vt::Color::Kind::Indexed && attr.bg.index < 16) {
                const int idx = attr.bg.index;
                bg[0] = kPalette[idx][0];
                bg[1] = kPalette[idx][1];
                bg[2] = kPalette[idx][2];
            }
            const bool cursorHere = (r == grid.row && c == grid.col);
            const bool swap = ((attr.flags & core::vt::Attr::kReverse) != 0) != cursorHere;
            inst[1] = swap ? bg[0] : fg[0];
            inst[2] = swap ? bg[1] : fg[1];
            inst[3] = swap ? bg[2] : fg[2];
            inst[4] = 1.0F;
            inst[5] = swap ? fg[0] : bg[0];
            inst[6] = swap ? fg[1] : bg[1];
            inst[7] = swap ? fg[2] : bg[2];
            inst[8] = 1.0F;
        }
    }
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
        m_backend->writeInput(bytes);
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
    m_atlas = term->atlas();
    m_instances = term->instances();
    m_cols = term->cols();
    m_rows = term->rows();
}

void TerminalRenderer::initialize(QRhiCommandBuffer* cb) {
    ensureResources(cb);
}

void TerminalRenderer::ensureResources(QRhiCommandBuffer* cb) {
    if (m_rhi != rhi()) {
        m_pipeline.reset();
        m_srb.reset();
        m_ubuf.reset();
        m_instanceBuf.reset();
        m_cornerBuf.reset();
        m_sampler.reset();
        m_atlasTex.reset();
        m_failed = false;
        m_rhi = rhi();
    }
    if (m_pipeline || m_failed || !m_atlas.valid()) {
        return;
    }

    QRhiResourceUpdateBatch* u = m_rhi->nextResourceUpdateBatch();

    const QImage& img = m_atlas.image;
    m_atlasTex.reset(m_rhi->newTexture(QRhiTexture::R8, img.size(), 1));
    if (!m_atlasTex->create()) {
        m_atlasTex.reset();
        m_failed = true;
        return;
    }
    QByteArray pixels(static_cast<qsizetype>(img.width()) * img.height(), 0);
    for (int y = 0; y < img.height(); ++y) {
        memcpy(pixels.data() + static_cast<qsizetype>(y) * img.width(), img.constScanLine(y),
               static_cast<std::size_t>(img.width()));
    }
    QRhiTextureSubresourceUploadDescription sub(pixels.constData(), pixels.size());
    sub.setDataStride(img.width());
    u->uploadTexture(m_atlasTex.get(), QRhiTextureUploadDescription({0, 0, sub}));

    m_sampler.reset(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    m_sampler->create();

    m_cornerBuf.reset(
        m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kCorners)));
    m_cornerBuf->create();
    u->uploadStaticBuffer(m_cornerBuf.get(), kCorners);

    m_ubuf.reset(
        m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 4 * sizeof(float)));
    m_ubuf->create();

    cb->resourceUpdate(u);

    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage,
                                                 m_ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  m_atlasTex.get(), m_sampler.get()),
    });
    m_srb->create();

    m_pipeline.reset(m_rhi->newGraphicsPipeline());
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(":/shaders/grid.vert.qsb")},
        {QRhiShaderStage::Fragment, loadShader(":/shaders/grid.frag.qsb")},
    });
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({
        {2 * sizeof(float)},
        {kInstanceStride, QRhiVertexInputBinding::PerInstance},
    });
    inputLayout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {1, 1, QRhiVertexInputAttribute::Float, 0},
        {1, 2, QRhiVertexInputAttribute::Float4, 1 * sizeof(float)},
        {1, 3, QRhiVertexInputAttribute::Float4, 5 * sizeof(float)},
    });
    m_pipeline->setVertexInputLayout(inputLayout);
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_pipeline->setSampleCount(renderTarget()->sampleCount());
    if (!m_pipeline->create()) {
        qWarning("terminal: pipeline create() failed");
        m_pipeline.reset();
        m_failed = true;
    }
}

void TerminalRenderer::render(QRhiCommandBuffer* cb) {
    ensureResources(cb);
    const QColor clear = QColor::fromRgbF(0.05F, 0.06F, 0.09F);
    const int cells = m_cols * m_rows;
    if (!m_pipeline || cells <= 0 || m_instances.empty()) {
        cb->beginPass(renderTarget(), clear, {1.0F, 0});
        cb->endPass();
        return;
    }
    QRhiResourceUpdateBatch* u = m_rhi->nextResourceUpdateBatch();
    const quint32 byteSize = static_cast<quint32>(m_instances.size() * sizeof(float));
    if (!m_instanceBuf || m_instanceBuf->size() < byteSize) {
        m_instanceBuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, byteSize));
        m_instanceBuf->create();
    }
    u->updateDynamicBuffer(m_instanceBuf.get(), 0, byteSize, m_instances.data());
    const float ubufData[4] = {static_cast<float>(m_cols), static_cast<float>(m_rows), 0.0F, 0.0F};
    u->updateDynamicBuffer(m_ubuf.get(), 0, sizeof(ubufData), ubufData);

    cb->beginPass(renderTarget(), clear, {1.0F, 0}, u);
    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setShaderResources();
    const QSize outputSize = renderTarget()->pixelSize();
    cb->setViewport({0.0F, 0.0F, static_cast<float>(outputSize.width()),
                     static_cast<float>(outputSize.height())});
    const QRhiCommandBuffer::VertexInput vbufs[] = {
        {m_cornerBuf.get(), 0},
        {m_instanceBuf.get(), 0},
    };
    cb->setVertexInput(0, 2, vbufs);
    cb->draw(4, cells);
    cb->endPass();
}

}  // namespace krait::app

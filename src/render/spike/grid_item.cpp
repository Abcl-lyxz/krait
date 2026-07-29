#include "grid_item.h"

#include <QCoreApplication>
#include <QFile>
#include <QTimer>

#include <algorithm>
#include <numeric>

namespace krait::render {

namespace {

// Per-instance layout: glyph index, fg rgba, bg rgba = 9 floats.
constexpr int kInstanceFloats = 9;
constexpr int kInstanceStride = kInstanceFloats * sizeof(float);
constexpr int kCellCount = GridSpikeItem::kCols * GridSpikeItem::kRows;

constexpr float kCorners[] = {
    0.0F, 0.0F,  //
    1.0F, 0.0F,  //
    0.0F, 1.0F,  //
    1.0F, 1.0F,  //
};

QShader loadShader(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

}  // namespace

GridSpikeItem::GridSpikeItem() {
    m_atlas = buildAsciiAtlas(
        {
            "C:/Windows/Fonts/CascadiaMono.ttf",
            "C:/Windows/Fonts/consola.ttf",
        },
        24);
    if (!m_atlas.valid()) {
        qWarning("spike: no monospace font could be rasterized");
    } else {
        qInfo("spike: atlas built %dx%d", m_atlas.image.width(), m_atlas.image.height());
        const QByteArray dump = qgetenv("KRAIT_SPIKE_ATLAS_DUMP");
        if (!dump.isEmpty()) {
            m_atlas.image.save(QString::fromLocal8Bit(dump));
        }
    }
    bool ok = false;
    const int frames = qEnvironmentVariableIntValue("KRAIT_BENCH", &ok);
    if (ok && frames > 0) {
        m_benchFrames = frames;
        // Watchdog: a failed pipeline would otherwise idle forever and
        // stall flood-report.cmd unattended.
        QTimer::singleShot(60000, this, [] {
            qWarning("bench: timed out");
            QCoreApplication::exit(2);
        });
        if (qEnvironmentVariableIsSet("KRAIT_BENCH_4K")) {
            // Honest 4K fill cost regardless of the window/monitor size.
            setFixedColorBufferWidth(3840);
            setFixedColorBufferHeight(2160);
        }
    }
}

void GridSpikeItem::finishBench(const QString& reportJson) {
    qInfo("bench: %s", qPrintable(reportJson));
    const QByteArray out = qgetenv("KRAIT_BENCH_OUT");
    if (!out.isEmpty()) {
        QFile f(QString::fromLocal8Bit(out));
        if (!f.open(QIODevice::WriteOnly)) {
            qWarning("bench: cannot write %s", out.constData());
            QCoreApplication::exit(1);  // never let stale JSON pass as fresh
            return;
        }
        f.write(reportJson.toUtf8());
    }
    QCoreApplication::quit();
}

QQuickRhiItemRenderer* GridSpikeItem::createRenderer() {
    return new GridSpikeRenderer;  // owned by the scene graph node
}

void GridSpikeRenderer::synchronize(QQuickRhiItem* item) {
    auto* gridItem = static_cast<GridSpikeItem*>(item);
    m_atlas = gridItem->atlas();
    m_item = gridItem;
    m_benchFrames = gridItem->benchFrames();
}

void GridSpikeRenderer::fillInstances() {
    m_instances.resize(static_cast<std::size_t>(kCellCount) * kInstanceFloats);
    // A recognizable fill: cycling ASCII with a few color bands.
    for (int i = 0; i < kCellCount; ++i) {
        float* inst = m_instances.data() + static_cast<std::size_t>(i) * kInstanceFloats;
        inst[0] = static_cast<float>(i % 95);  // glyph index
        const int band = (i / GridSpikeItem::kCols) % 4;
        const float r = band == 0 || band == 3 ? 0.9F : 0.4F;
        const float g = band == 1 || band == 3 ? 0.9F : 0.5F;
        const float b = band == 2 || band == 3 ? 0.9F : 0.6F;
        inst[1] = r;
        inst[2] = g;
        inst[3] = b;
        inst[4] = 1.0F;  // fg alpha
        inst[5] = 0.05F;
        inst[6] = 0.06F;
        inst[7] = 0.09F;
        inst[8] = 1.0F;  // bg alpha
    }
}

void GridSpikeRenderer::initialize(QRhiCommandBuffer* cb) {
    ensureResources(cb);
}

void GridSpikeRenderer::ensureResources(QRhiCommandBuffer* cb) {
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
        qWarning("spike: atlas texture create() failed");
        m_atlasTex.reset();
        m_failed = true;  // retried only after an rhi/device change
        return;
    }
    // Tight-pack the rows: QImage scanlines are 4-byte aligned.
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

    fillInstances();
    m_instanceBuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                                         static_cast<quint32>(m_instances.size() * sizeof(float))));
    m_instanceBuf->create();
    u->updateDynamicBuffer(m_instanceBuf.get(), 0,
                           static_cast<quint32>(m_instances.size() * sizeof(float)),
                           m_instances.data());

    const float ubufData[4] = {static_cast<float>(GridSpikeItem::kCols),
                               static_cast<float>(GridSpikeItem::kRows), 0.0F, 0.0F};
    m_ubuf.reset(
        m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(ubufData)));
    m_ubuf->create();
    u->updateDynamicBuffer(m_ubuf.get(), 0, sizeof(ubufData), ubufData);

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
        qWarning("spike: grid pipeline create() FAILED");
        m_pipeline.reset();
        m_failed = true;  // retried only after an rhi/device change
    } else {
        qInfo("spike: grid pipeline ready (atlas %dx%d, cell %dx%d, adapter %s)",
              m_atlas.image.width(), m_atlas.image.height(), m_atlas.cellWidth, m_atlas.cellHeight,
              m_rhi->driverInfo().deviceName.constData());
    }
}

void GridSpikeRenderer::render(QRhiCommandBuffer* cb) {
    ensureResources(cb);  // no active pass yet at this point
    const QColor clear = QColor::fromRgbF(0.02F, 0.02F, 0.04F);
    if (!m_pipeline) {
        cb->beginPass(renderTarget(), clear, {1.0F, 0});
        cb->endPass();
        return;
    }
    QRhiResourceUpdateBatch* batch = nullptr;
    if (m_benchFrames > 0 && !m_benchDone) {
        // T13 flood: every cell changes glyph and fg every frame, and the
        // entire per-instance buffer is re-uploaded — the worst realistic
        // frame a terminal produces.
        for (int i = 0; i < kCellCount; ++i) {
            float* inst = m_instances.data() + static_cast<std::size_t>(i) * kInstanceFloats;
            inst[0] = static_cast<float>((i + m_frame) % 95);
            inst[1] = 0.2F + static_cast<float>((i + m_frame) % 7) / 8.0F;
        }
        batch = m_rhi->nextResourceUpdateBatch();
        batch->updateDynamicBuffer(m_instanceBuf.get(), 0,
                                   static_cast<quint32>(m_instances.size() * sizeof(float)),
                                   m_instances.data());
        constexpr int kWarmup = 60;
        if (m_frame == kWarmup) {
            m_timer.start();
        } else if (m_frame > kWarmup) {
            m_cpuMs.push_back(static_cast<double>(m_timer.nsecsElapsed()) / 1e6);
            m_timer.restart();
            const double gpuSeconds = cb->lastCompletedGpuTime();
            if (gpuSeconds > 0.0) {
                m_gpuMs.push_back(gpuSeconds * 1000.0);
            }
        }
        ++m_frame;
        if (static_cast<int>(m_cpuMs.size()) >= m_benchFrames) {
            m_benchDone = true;
            reportBench();
        }
    }
    cb->beginPass(renderTarget(), clear, {1.0F, 0}, batch);
    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setShaderResources();  // binds the pipeline's srb (ubuf + atlas)
    const QSize outputSize = renderTarget()->pixelSize();
    cb->setViewport({0.0F, 0.0F, static_cast<float>(outputSize.width()),
                     static_cast<float>(outputSize.height())});
    const QRhiCommandBuffer::VertexInput vbufs[] = {
        {m_cornerBuf.get(), 0},
        {m_instanceBuf.get(), 0},
    };
    cb->setVertexInput(0, 2, vbufs);
    cb->draw(4, kCellCount);
    cb->endPass();
    if (m_benchFrames > 0 && !m_benchDone) {
        update();  // keep the flood running at presentation rate
    }
}

void GridSpikeRenderer::reportBench() {
    std::vector<double> cpu = m_cpuMs;
    std::sort(cpu.begin(), cpu.end());
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
    const QString json =
        QString::asprintf("{\"target\":\"%dx%d\",\"frames\":%d,\"cpu_avg_ms\":%.3f,"
                          "\"cpu_p99_ms\":%.3f,\"cpu_max_ms\":%.3f,\"fps\":%.1f,"
                          "\"gpu_avg_ms\":%.3f,\"gpu_samples\":%d}",
                          size.width(), size.height(), static_cast<int>(m_cpuMs.size()), avg, p99,
                          worst, fps, gpuAvg, static_cast<int>(m_gpuMs.size()));
    // Queued: the item lives on the GUI thread and outlives the renderer.
    QMetaObject::invokeMethod(m_item, "finishBench", Qt::QueuedConnection, Q_ARG(QString, json));
}

}  // namespace krait::render

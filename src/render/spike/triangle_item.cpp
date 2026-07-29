#include "triangle_item.h"

#include <QFile>

namespace krait::render {

namespace {

// x, y, r, g, b
constexpr float kVertices[] = {
    0.0F,  0.5F,  1.0F, 0.3F, 0.1F,  //
    -0.5F, -0.5F, 0.1F, 0.9F, 0.4F,  //
    0.5F,  -0.5F, 0.2F, 0.4F, 1.0F,  //
};

QShader loadShader(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

}  // namespace

QQuickRhiItemRenderer* TriangleItem::createRenderer() {
    return new TriangleRenderer;  // owned by the scene graph node
}

void TriangleRenderer::initialize(QRhiCommandBuffer* cb) {
    if (m_rhi != rhi()) {
        m_pipeline.reset();
        m_srb.reset();
        m_vbuf.reset();
        m_rhi = rhi();
    }
    if (m_pipeline) {
        return;
    }

    m_vbuf.reset(
        m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kVertices)));
    m_vbuf->create();
    QRhiResourceUpdateBatch* updates = m_rhi->nextResourceUpdateBatch();
    updates->uploadStaticBuffer(m_vbuf.get(), kVertices);
    cb->resourceUpdate(updates);

    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->create();

    m_pipeline.reset(m_rhi->newGraphicsPipeline());
    m_pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(":/shaders/triangle.vert.qsb")},
        {QRhiShaderStage::Fragment, loadShader(":/shaders/triangle.frag.qsb")},
    });
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({{5 * sizeof(float)}});
    inputLayout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float3, 2 * sizeof(float)},
    });
    m_pipeline->setVertexInputLayout(inputLayout);
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_pipeline->setSampleCount(renderTarget()->sampleCount());
    if (!m_pipeline->create()) {
        m_pipeline.reset();  // render() then clears and skips the draw
    }
}

void TriangleRenderer::synchronize(QQuickRhiItem*) {}

void TriangleRenderer::render(QRhiCommandBuffer* cb) {
    const QColor clear = QColor::fromRgbF(0.05F, 0.06F, 0.09F);
    if (!m_pipeline) {  // pipeline creation failed: clear only, no draw
        cb->beginPass(renderTarget(), clear, {1.0F, 0});
        cb->endPass();
        return;
    }
    cb->beginPass(renderTarget(), clear, {1.0F, 0});
    cb->setGraphicsPipeline(m_pipeline.get());
    const QSize outputSize = renderTarget()->pixelSize();
    cb->setViewport({0.0F, 0.0F, static_cast<float>(outputSize.width()),
                     static_cast<float>(outputSize.height())});
    const QRhiCommandBuffer::VertexInput vbufBinding(m_vbuf.get(), 0);
    cb->setVertexInput(0, 1, &vbufBinding);
    cb->draw(3);
    cb->endPass();
}

}  // namespace krait::render

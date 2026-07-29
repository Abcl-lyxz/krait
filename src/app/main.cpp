#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

namespace {

const char* apiName(QSGRendererInterface::GraphicsApi api) {
    switch (api) {
    case QSGRendererInterface::Direct3D11:
        return "D3D11";
    case QSGRendererInterface::Direct3D12:
        return "D3D12";
    case QSGRendererInterface::Vulkan:
        return "Vulkan";
    case QSGRendererInterface::OpenGL:
        return "OpenGL";
    case QSGRendererInterface::Software:
        return "Software";
    default:
        return "other";
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    // ADR-0001: D3D11 is the primary QRhi backend on Windows.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);

    QGuiApplication app(argc, argv);
    app.setApplicationName("Krait");
    qInfo("krait starting");

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Krait", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (window != nullptr) {
        // Queried on the GUI thread after the first frames; the
        // sceneGraphInitialized signal is emitted on the render thread and
        // proved unreliable to observe from here.
        QTimer::singleShot(1000, window, [window] {
            qInfo("rhi backend: %s", apiName(window->rendererInterface()->graphicsApi()));
        });
    }

    // Headless verification hook: KRAIT_SPIKE_AUTOQUIT closes after 2 s so
    // the T11 gate (log line above) can run unattended.
    if (qEnvironmentVariableIsSet("KRAIT_SPIKE_AUTOQUIT")) {
        QTimer::singleShot(2000, &app, &QCoreApplication::quit);
    }
    return app.exec();
}

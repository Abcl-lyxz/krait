// These two DEFINES must precede every include in this file, and that is what
// the position here buys — terminal_item.h reaches <windows.h> through
// conpty_backend.h, which does not guard it, so a define placed after them is
// dead and the min/max macros land in scope for the whole translation unit.
// Where clang-format then sorts the <windows.h> line below is irrelevant: the
// header is include-guarded and the macros are already set by the time anything
// pulls it in.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "gpu_policy.h"
#include "settings/paths.h"
#include "settings/registry.h"
#include "terminal_item.h"
#include <windows.h>
// Same guards as src/render/shaper/fontdb.cpp: without NOMINMAX the min/max
// macros land in scope for the whole translation unit.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QTranslator>

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

    // Settings before the window: the GPU adapter choice below reads one, and
    // QQuickGraphicsConfiguration has to be set before the scene graph starts.
    namespace ks = krait::app::settings;
    const ks::Resolution configDir = ks::resolveConfigDir(
        ks::systemPathInputs(), [](const QString& path) { return QFile::exists(path); });
    QDir().mkpath(configDir.dir);
    ks::Registry registry;
    registry.load(ks::configFilePath(configDir.dir));
    // Write it back on first run so there is a file to edit. A settings system
    // whose file only appears after you change something in the UI is one
    // nobody discovers.
    if (!QFile::exists(ks::configFilePath(configDir.dir))) {
        registry.save();
    }
    registry.setWatching(true);
    qInfo("settings: %s (%s)", qPrintable(ks::configFilePath(configDir.dir)),
          qPrintable(ks::describeSource(configDir.source)));

    // Language (T32). "system" follows the OS; "en"/"th" pin it. Installed
    // BEFORE the QML engine loads, because qsTr() bindings evaluate as objects
    // are created and a translator installed afterwards leaves the first window
    // in English until something re-evaluates.
    const std::string language = registry.text("ui.language");
    const QLocale locale = language == "system" || language.empty()
                               ? QLocale()
                               : QLocale(QString::fromStdString(language));
    QTranslator appTranslator;
    const bool translationsLoaded = appTranslator.load(
        locale, QStringLiteral("krait"), QStringLiteral("_"), QStringLiteral(":/i18n"));
    if (translationsLoaded) {
        QGuiApplication::installTranslator(&appTranslator);
    }
    // Qt's own strings (standard dialogs, shortcuts) come from a separate
    // catalogue; without it a Thai UI is Thai with English scattered through it.
    QTranslator qtTranslator;
    if (qtTranslator.load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QGuiApplication::installTranslator(&qtTranslator);
    }
    // Says whether the catalogue actually loaded, not just which locale was
    // asked for. A missing .qm is silent otherwise: the UI is simply in English
    // and nothing anywhere says why.
    qInfo("locale: %s (setting '%s'), krait translations %s", qPrintable(locale.name()),
          language.c_str(), translationsLoaded ? "loaded" : "NOT FOUND");

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("benchMode", qEnvironmentVariableIsSet("KRAIT_BENCH"));
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Krait", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    // Every terminal reads the same live registry rather than caching its own
    // copy, so a hot reload reaches all of them.
    for (QObject* root : engine.rootObjects()) {
        for (krait::app::TerminalItem* item : root->findChildren<krait::app::TerminalItem*>()) {
            item->setSettings(&registry);
        }
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (window != nullptr) {
        // Before scene graph init (first expose): GPU timestamps for the
        // bench, and the WARP software adapter when benching that leg.
        QQuickGraphicsConfiguration config;
        config.setTimestamps(true);
        // T26 adapter selection. SM_REMOTESESSION is the documented RDP probe;
        // a hardware D3D11 device inside an RDP session is emulated anyway, and
        // on some hosts fails to create at all. KRAIT_BENCH_WARP stays honoured
        // so the WARP leg of the flood bench keeps working unchanged.
        const bool remote = GetSystemMetrics(SM_REMOTESESSION) != 0;
        // The environment wins over the setting: KRAIT_GPU is the escape hatch
        // for a machine whose config cannot be edited because the app will not
        // start on it, which is exactly when it is needed.
        QByteArray gpu = qgetenv("KRAIT_GPU").toLower();
        if (gpu.isEmpty()) {
            gpu = QByteArray::fromStdString(registry.text("gpu.adapter"));
        }
        const bool software = krait::app::preferSoftwareDevice(gpu.toStdString(), remote) ||
                              qEnvironmentVariableIsSet("KRAIT_BENCH_WARP");
        config.setPreferSoftwareDevice(software);
        qInfo("gpu: remote session %s, KRAIT_GPU='%s' -> %s adapter", remote ? "yes" : "no",
              gpu.constData(), software ? "software (WARP)" : "hardware");
        window->setGraphicsConfiguration(config);

        // Without a slot here Qt prints the message, shows a MESSAGE BOX and
        // terminates — and rules/ui.md bans app-modal surfaces outright. Must
        // be connected before show(), same as the graphics configuration.
        QObject::connect(window, &QQuickWindow::sceneGraphError, &app,
                         [software](QQuickWindow::SceneGraphError, const QString& message) {
                             qCritical("gpu: scene graph init failed: %s", qPrintable(message));
                             if (!software) {
                                 qCritical("gpu: retry with KRAIT_GPU=warp to force the software "
                                           "adapter");
                             }
                             QCoreApplication::exit(3);
                         });
        window->setVisible(true);  // deferred so the config precedes sg init
        // Queried on the GUI thread after the first frames; the
        // sceneGraphInitialized signal is emitted on the render thread and
        // proved unreliable to observe from here.
        QTimer::singleShot(1000, window, [window] {
            qInfo("rhi backend: %s", apiName(window->rendererInterface()->graphicsApi()));
        });
        // KRAIT_SPIKE_SCREENSHOT=<path>: grab the first rendered frames to a
        // PNG so visual gates (T12 "grid of glyphs visible") leave evidence.
        const QByteArray shotPath = qgetenv("KRAIT_SPIKE_SCREENSHOT");
        if (!shotPath.isEmpty()) {
            QTimer::singleShot(2500, window, [window, shotPath] {
                const QImage frame = window->grabWindow();
                const bool ok = frame.save(QString::fromLocal8Bit(shotPath));
                qInfo("screenshot %s: %s", ok ? "saved" : "FAILED", shotPath.constData());
            });
        }
    }

    // Headless verification hook: KRAIT_SPIKE_AUTOQUIT closes after 2 s so
    // the T11 gate (log line above) can run unattended.
    if (qEnvironmentVariableIsSet("KRAIT_SPIKE_AUTOQUIT")) {
        QTimer::singleShot(3000, &app, &QCoreApplication::quit);
    }
    return app.exec();
}

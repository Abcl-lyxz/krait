// These two DEFINES must precede every include in this file, and that is what
// the position here buys — terminal_item.h reaches <windows.h> through
// conpty_backend.h, which does not guard it, so a define placed after them is
// dead and the min/max macros land in scope for the whole translation unit.
// Where clang-format then sorts the <windows.h> line below is irrelevant: the
// header is include-guarded and the macros are already set by the time anything
// pulls it in.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../net/vault/vault.h"
#include "gpu_policy.h"
#include "session/cli.h"
#include "session_model.h"
#include "settings/paths.h"
#include "settings/registry.h"
#include "settings_model.h"
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

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

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

    // The command line is parsed BEFORE the QGuiApplication, so --help and a
    // bad argument cost no window and no GPU device — and, more to the point,
    // exit with a status a script can read.
    namespace kses = krait::app::session;
    std::vector<std::string> rawArgs;
    rawArgs.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        rawArgs.emplace_back(argv[i]);
    }
    const kses::Launch launch = kses::parseCommandLine(rawArgs);
    if (launch.kind == kses::Launch::Kind::Message) {
        std::fputs(launch.message.c_str(), launch.error ? stderr : stdout);
        return launch.error ? 2 : 0;
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("Krait");
    qInfo("krait starting");
    // What was asked for, said out loud. A session manager that silently opens
    // something other than what the command line named is one nobody trusts
    // twice — and the connection itself is T45's wiring.
    switch (launch.kind) {
    case kses::Launch::Kind::Profile:
        qInfo("launch: profile '%s'", launch.profileName.c_str());
        break;
    case kses::Launch::Kind::Adhoc:
        qInfo("launch: ssh %s@%s:%lld", launch.profile.user.c_str(), launch.profile.host.c_str(),
              static_cast<long long>(launch.profile.port));
        break;
    default:
        qInfo("launch: default session");
        break;
    }

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

    // The DPAPI secret store (T38), declared HERE — before the QML engine —
    // rather than beside the wiring below. Destruction runs in reverse
    // declaration order, so a vault declared after the engine would be gone
    // while the backends that borrow it were still shutting down.
    //
    // One instance for the whole app: two Vaults over one file would each save
    // a copy of what the other did not know about.
    krait::net::Vault vault;
    const QString vaultPath = QDir(configDir.dir).filePath(QStringLiteral("secrets.vault"));
    if (!vault.load(vaultPath.toStdString())) {
        // Not fatal, and never a dialog: a broken vault means credentials get
        // asked for, which is worse than convenient but better than not
        // starting. The error text never contains a secret (rules/net.md).
        qWarning("vault: %s (%s)", vault.error().c_str(), qPrintable(vaultPath));
    }

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

    // BEFORE the engine builds the tree. TerminalView gets its geometry during
    // loadFromModule(), and geometry is what starts the first shell — so a
    // launch profile handed over afterwards would mean spawning PowerShell and
    // killing it again a few lines later, paying a process create and a wait on
    // the UI thread to show nothing.
    //
    // The store is not loaded yet either (SessionModel is constructed by QML),
    // so a named profile cannot be resolved here; only the ad-hoc case, which
    // carries its own profile, can be pre-placed. The named case is handled
    // after the tree exists and accepts the swap that costs.
    if (launch.kind == kses::Launch::Kind::Adhoc) {
        krait::app::TerminalItem::setLaunchProfile(launch.profile);
    }

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
            item->setVault(&vault);
        }
    }

    // The settings page reads the same one, for the same reason — and it is
    // where a hot reload has to land visibly, since a page showing stale values
    // is a page that will be edited on top of them.
    for (QObject* root : engine.rootObjects()) {
        for (krait::app::SettingsModel* model : root->findChildren<krait::app::SettingsModel*>()) {
            model->setRegistry(&registry);
        }
    }

    // T52: choosing a session in the palette opens it. The lookup lives here
    // and not in QML because rules/ui.md keeps decisions out of views, and
    // because a Profile is not something QML can carry.
    //
    // One terminal, so one target; T53 replaces this with "open a new tab" and
    // nothing else about the path changes.
    krait::app::TerminalItem* terminal = nullptr;
    for (QObject* root : engine.rootObjects()) {
        const QList<krait::app::TerminalItem*> items =
            root->findChildren<krait::app::TerminalItem*>();
        if (!items.isEmpty()) {
            terminal = items.first();
        }
        for (krait::app::SessionModel* model : root->findChildren<krait::app::SessionModel*>()) {
            QObject::connect(
                model, &krait::app::SessionModel::sessionRequested, model,
                [model, terminal](const QString& id) {
                    if (terminal == nullptr) {
                        return;
                    }
                    const std::optional<kses::Profile> profile = model->profileById(id);
                    if (!profile) {
                        // The palette offered it, so this means the
                        // store changed underneath — say so rather
                        // than opening a shell nobody asked for.
                        terminal->raiseError(
                            QCoreApplication::translate("main", "That session is no longer saved."),
                            id);
                        return;
                    }
                    terminal->openProfile(*profile);
                });
        }

        // What the command line asked for, now that there is something to open
        // it in. `Default` deliberately does nothing: the terminal starts a
        // local shell on its own.
        const QList<krait::app::SessionModel*> models =
            root->findChildren<krait::app::SessionModel*>();
        if (terminal != nullptr && !models.isEmpty()) {
            // Adhoc was already pre-placed above, before the item existed.
            if (launch.kind == kses::Launch::Kind::Profile) {
                const std::optional<kses::Profile> profile =
                    models.first()->profileByName(QString::fromStdString(launch.profileName));
                if (profile) {
                    terminal->openProfile(*profile);
                } else {
                    // Named something that is not there. Saying so beats
                    // opening a local shell and letting the user work out why
                    // their server is not answering.
                    terminal->raiseError(
                        QCoreApplication::translate("main", "No saved session is called that."),
                        QString::fromStdString(launch.profileName));
                }
            }
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

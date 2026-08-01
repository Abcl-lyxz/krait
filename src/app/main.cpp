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
#include "notifier.h"
#include "session/cli.h"
#include "session_model.h"
#include "settings/paths.h"
#include "settings/registry.h"
#include "settings_model.h"
#include "taskbar_progress.h"
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
#include <exception>
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

// A function-try-block, because main() calls into toml++, the standard library
// and Qt, none of which promise not to throw — and rules/cpp.md says exceptions
// do not cross module boundaries, which makes this the boundary.
//
// The alternative is what was here before: an exception escaping main, which on
// Windows is a silent abort with no message and an exit code nobody can read.
int main(int argc, char* argv[]) try {
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

    // The saved sessions, loaded ONCE here (T53). Before tabs, each SessionModel
    // opened its own copy of the file; with tabs there are several readers and
    // an importer that writes, and two copies of a session list disagree the
    // moment one of them saves.
    kses::ProfileStore store;
    const QString sessionsPath = ks::sessionsFilePath(configDir.dir);
    if (!store.load(sessionsPath.toStdString())) {
        // A hand-edited file is user input: a broken one degrades to "no
        // sessions" and says why, rather than refusing to start. SessionModel
        // turns store.error() into the banner.
        qWarning("sessions: %s (%s)", store.error().c_str(), qPrintable(sessionsPath));
    }
    qInfo("sessions: %s (%zu profiles)", qPrintable(sessionsPath), store.profiles().size());

    // Handed over BEFORE the engine builds anything, because QML constructs
    // these objects — including every terminal a new tab creates, long after
    // startup. The findChildren() sweep this replaces could only reach the ones
    // that existed when main() ran.
    krait::app::TerminalItem::setServices(&registry, &vault, &store);
    krait::app::SessionModel::setStore(&store);

    // T67, OSC 9;4. Declared here — before the engine — for the same reason the
    // vault is: it installs a native event filter on the application and holds
    // a COM interface, and destruction in reverse declaration order means both
    // are released while the tabs that report into it are already gone.
    krait::app::TaskbarProgress taskbar;
    krait::app::TerminalItem::setTaskbar(&taskbar);

    // T68, the desktop notification. Here for a third reason on top of those
    // two: it holds a notification-area icon keyed by the window's HWND, and
    // its destructor is the only thing that retracts it. Declared before the
    // engine means destroyed after it — late enough that every tab is gone,
    // and the cached HWND is what makes that work with no window left.
    krait::app::Notifier notifier;
    krait::app::TerminalItem::setNotifier(&notifier);

    // BEFORE the engine builds the tree. TerminalView gets its geometry during
    // loadFromModule(), and geometry is what starts the first shell — so a
    // launch profile handed over afterwards would mean spawning PowerShell and
    // killing it again a few lines later, paying a process create and a wait on
    // the UI thread to show nothing.
    //
    // Both kinds resolve here now that the store is loaded before the engine:
    // `krait ssh user@host` carries its own profile, and `krait prod` is a
    // lookup this can finally do.
    QString launchError;
    QString launchErrorDetail;
    if (launch.kind == kses::Launch::Kind::Adhoc) {
        krait::app::TerminalItem::setLaunchProfile(launch.profile);
    } else if (launch.kind == kses::Launch::Kind::Profile) {
        const kses::Profile* named = nullptr;
        for (const kses::Profile& profile : store.profiles()) {
            if (profile.name == launch.profileName) {
                named = &profile;
                break;
            }
        }
        if (named != nullptr) {
            krait::app::TerminalItem::setLaunchProfile(store.resolve(*named));
        } else {
            // Named something that is not there. The window still opens — with
            // a local shell and a banner saying why — because exiting would
            // leave a typo looking like a crash.
            launchError = QCoreApplication::translate("main", "No saved session is called that.");
            launchErrorDetail = QString::fromStdString(launch.profileName);
        }
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("benchMode", qEnvironmentVariableIsSet("KRAIT_BENCH"));
    // Read once by Main.qml on completion. A context property rather than a
    // call into the item: the banner belongs to whichever tab exists, and at
    // this point none do.
    engine.rootContext()->setContextProperty("launchError", launchError);
    engine.rootContext()->setContextProperty("launchErrorDetail", launchErrorDetail);
    // No uiSelfTest context property: the self-test is invoked by name below,
    // after the tree exists. A context property nothing reads is a global name
    // waiting to shadow a real one.
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Krait", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    // The settings page reads the same live registry, and it is where a hot
    // reload has to land visibly since a page showing stale values is a page
    // that will be edited on top of them. Still a findChildren() sweep because
    // there is exactly one settings page and it exists from startup — unlike a
    // terminal, which a new tab creates whenever it likes.
    for (QObject* root : engine.rootObjects()) {
        for (krait::app::SettingsModel* model : root->findChildren<krait::app::SettingsModel*>()) {
            model->setRegistry(&registry);
        }
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (window != nullptr) {
        // Before show(): the TaskbarButtonCreated message this waits for is
        // posted once the button exists, and Microsoft is explicit that it
        // "must be received by your application before it calls any
        // ITaskbarList3 method".
        taskbar.attach(window);
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

    // KRAIT_UI_SELFTEST: drive the tab and split actions and log what came out
    // (T53). Called from HERE rather than from a QML Timer, which reports
    // running == true and never triggers in a window that is never composited
    // — which is exactly how an unattended run works.
    if (qEnvironmentVariableIsSet("KRAIT_UI_SELFTEST") && !engine.rootObjects().isEmpty()) {
        QObject* uiRoot = engine.rootObjects().first();
        QTimer::singleShot(1500, uiRoot, [uiRoot] {
            if (!QMetaObject::invokeMethod(uiRoot, "runSelfTest")) {
                qCritical("selftest: Main.qml has no runSelfTest()");
                QCoreApplication::exit(4);
            }
        });
    }

    // Headless verification hook: KRAIT_SPIKE_AUTOQUIT closes after 2 s so
    // the T11 gate (log line above) can run unattended.
    if (qEnvironmentVariableIsSet("KRAIT_SPIKE_AUTOQUIT")) {
        QTimer::singleShot(3000, &app, &QCoreApplication::quit);
    }
    return app.exec();
} catch (const std::exception& error) {

    // stderr, not a banner: by definition there is no window to put one in.
    std::fprintf(stderr, "krait: fatal: %s\n", error.what());
    return 4;
} catch (...) {
    std::fprintf(stderr, "krait: fatal: unknown error\n");
    return 4;
}

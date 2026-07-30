#include "paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace krait::app::settings {
namespace {

constexpr const char* kMarker = "krait.portable";
constexpr const char* kConfigName = "krait.toml";

}  // namespace

QString portableMarkerName() {
    return QString::fromLatin1(kMarker);
}

QString configFilePath(const QString& dir) {
    return QDir(dir).filePath(QString::fromLatin1(kConfigName));
}

Resolution resolveConfigDir(const PathInputs& inputs,
                            const std::function<bool(const QString&)>& exists) {
    if (!inputs.envOverride.isEmpty()) {
        // Deliberately NOT checked for existence. If someone points
        // KRAIT_CONFIG_DIR at a directory that is not there yet, the honest
        // outcome is to create the config there — not to fall through to the
        // user profile and leave them wondering why their override did nothing.
        return {.dir = inputs.envOverride, .source = ConfigSource::EnvOverride};
    }
    if (!inputs.exeDir.isEmpty() &&
        exists(QDir(inputs.exeDir).filePath(QString::fromLatin1(kMarker)))) {
        return {.dir = inputs.exeDir, .source = ConfigSource::Portable};
    }
    return {.dir = inputs.userConfigDir, .source = ConfigSource::UserProfile};
}

PathInputs systemPathInputs() {
    return PathInputs{
        .envOverride = qEnvironmentVariable("KRAIT_CONFIG_DIR"),
        .exeDir = QCoreApplication::applicationDirPath(),
        // AppConfigLocation, not AppDataLocation: on Windows that is
        // %APPDATA%\Krait rather than %LOCALAPPDATA%, so the config roams with
        // a domain profile — which is what someone who logs into three machines
        // expects of their settings, and not of their cache.
        .userConfigDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation),
    };
}

QString describeSource(ConfigSource source) {
    switch (source) {
    case ConfigSource::EnvOverride:
        return QCoreApplication::translate("Settings", "KRAIT_CONFIG_DIR");
    case ConfigSource::Portable:
        return QCoreApplication::translate("Settings", "portable (beside the executable)");
    case ConfigSource::UserProfile:
        break;
    }
    return QCoreApplication::translate("Settings", "user profile");
}

}  // namespace krait::app::settings

#pragma once

#include <QString>

#include <cstdint>
#include <functional>

namespace krait::app::settings {

// Where the config came from. Reported rather than inferred, because "why is
// Krait not reading my file" is unanswerable without it — and the answer is
// almost always that a different one won.
enum class ConfigSource : std::uint8_t {
    EnvOverride,  // KRAIT_CONFIG_DIR
    Portable,     // beside the executable
    UserProfile,  // the per-user config location
};

struct Resolution {
    QString dir;
    ConfigSource source = ConfigSource::UserProfile;
};

// The inputs to the decision, injected so the ORDER is testable without a real
// filesystem, a real install, or an environment variable set in a test process
// that then leaks into the next one.
struct PathInputs {
    QString envOverride;  // KRAIT_CONFIG_DIR, empty when unset
    QString exeDir;
    QString userConfigDir;
};

// Resolves the config directory (plan T34). The order, highest first:
//
//   1. KRAIT_CONFIG_DIR. An explicit instruction beats every heuristic, and it
//      is what makes the other two debuggable in the field: point it somewhere
//      and the ambiguity is gone.
//   2. PORTABLE — a `krait.portable` marker beside the executable. Drop Krait
//      on a USB stick and the config travels with it, which is the habit PuTTY
//      users arrive with and the reason this task exists.
//   3. The per-user config location.
//
// A MARKER file decides portable mode, not the presence of krait.toml: an
// installed copy whose directory happens to be writable would otherwise turn
// portable the moment it saved once, silently orphaning the user's real config.
//
// `exists` is injected for the same reason as everything else here.
Resolution resolveConfigDir(const PathInputs& inputs,
                            const std::function<bool(const QString&)>& exists);

// The inputs as this machine actually reports them.
PathInputs systemPathInputs();

// The config file inside a resolved directory.
QString configFilePath(const QString& dir);

// The marker filename that turns portable mode on.
QString portableMarkerName();

// A one-line, translated explanation of where the config came from — for the
// startup log line and the settings page. Someone editing the wrong file should
// be able to find that out without reading the source.
QString describeSource(ConfigSource source);

}  // namespace krait::app::settings

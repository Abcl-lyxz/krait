#pragma once

#include "schema.h"

#include <QObject>
#include <QString>

#include <map>
#include <string>

class QFileSystemWatcher;
class QTimer;

namespace krait::app::settings {

// The live settings, backed by a TOML file (plan T30).
//
// rules/ui.md: "The settings registry is the only path for settings." Every
// read goes through here, so a subsystem cannot cache a value and miss a hot
// reload, and every write is validated against the schema, so a hand-edited
// file cannot put the app into a state its own UI could never produce.
//
// Errors are values, not exceptions: toml++ is built in its no-exceptions mode
// (rules/cpp.md — no exceptions across module boundaries), and a config file is
// user input, so a bad one has to degrade rather than abort.
class Registry : public QObject {
    Q_OBJECT

  public:
    explicit Registry(QObject* parent = nullptr);
    ~Registry() override;

    // Reads `path`, filling anything absent or invalid from the schema. Returns
    // false only when the file exists and cannot be PARSED — a missing file is
    // a first run, not a failure. Either way the registry is usable afterwards:
    // there is no state in which a setting has no value.
    bool load(const QString& path);

    // Writes every setting, including the ones still at their default. A config
    // listing only what the user changed is smaller but tells them nothing
    // about what they could change, and this file is meant to be read and
    // edited by hand.
    bool save() const;

    // Re-reads the file load() opened. This is what hot reload does, and it is
    // separate from the watcher so it can be tested without one: a test that
    // waits on a filesystem notification is a test that fails on a busy
    // machine.
    bool reload();

    // Starts or stops watching the file. Changes are debounced, because an
    // editor writing a file is several filesystem events — a save that lands as
    // truncate-then-write would otherwise reload an empty file first and reset
    // every setting to its default in front of the user.
    void setWatching(bool watching);

    const QString& path() const { return m_path; }

    // The file was written by a NEWER schema than this build understands.
    // Everything still loads, but saving is refused: overwriting would strip
    // whatever the newer version added, and a user who downgrades for an
    // afternoon should not lose their configuration.
    bool isFromFuture() const { return m_fileVersion > kSchemaVersion; }

    int fileVersion() const { return m_fileVersion; }

    bool boolean(std::string_view id) const;
    std::int64_t integer(std::string_view id) const;
    std::string text(std::string_view id) const;
    Value value(std::string_view id) const;

    // Rejects an unknown id, or a value the schema does not allow, and returns
    // false rather than clamping: silently storing something other than what
    // was asked for is how a settings UI ends up lying to the user.
    bool set(std::string_view id, const Value& value);

  signals:
    // One id. Subsystems connect to the settings they care about rather than
    // rebuilding the world on every keystroke in the settings page.
    void changed(const QString& id);

    // The file was re-read. Emitted once after a reload and after every
    // individual changed(), so a listener that needs a consistent view (the
    // renderer, for one) can rebuild here instead of part-way through.
    void reloaded();

  private:
    void applyDefaults();

    QString m_path;
    std::map<std::string, Value, std::less<>> m_values;
    int m_fileVersion = kSchemaVersion;
    QFileSystemWatcher* m_watcher = nullptr;  // owned by this (QObject parent)
    QTimer* m_debounce = nullptr;             // owned by this
};

}  // namespace krait::app::settings

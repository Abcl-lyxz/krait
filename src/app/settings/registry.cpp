#include "registry.h"

#include <QFile>
#include <QFileSystemWatcher>
#include <QSaveFile>
#include <QTimer>

#include <toml++/toml.hpp>

#include <optional>
#include <sstream>
#include <utility>

namespace krait::app::settings {
namespace {

constexpr int kDebounceMs = 120;

// Splits "font.size" into "font" and "size". The dots ARE the table path, so
// there is no mapping table to keep in step with the schema.
std::pair<std::string_view, std::string_view> splitId(std::string_view id) {
    const auto dot = id.rfind('.');
    if (dot == std::string_view::npos) {
        return {{}, id};
    }
    return {id.substr(0, dot), id.substr(dot + 1)};
}

// Reads one setting out of a parsed document. Returns nullopt when the key is
// absent OR present with the wrong type — both mean "fall back to the default",
// because a config file is user input and a typo must not take the app down.
std::optional<Value> readValue(const toml::table& doc, const Def& def) {
    const auto [tableName, key] = splitId(def.id);
    const toml::node* node = nullptr;
    if (tableName.empty()) {
        node = doc.get(key);
    } else if (const toml::table* sub = doc[tableName].as_table()) {
        node = sub->get(key);
    }
    if (node == nullptr) {
        return std::nullopt;
    }
    // The node's TYPE is checked before its value, not left to value<T>():
    // toml++ will happily hand back `true` for `ligatures = 3`, and a config
    // that means one thing and loads as another is worse than one that fails.
    switch (def.type) {
    case Type::Bool:
        if (node->is_boolean()) {
            return Value{node->value_or(false)};
        }
        return std::nullopt;
    case Type::Int:
        if (node->is_integer()) {
            return Value{node->value_or(std::int64_t{0})};
        }
        return std::nullopt;
    case Type::String:
        if (node->is_string()) {
            return Value{node->value_or(std::string{})};
        }
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

Registry::Registry(QObject* parent) : QObject(parent) {
    applyDefaults();
}

Registry::~Registry() = default;

void Registry::applyDefaults() {
    for (const Def& def : definitions()) {
        m_values[std::string(def.id)] = defaultValue(def);
    }
}

bool Registry::load(const QString& path) {
    m_path = path;
    return reload();
}

bool Registry::reload() {
    // Start from the defaults every time. Without this, DELETING a key from the
    // file would leave the old value in memory — the setting would look
    // unchanged until the next restart, which is the most confusing possible
    // outcome for someone editing the file to try something out.
    const std::map<std::string, Value, std::less<>> previous = m_values;
    applyDefaults();
    m_fileVersion = kSchemaVersion;

    QFile file(m_path);
    bool parsed = true;
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = file.readAll();
        // toml++ in its no-exceptions mode returns a result rather than throwing.
        const toml::parse_result result =
            toml::parse(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                        m_path.toStdString());
        if (result) {
            const toml::table& doc = result.table();
            if (const auto version = doc["schema_version"].value<std::int64_t>()) {
                m_fileVersion = static_cast<int>(*version);
            }
            // No migrations yet: kSchemaVersion is 1 and nothing has been
            // renamed. A file with no schema_version is treated as current and
            // stamped on the next save, which is the whole v0 -> v1 story. When
            // the first rename lands it goes HERE, keyed on m_fileVersion, and
            // kSchemaVersion goes to 2.
            for (const Def& def : definitions()) {
                const auto value = readValue(doc, def);
                if (value.has_value() && validate(def, *value)) {
                    m_values[std::string(def.id)] = *value;
                }
                // An out-of-range or wrong-typed value keeps the DEFAULT and is
                // not clamped: clamping 5000 to 200 silently gives the user a
                // font size they did not ask for and cannot tell they did not
                // get.
            }
        } else {
            parsed = false;
            qWarning("settings: %s could not be parsed (%s); using defaults", qPrintable(m_path),
                     std::string(result.error().description()).c_str());
        }
    }

    for (const auto& [id, value] : m_values) {
        const auto it = previous.find(id);
        if (it == previous.end() || it->second != value) {
            emit changed(QString::fromStdString(id));
        }
    }
    emit reloaded();
    return parsed;
}

bool Registry::save() const {
    if (m_path.isEmpty()) {
        return false;
    }
    if (isFromFuture()) {
        qWarning("settings: %s was written by schema v%d and this build is v%d; not overwriting",
                 qPrintable(m_path), m_fileVersion, kSchemaVersion);
        return false;
    }

    toml::table doc;
    doc.insert("schema_version", static_cast<std::int64_t>(kSchemaVersion));
    for (const Def& def : definitions()) {
        const auto it = m_values.find(def.id);
        if (it == m_values.end()) {
            continue;
        }
        const auto [tableName, key] = splitId(def.id);
        toml::table* target = &doc;
        if (!tableName.empty()) {
            auto existing = doc.find(tableName);
            if (existing == doc.end()) {
                existing = doc.emplace(tableName, toml::table{}).first;
            }
            target = existing->second.as_table();
        }
        if (target == nullptr) {
            continue;
        }
        std::visit(
            [target, k = std::string(key)](const auto& held) { target->insert_or_assign(k, held); },
            it->second);
    }

    std::ostringstream out;
    out << doc << '\n';
    const std::string text = out.str();

    // QSaveFile, not QFile: it writes to a temporary and renames, so a crash or
    // a full disk mid-write leaves the OLD config intact rather than a
    // half-written one the app cannot parse on the next launch.
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning("settings: cannot write %s", qPrintable(m_path));
        return false;
    }
    file.write(text.data(), static_cast<qint64>(text.size()));
    return file.commit();
}

void Registry::setWatching(bool watching) {
    if (!watching) {
        // Both, not just the watcher. They are QObject children of `this`, so a
        // half-teardown leaves the timer alive with its lambda still connected;
        // switching watching back on would then create a SECOND timer and
        // reload twice for every change.
        delete m_watcher;
        m_watcher = nullptr;
        delete m_debounce;
        m_debounce = nullptr;
        return;
    }
    if (m_watcher != nullptr || m_path.isEmpty()) {
        return;
    }
    m_watcher = new QFileSystemWatcher(this);  // owned by this
    m_debounce = new QTimer(this);             // owned by this
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, [this] {
        // Re-add the path: an editor that saves by RENAME replaces the inode,
        // and the watcher stops watching a file that was replaced rather than
        // written in place. Without this, hot reload works exactly once.
        if (m_watcher != nullptr && !m_watcher->files().contains(m_path)) {
            m_watcher->addPath(m_path);
        }
        reload();
    });
    connect(m_watcher, &QFileSystemWatcher::fileChanged, m_debounce, qOverload<>(&QTimer::start));
    m_watcher->addPath(m_path);
}

Value Registry::value(std::string_view id) const {
    const auto it = m_values.find(id);
    if (it != m_values.end()) {
        return it->second;
    }
    // An unknown id is a caller bug. Returning the schema default would hide it
    // behind plausible behaviour, so this is deliberately loud and empty.
    qWarning("settings: unknown id '%s'", std::string(id).c_str());
    return Value{std::string{}};
}

bool Registry::boolean(std::string_view id) const {
    const Value held = value(id);
    const auto* result = std::get_if<bool>(&held);
    return result != nullptr && *result;
}

std::int64_t Registry::integer(std::string_view id) const {
    const Value held = value(id);
    const auto* result = std::get_if<std::int64_t>(&held);
    return result != nullptr ? *result : 0;
}

std::string Registry::text(std::string_view id) const {
    const Value held = value(id);
    const auto* result = std::get_if<std::string>(&held);
    return result != nullptr ? *result : std::string{};
}

bool Registry::set(std::string_view id, const Value& newValue) {
    const Def* def = find(id);
    if (def == nullptr) {
        qWarning("settings: cannot set unknown id '%s'", std::string(id).c_str());
        return false;
    }
    if (!validate(*def, newValue)) {
        return false;
    }
    const auto it = m_values.find(id);
    if (it != m_values.end() && it->second == newValue) {
        return true;  // no change, no signal: listeners rebuild on every one
    }
    m_values[std::string(id)] = newValue;
    emit changed(QString::fromStdString(std::string(id)));
    return true;
}

}  // namespace krait::app::settings

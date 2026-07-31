#include "session_model.h"

#include "session/actions.h"
#include "session/putty_import.h"
#include "settings/paths.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QVariantMap>

namespace krait::app {

namespace {

// sessions.toml sits beside krait.toml, so portable mode and KRAIT_CONFIG_DIR
// move both together — a config directory that holds the settings but not the
// sessions would be a surprise nobody wants twice.
QString sessionsPath() {
    namespace ks = settings;
    const ks::Resolution dir = ks::resolveConfigDir(
        ks::systemPathInputs(), [](const QString& path) { return QFile::exists(path); });
    QDir().mkpath(dir.dir);
    return dir.dir + "/sessions.toml";
}

// Kept in the registry's order for readability; the test compares them as sets,
// so a reorder is fine and a rename is not.
constexpr const char* const kActionLabels[] = {
    QT_TR_NOOP("New session"),
    QT_TR_NOOP("Close session"),
    QT_TR_NOOP("Reconnect session"),
    QT_TR_NOOP("Command palette"),
    QT_TR_NOOP("Open a saved session"),
    QT_TR_NOOP("Manage sessions"),
    QT_TR_NOOP("Import sessions from PuTTY"),
    QT_TR_NOOP("Settings"),
    QT_TR_NOOP("Reload settings from disk"),
    QT_TR_NOOP("Copy"),
    QT_TR_NOOP("Paste"),
    QT_TR_NOOP("Search scrollback"),
    QT_TR_NOOP("Clear scrollback"),
    QT_TR_NOOP("About Krait"),
};

}  // namespace

std::span<const char* const> translatableActionLabels() {
    return kActionLabels;
}

SessionModel::SessionModel(QObject* parent) : QObject(parent) {
    load();
}

void SessionModel::load() {
    const QString path = sessionsPath();
    if (!m_store.load(path.toStdString())) {
        // A banner, not a dialog, and the app keeps running with no sessions
        // rather than refusing to start (rules/ui.md).
        emit loadError(
            tr("Could not read %1: %2").arg(path, QString::fromStdString(m_store.error())));
    }
    refresh();
}

void SessionModel::setQuery(const QString& query) {
    if (m_query == query) {
        return;
    }
    m_query = query;
    emit queryChanged();
    refresh();
}

void SessionModel::refresh() {
    const std::string query = m_query.toStdString();

    m_entries.clear();
    for (const session::PaletteEntry& entry : session::rankPalette(query, m_store)) {
        QVariantMap row;
        row["kind"] = entry.kind == session::PaletteEntry::Kind::Session ? "session" : "action";
        row["id"] = QString::fromStdString(entry.id);
        // Actions carry English labels in the registry so it can stay Qt-free;
        // translation happens HERE, at the point of display.
        row["label"] = entry.kind == session::PaletteEntry::Kind::Action
                           ? tr(entry.label.c_str())
                           : QString::fromStdString(entry.label);
        row["detail"] = QString::fromStdString(entry.detail);
        m_entries.append(row);
    }
    emit entriesChanged();

    m_tree.clear();
    for (const session::TreeRow& node : session::buildTree(m_store)) {
        QVariantMap row;
        row["isFolder"] = node.isFolder;
        row["depth"] = node.depth;
        row["label"] = QString::fromStdString(node.label);
        row["id"] = QString::fromStdString(node.id);
        m_tree.append(row);
    }
    emit treeChanged();
}

void SessionModel::activate(int index) {
    if (index < 0 || index >= static_cast<int>(m_entries.size())) {
        return;
    }
    const QVariantMap row = m_entries.at(index).toMap();
    if (row.value("kind").toString() == QStringLiteral("session")) {
        emit sessionRequested(row.value("id").toString());
    } else {
        emit actionRequested(row.value("id").toString());
    }
}

std::optional<session::Profile> SessionModel::profileById(const QString& id) const {
    const session::Profile* raw = m_store.find(id.toStdString());
    if (raw == nullptr) {
        return std::nullopt;
    }
    // resolve(), not the stored profile: the store holds only the keys each
    // profile owns, so a session that inherits its user from [folders."prod"]
    // would otherwise reach the backend with an empty user and fail with a
    // message about a value the user never wrote.
    return m_store.resolve(*raw);
}

std::optional<session::Profile> SessionModel::profileByName(const QString& name) const {
    const std::string wanted = name.toStdString();
    for (const session::Profile& profile : m_store.profiles()) {
        if (profile.name == wanted) {
            return m_store.resolve(profile);
        }
    }
    return std::nullopt;
}

QString SessionModel::importFromPutty() {
    const session::PuttyImport imported = session::importFromPuttyRegistry();
    if (!imported.error.empty()) {
        return tr("No PuTTY sessions were found.");
    }

    int added = 0;
    for (const session::Profile& profile : imported.profiles) {
        // add() de-duplicates the id, so importing twice makes copies rather
        // than silently overwriting a profile the user has since edited.
        m_store.add(profile);
        ++added;
    }
    m_store.save();
    refresh();

    if (imported.skipped.empty()) {
        return tr("Imported %1 session(s) from PuTTY.").arg(added);
    }
    // Naming what was left behind, not merely counting it: "3 skipped" sends
    // someone hunting through PuTTY to work out which three.
    QStringList names;
    names.reserve(static_cast<qsizetype>(imported.skipped.size()));
    for (const std::string& name : imported.skipped) {
        names.append(QString::fromStdString(name));
    }
    return tr("Imported %1 session(s). Left behind, because Krait does not speak their protocol "
              "yet: %2.")
        .arg(added)
        .arg(names.join(QStringLiteral(", ")));
}

}  // namespace krait::app

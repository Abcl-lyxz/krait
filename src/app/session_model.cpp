#include "session_model.h"

#include "mremoteng_import.h"
#include "session/actions.h"
#include "session/putty_import.h"
#include "session/ssh_config_import.h"
#include "settings/paths.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QVariantMap>

namespace krait::app {

namespace {

// Moved to settings::sessionsFilePath — main() resolves the directory once and
// loads the store, rather than every view-model re-deriving the path and
// opening its own copy of the file.
// Kept in the registry's order for readability; the test compares them as sets,
// so a reorder is fine and a rename is not.
constexpr const char* const kActionLabels[] = {
    QT_TR_NOOP("New session"),
    QT_TR_NOOP("Close session"),
    QT_TR_NOOP("Reconnect session"),
    QT_TR_NOOP("Next tab"),
    QT_TR_NOOP("Previous tab"),
    QT_TR_NOOP("Split right"),
    QT_TR_NOOP("Split down"),
    QT_TR_NOOP("Close pane"),
    QT_TR_NOOP("Toggle hexdump"),
    QT_TR_NOOP("Start or stop logging this session"),
    QT_TR_NOOP("Copy mode (vim keys)"),
    QT_TR_NOOP("Broadcast to several sessions"),
    QT_TR_NOOP("Show port forwards"),
    QT_TR_NOOP("Show the file transfer panel"),
    QT_TR_NOOP("Show the snippet bar"),
    QT_TR_NOOP("Jump to the previous prompt"),
    QT_TR_NOOP("Jump to the next prompt"),
    QT_TR_NOOP("Command palette"),
    QT_TR_NOOP("Open a saved session"),
    QT_TR_NOOP("Manage sessions"),
    QT_TR_NOOP("Import sessions from PuTTY"),
    QT_TR_NOOP("Import hosts from OpenSSH config"),
    QT_TR_NOOP("Import connections from mRemoteNG"),
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

namespace {
session::ProfileStore* g_store = nullptr;
}  // namespace

void SessionModel::setStore(session::ProfileStore* store) {
    g_store = store;
}

SessionModel::SessionModel(QObject* parent) : QObject(parent), m_store(g_store) {
    load();
}

void SessionModel::load() {
    if (m_store == nullptr) {
        return;
    }
    // The FILE was read by main(); a broken one is already reported there. This
    // only rebuilds the rows, which is what the importer needs after it adds
    // some — and what a second SessionModel needs in order to see them.
    if (!m_store->error().empty()) {
        // A banner, not a dialog, and the app keeps running with no sessions
        // rather than refusing to start (rules/ui.md).
        emit loadError(tr("Could not read %1: %2")
                           .arg(QString::fromStdString(m_store->path()),
                                QString::fromStdString(m_store->error())));
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
    // The guard lives HERE, not in load(): setQuery() and importFromPutty()
    // reach refresh() without going through load(), and the unit binary
    // compiles this file directly (tests/unit/CMakeLists.txt) without ever
    // calling setStore(). A guard only on load() advertises the class as
    // null-safe while leaving the paths that actually run unprotected.
    if (m_store == nullptr) {
        return;
    }
    const std::string query = m_query.toStdString();

    m_entries.clear();
    for (const session::PaletteEntry& entry : session::rankPalette(query, *m_store)) {
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
    for (const session::TreeRow& node : session::buildTree(*m_store)) {
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

QString SessionModel::importFromPutty() {
    if (m_store == nullptr) {
        return {};
    }
    const session::PuttyImport imported = session::importFromPuttyRegistry();
    if (!imported.error.empty()) {
        return tr("No PuTTY sessions were found.");
    }

    int added = 0;
    for (const session::Profile& profile : imported.profiles) {
        // add() de-duplicates the id, so importing twice makes copies rather
        // than silently overwriting a profile the user has since edited.
        m_store->add(profile);
        ++added;
    }
    m_store->save();
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

namespace {

// A config nobody would write by hand. Both of these files are the user's own,
// but they are still a size this thread has to survive: the import runs on the
// GUI thread, and readAll() on a file with no ceiling is how a mistyped path at
// a multi-gigabyte file freezes the window instead of raising a banner.
constexpr qint64 kMaxImportBytes = qint64{8} * 1024 * 1024;

// Reads a whole file as UTF-8, or nothing. Both T62 importers parse TEXT and
// touch no filesystem themselves — that split is what lets their mapping be
// tested without anybody's real config.
bool readTextFile(const QString& path, QString* text) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    if (file.size() > kMaxImportBytes) {
        return false;
    }
    *text = QString::fromUtf8(file.readAll());
    return true;
}

}  // namespace

QString SessionModel::importFromSshConfig() {
    if (m_store == nullptr) {
        return {};
    }
    const QString path = QString::fromStdString(session::defaultSshConfigPath());
    QString text;
    if (path.isEmpty() || !readTextFile(path, &text)) {
        return tr("No OpenSSH config found at %1.").arg(path);
    }

    const session::SshConfigImport imported = session::importFromSshConfig(text.toStdString());
    int added = 0;
    for (const session::Profile& profile : imported.profiles) {
        m_store->add(profile);
        ++added;
    }
    m_store->save();
    refresh();

    QStringList notes;
    if (!imported.skipped.empty()) {
        QStringList names;
        names.reserve(static_cast<qsizetype>(imported.skipped.size()));
        for (const std::string& name : imported.skipped) {
            names.append(QString::fromStdString(name));
        }
        notes.append(tr("Not imported: %1.").arg(names.join(QStringLiteral(", "))));
    }
    if (!imported.includes.empty()) {
        // The case that would otherwise look like a successful import of an
        // almost empty file: a config whose hosts all live in an included
        // directory.
        QStringList names;
        names.reserve(static_cast<qsizetype>(imported.includes.size()));
        for (const std::string& name : imported.includes) {
            names.append(QString::fromStdString(name));
        }
        notes.append(tr("Included files were NOT followed, so anything they define is missing: %1.")
                         .arg(names.join(QStringLiteral(", "))));
    }
    if (notes.isEmpty()) {
        return tr("Imported %1 host(s) from %2.").arg(added).arg(path);
    }
    return tr("Imported %1 host(s) from %2. %3")
        .arg(added)
        .arg(path, notes.join(QStringLiteral(" ")));
}

QString SessionModel::importFromMremoteng() {
    if (m_store == nullptr) {
        return {};
    }
    const QString appData = qEnvironmentVariable("APPDATA");
    const QString path =
        appData.isEmpty() ? QString() : appData + QStringLiteral("/mRemoteNG/confCons.xml");
    QString text;
    if (path.isEmpty() || !readTextFile(path, &text)) {
        return tr("No mRemoteNG connection file found at %1.").arg(path);
    }

    // Qualified, because unqualified lookup from inside this member finds the
    // member itself and calls it with an argument it does not take.
    const MremotengImport imported = krait::app::importFromMremoteng(text);
    if (!imported.error.isEmpty()) {
        return imported.error;
    }

    int added = 0;
    for (const session::Profile& profile : imported.profiles) {
        m_store->add(profile);
        ++added;
    }
    m_store->save();
    refresh();

    // Said every time, not only when something was skipped: someone who just
    // imported a connection manager's worth of hosts needs to know the
    // passwords did not come with them BEFORE they try to connect to one.
    const QString passwords = tr("Passwords were not imported — Krait asks for them once and "
                                 "keeps them in the Windows vault.");
    if (imported.skipped.isEmpty()) {
        return tr("Imported %1 connection(s) from mRemoteNG. %2").arg(added).arg(passwords);
    }
    return tr("Imported %1 connection(s) from mRemoteNG. Left behind: %2. %3")
        .arg(added)
        .arg(imported.skipped.join(QStringLiteral(", ")), passwords);
}

}  // namespace krait::app

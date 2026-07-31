#pragma once

#include "session/palette.h"
#include "session/profile.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <span>

namespace krait::app {

// Every action label, repeated as a literal for lupdate.
//
// refresh() translates with tr(entry.label.c_str()), and lupdate cannot see
// through a runtime string — exactly the trap that cost M1 eight strings in
// error_banner.h, where a translate() lambda hid every literal behind one level
// of indirection and did it silently. The array below is what lupdate actually
// extracts; the runtime tr() then finds those entries in the catalogue.
//
// A duplicated list rots, so it is not left to discipline: a test asserts this
// array and allActions() carry exactly the same labels.
std::span<const char* const> translatableActionLabels();

// The view-model behind the command palette and the session tree.
//
// rules/ui.md: "QML is views only... View-models are C++ QObjects with typed
// properties; QML binds." Every decision — ranking, tree shape, what an entry
// means when activated — lives in krait-session as a pure function; this class
// only turns those answers into something QML can repeat.
class SessionModel : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(QVariantList tree READ tree NOTIFY treeChanged)
    Q_PROPERTY(int count READ count NOTIFY entriesChanged)

  public:
    explicit SessionModel(QObject* parent = nullptr);  // owned by parent

    // The session list, owned by main() and shared with every terminal (T53).
    // Static for the same reason TerminalItem::setServices is: QML constructs
    // this object, so there is no moment between construction and first use in
    // which to hand it anything.
    //
    // One store, not one per view-model: a tab opened from the palette and the
    // palette itself must agree about what is saved, and an importer writing
    // through one copy while another holds stale rows is a bug that only shows
    // up after a save.
    static void setStore(session::ProfileStore* store);

    // Re-reads the model from the store. The FILE is loaded by main(); this
    // only rebuilds what the views show, and is what the importer calls after
    // it has added rows.
    Q_INVOKABLE void load();

    const QString& query() const { return m_query; }

    void setQuery(const QString& query);

    const QVariantList& entries() const { return m_entries; }

    const QVariantList& tree() const { return m_tree; }

    int count() const { return static_cast<int>(m_entries.size()); }

    // Fires actionRequested or sessionRequested depending on what the row is.
    // QML never has to know the difference, which keeps the branch here where
    // it can be read.
    Q_INVOKABLE void activate(int index);

    // Imports from the PuTTY registry and merges into the store, returning a
    // human-readable summary for the banner — including how many sessions were
    // left behind and why.
    Q_INVOKABLE QString importFromPutty();

    // T52 had profileById()/profileByName() here for main()'s wiring. T53
    // deleted both callers: a session now opens in a NEW TAB, which QML
    // decides, so the lookup moved to TerminalItem::openProfileById; and the
    // command line is resolved against the store directly in main(), before any
    // SessionModel exists. Keeping them would have left two unguarded
    // dereferences behind a comment naming a caller that no longer existed.

  signals:
    void queryChanged();
    void entriesChanged();
    void treeChanged();
    void actionRequested(const QString& actionId);
    void sessionRequested(const QString& profileId);
    // Non-fatal: shown as a per-tab banner, never a dialog.
    void loadError(const QString& message);

  private:
    void refresh();

    session::ProfileStore* m_store = nullptr;  // borrowed; owned by main()
    QString m_query;
    QVariantList m_entries;
    QVariantList m_tree;
};

}  // namespace krait::app

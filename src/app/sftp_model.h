#pragma once

#include <QHash>
#include <QObject>
#include <QString>
// Included, not forward-declared: uploadUrls is Q_INVOKABLE, and moc's
// generated metatype table needs QUrl complete to decide whether QList<QUrl>
// is streamable. A forward declaration fails deep inside qdatastream.h with an
// error that names neither this file nor the reason.
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <optional>

namespace krait::net {
class SshBackend;
}  // namespace krait::net

namespace krait::app {

// True when `name` is safe to use as ONE path component on either side.
//
// Remote input is hostile (rules/net.md). Sftp::listDir already drops names
// carrying separators or control characters, but the panel COMPOSES a local
// destination path out of those names, and a check that lives only in the
// producer is a check that disappears the day a second producer appears. This
// is the one at the point of use, and it is what stops a server answering a
// readdir with "..\\..\\Startup\\evil.lnk" from choosing where a download
// lands.
//
// It runs on names going the other way too: a LOCAL file called "a/b" cannot
// exist on Windows, but a name arriving from a dropped URL has been through
// somebody else's code first.
bool isSafeLeafName(const QString& name);

// Remote paths are POSIX whatever the client runs on — SFTP has one separator
// and it is not the platform's. QDir would happily hand back a backslash here.
QString remoteJoin(const QString& base, const QString& name);

// The directory containing `path`. The root is its own parent, which is what
// makes an "up" button that is always safe to press.
QString remoteParent(const QString& path);

// Which reply belongs to which question.
//
// SshBackend serves requests in order on one worker, so a deque would do — but
// the ids also outlive a backend swap: a queued cross-thread emission from the
// session the user just left is already posted to this object and disconnect()
// cannot reach it (the same hazard TerminalItem::adoptBackend documents). Ids
// are therefore never reused, so a stale reply finds nothing and is dropped
// rather than being read as an answer to the current question.
class SftpRequests {
  public:
    enum class Kind { Resolve, List, Download, Upload };

    struct Request {
        Kind kind = Kind::List;
        // What the request is about, for the progress line: the path being
        // listed, or the file name being moved.
        QString subject;
    };

    // Returns the id to hand to the backend.
    quint64 add(Kind kind, QString subject);

    // Removes the request `id` names. Empty when there is no such request —
    // which is what a reply from a previous session looks like.
    std::optional<Request> take(quint64 id);

    const Request* find(quint64 id) const;

    bool empty() const { return m_open.isEmpty(); }

    // Forgets everything in flight. The ids keep counting up; see above.
    void clear() { m_open.clear(); }

  private:
    QHash<quint64, Request> m_open;
    quint64 m_next = 1;
};

// The dual-pane file panel's view-model (plan T65).
//
// rules/ui.md: QML is views only. Every decision — where a path composes to,
// whether a name may be used, which pane a reply belongs to — is here; the QML
// binds and repeats.
//
// One of these per TerminalItem, which owns it and points it at the SSH backend
// when there is one. `available` is false for every other backend, and every
// method is a no-op then: a local shell tab must not offer file transfer, and
// must not crash when something asks anyway.
class SftpModel : public QObject {
    Q_OBJECT
    // Anonymous, not QML_ELEMENT: QML must be able to dereference the type
    // through TerminalItem::files, but a panel instantiated from QML would have
    // no backend and no way to be given one.
    QML_ANONYMOUS

    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QString localPath READ localPath NOTIFY localChanged)
    Q_PROPERTY(QVariantList localEntries READ localEntries NOTIFY localChanged)
    Q_PROPERTY(QString remotePath READ remotePath NOTIFY remoteChanged)
    Q_PROPERTY(QVariantList remoteEntries READ remoteEntries NOTIFY remoteChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY progressChanged)
    // What is happening, already phrased for a human. Empty when nothing is.
    Q_PROPERTY(QString activity READ activity NOTIFY progressChanged)
    // 0..1, or -1 when the server did not say how big the file is — a bar that
    // invents a length is a bar that lies about how long the wait is.
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)

  public:
    explicit SftpModel(QObject* parent = nullptr);  // owned by parent

    // Points this model at `backend`, or at nothing. Called by TerminalItem
    // every time the terminal's session changes, so the panel of a tab that has
    // been re-pointed at a local shell stops offering transfers.
    void attach(net::SshBackend* backend);

    bool available() const { return m_backend != nullptr; }

    const QString& localPath() const { return m_localPath; }

    const QVariantList& localEntries() const { return m_localEntries; }

    const QString& remotePath() const { return m_remotePath; }

    const QVariantList& remoteEntries() const { return m_remoteEntries; }

    bool busy() const { return !m_open.empty(); }

    const QString& activity() const { return m_activity; }

    qreal progress() const;

    // Fills both panes. Called when the panel is first shown rather than on
    // attach: SshBackend opens the SFTP channel lazily because most sessions
    // never transfer a file, and resolving on connect would throw that away.
    Q_INVOKABLE void start();

    Q_INVOKABLE void refreshLocal();
    Q_INVOKABLE void refreshRemote();

    // Enter `name`, which must be a directory in the pane's current listing.
    Q_INVOKABLE void enterLocal(const QString& name);
    Q_INVOKABLE void enterRemote(const QString& name);

    Q_INVOKABLE void leaveLocal();
    Q_INVOKABLE void leaveRemote();

    // `name` is a leaf in the opposite pane's current directory.
    Q_INVOKABLE void download(const QString& name);
    Q_INVOKABLE void upload(const QString& name);

    // Files dropped from Explorer. `urls` carry absolute local paths, so only
    // the leaf — which becomes part of a REMOTE path — is composed here.
    Q_INVOKABLE void uploadUrls(const QList<QUrl>& urls);

    // Stops the transfer in flight and drops the queue behind it.
    Q_INVOKABLE void cancel();

  signals:
    void availableChanged();
    void localChanged();
    void remoteChanged();
    void progressChanged();
    // Per-tab banner, never a dialog (rules/ui.md). `detail` is the backend's
    // own message when there is one.
    void errorRaised(const QString& message, const QString& detail);

  private:
    void handleListed(quint64 requestId, const QString& path, const QVariantList& entries);
    void handleResolved(quint64 requestId, const QString& path);
    void handleProgress(quint64 requestId, qulonglong done, qulonglong total);
    void handleFinished(quint64 requestId, bool ok, bool cancelled, const QString& message);
    // Refuses `name` with a banner and returns false. One place, because every
    // caller that composes a path needs the same answer to the same question.
    bool checkName(const QString& name);
    // Looks `name` up in `entries`; empty when the pane has no such row. The
    // panel only ever transfers what it is showing, so a click on a listing
    // that has moved on cannot send a path the user never saw.
    static QVariantMap findEntry(const QVariantList& entries, const QString& name);
    void clearProgress();

    net::SshBackend* m_backend = nullptr;  // borrowed; owned by TerminalItem
    SftpRequests m_open;
    QString m_localPath;
    QString m_remotePath;
    QVariantList m_localEntries;
    QVariantList m_remoteEntries;
    QString m_activity;
    qulonglong m_done = 0;
    qulonglong m_total = 0;
};

}  // namespace krait::app

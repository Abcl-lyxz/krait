#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
// Included, not forward-declared: uploadUrls is Q_INVOKABLE, and moc's
// generated metatype table needs QUrl complete to decide whether QList<QUrl>
// is streamable. A forward declaration fails deep inside qdatastream.h with an
// error that names neither this file nor the reason.
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <cstddef>
#include <optional>

class QFileSystemWatcher;
class QTimer;

namespace krait::net {
class SshBackend;
}  // namespace krait::net

namespace krait::app::settings {
class Registry;
}  // namespace krait::app::settings

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

// True when opening `name` with whatever this computer associates with it would
// RUN it rather than show it.
//
// The editor round-trip's default is the OS association, which is the right
// answer for a .conf or a .py and the wrong one for a .exe: "Edit" on a file the
// SERVER named and the SERVER filled would then be a button that executes it,
// on this machine, from a temp file nobody marked as coming from the internet.
// isSafeLeafName stops a name from steering a path; this stops it from choosing
// a program.
//
// A denylist is the wrong shape in general; here it is the only shape
// available, because the set worth ALLOWING is every extension a text editor
// might be registered for — which is all of them.
bool isShellExecutableName(const QString& name);

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
    // T73 adds four. They are separate kinds rather than a flag on Download
    // because each one ends differently: a Probe that fails is the ANSWER (the
    // rc file is not there) and must not raise a banner, and an EditUpload that
    // succeeds has to leave the watcher armed while a Download does not.
    enum class Kind { Resolve, List, Download, Upload, Probe, WriteRc, EditDownload, EditUpload };

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

    // T73, the shell-integration installer. One string rather than three bools
    // because the surface is a small state machine and QML switching on a
    // stage cannot render two of them at once by accident:
    //   ""          nothing is being proposed
    //   "probing"   looking for rc files on the server
    //   "choosing"  detection was ambiguous; installChoices holds the options
    //   "proposed"  installPath and installPreview are the whole of what will
    //               happen, and nothing has been written yet
    //   "writing"   the user said yes and the upload is in flight
    //   "done"      it is written; the surface says so and offers Close
    Q_PROPERTY(QString installStage READ installStage NOTIFY installChanged)
    // Already phrased for a human, and it always names the HOST: this writes to
    // someone else's machine, and a confirmation that does not say whose is not
    // a confirmation.
    Q_PROPERTY(QString installTitle READ installTitle NOTIFY installChanged)
    // The absolute remote path that will be rewritten. Empty while choosing.
    Q_PROPERTY(QString installPath READ installPath NOTIFY installChanged)
    // EXACTLY the text that will be placed between the markers, so "what will
    // be written" is a thing the user can read rather than a promise.
    Q_PROPERTY(QString installPreview READ installPreview NOTIFY installChanged)
    // The rc paths to pick between, when detection could not. Empty otherwise.
    Q_PROPERTY(QStringList installChoices READ installChoices NOTIFY installChanged)

    // T73, the editor round-trip. One row per remote file still being watched:
    // {name, remotePath, localPath}. A file left in here after the user thinks
    // they are finished is a surprise upload to a production host, so the panel
    // shows this list whenever it is non-empty and every row has a Stop.
    Q_PROPERTY(QVariantList editing READ editing NOTIFY editingChanged)

  public:
    explicit SftpModel(QObject* parent = nullptr);  // owned by parent
    ~SftpModel() override;

    // Points this model at `backend`, or at nothing. Called by TerminalItem
    // every time the terminal's session changes, so the panel of a tab that has
    // been re-pointed at a local shell stops offering transfers.
    //
    // `host` is what to call the far end in the install confirmation. It is
    // display text only and never composes a path.
    void attach(net::SshBackend* backend, const QString& host = {});

    // The settings this model reads (`editor.command`, so far). Borrowed and
    // owned by main(); null in the tests, which then get the OS default.
    void setSettings(settings::Registry* registry);

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

    const QString& installStage() const { return m_install.stage; }

    const QString& installTitle() const { return m_install.title; }

    const QString& installPath() const { return m_install.path; }

    const QString& installPreview() const { return m_install.preview; }

    const QStringList& installChoices() const { return m_install.choices; }

    // T73. Starts the shell-integration flow: find the rc files that exist on
    // the server, then propose ONE edit to ONE of them. Nothing is written
    // until confirmShellIntegration(). `remove` takes the block out instead of
    // putting it in.
    Q_INVOKABLE void proposeShellIntegration(bool remove);

    // Picks between installChoices. `rc` must be one of them.
    //
    // BY VALUE. finishProbe() calls this with an element of m_install.found,
    // and three paths in here call resetInstall(), which assigns over the whole
    // Install — including the list the reference would point into. Today every
    // one of them happens to read `rc` first; a copy means that stays true
    // after the next edit.
    Q_INVOKABLE void chooseShellTarget(QString rc);

    // Writes what installPath/installPreview describe, and nothing else.
    Q_INVOKABLE void confirmShellIntegration();

    // Refuses it, or closes the surface once it is done. Either way nothing
    // further happens.
    Q_INVOKABLE void cancelShellIntegration();

    const QVariantList& editing() const { return m_editingRows; }

    // T73. Downloads `name` to a temp file of its own, opens it in the user's
    // editor, and uploads it back every time it is saved — until stopEditing().
    Q_INVOKABLE void editRemote(const QString& name);

    // Stops watching the temp file at `localPath` and deletes it. Explicit
    // because the alternative is a file still being watched after the user
    // believes they are finished, which is an upload nobody asked for.
    //
    // Keyed by the LOCAL path — the `localPath` field of an `editing` row —
    // because two remote folders may hold a file with the same name and both
    // may be open at once.
    Q_INVOKABLE void stopEditing(const QString& localPath);

    Q_INVOKABLE void stopAllEditing();

  signals:
    void availableChanged();
    void localChanged();
    void remoteChanged();
    void progressChanged();
    void installChanged();
    void editingChanged();
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

    // --- T73, the shell-integration installer ---------------------------
    //
    // Every rc file is read with sftpGet and written back with sftpPut rather
    // than appended to through the shell channel. A blind `>>` is not something
    // the user can be shown before it happens; a file we downloaded, spliced
    // and are about to upload is.
    // A shell start-up file larger than this is not one, and reading it into a
    // QString and then a QStringList is an allocation the far end chose the
    // size of (rules/net.md).
    static constexpr qint64 kMaxRcBytes = 1024 * 1024;

    void probeNextRc();
    void finishProbe();
    void resetInstall();
    void setInstallStage(const QString& stage);
    // The absolute remote path of `rc`, which is always relative to the login
    // directory.
    QString rcRemotePath(const QString& rc) const;

    // --- T73, the editor round-trip -------------------------------------
    struct Edit {
        QString name;        // leaf, already through isSafeLeafName
        QString remotePath;  // where it came from and where it goes back
        QString dir;         // a temp directory holding this ONE file
        QString local;
        // What we last sent or received. A save is a change against THIS, not
        // against whatever the watcher last shouted about — editors touch a
        // file several times per save and every touch is a signal.
        qint64 size = -1;
        qint64 mtimeMs = -1;
        bool uploading = false;
    };

    void startWatching(Edit& edit);
    // By value rather than by Edit&: this reaches the OS and can raise a
    // banner, and anything that runs an event loop can erase the entry a
    // reference would be pointing at.
    void launchEditor(const QString& local, const QString& name);
    // Both watcher signals funnel here. `local` is the key into m_edits.
    void noteEditActivity(const QString& local);
    void flushEdits();
    void rebuildEditingRows();
    // Removes the watch and the temp directory. Silent: called from the
    // destructor as well as from stopEditing().
    void discardEdit(const Edit& edit);

    net::SshBackend* m_backend = nullptr;      // borrowed; owned by TerminalItem
    settings::Registry* m_settings = nullptr;  // borrowed; owned by main()
    QString m_host;
    SftpRequests m_open;
    QString m_localPath;
    QString m_remotePath;
    // Where the login directory is, captured from the FIRST resolve and never
    // moved again. m_remotePath follows the user around; an rc file does not.
    QString m_homePath;
    QVariantList m_localEntries;
    QVariantList m_remoteEntries;
    QString m_activity;
    qulonglong m_done = 0;
    qulonglong m_total = 0;

    struct Install {
        QString stage;
        bool remove = false;
        // Waiting on the login directory before the probe can start.
        bool afterResolve = false;
        std::size_t probeIndex = 0;
        // The rc paths that turned out to exist, in probe order. Separate from
        // `choices` because zero found is still a question worth asking.
        QStringList found;
        QStringList choices;
        // rc path -> what that file holds right now. Small files, and holding
        // them means the confirmed write does not have to re-read a file that
        // may have changed since the user looked at it.
        QHash<QString, QString> contents;
        QString rc;
        QString title;
        QString path;
        QString preview;
        // Where a probe or the spliced file is staged. Removed with the model.
        QString scratchDir;
    };

    Install m_install;

    QHash<QString, Edit> m_edits;  // keyed by local path
    // Request id -> local path. The request's `subject` is the file's NAME,
    // which two edits in two directories can share; this is what makes a reply
    // belong to exactly one of them.
    QHash<quint64, QString> m_editRequests;
    QVariantList m_editingRows;
    QSet<QString> m_editDirty;
    QFileSystemWatcher* m_watcher = nullptr;  // owned by this, built lazily
    QTimer* m_settle = nullptr;               // owned by this, built lazily
};

}  // namespace krait::app

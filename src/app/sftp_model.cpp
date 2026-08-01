#include "sftp_model.h"

#include "../net/ssh/ssh_backend.h"
#include "settings/registry.h"
#include "shell_integration.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLocale>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace krait::app {

namespace {

// Names that still resolve to a DEVICE on Windows whatever directory they are
// written in, and whatever extension follows. A download saved to one is
// silently thrown away and reported as a success, which is the worst of the
// three possible outcomes.
constexpr const char* const kReservedNames[] = {
    "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
    "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
};

QString sizeText(qulonglong bytes) {
    // QLocale, not a hand-rolled ladder of divisions: it is already the app's
    // locale and already knows the units in it.
    return QLocale().formattedDataSize(static_cast<qint64>(bytes));
}

QString timeText(qint64 secondsSinceEpoch) {
    if (secondsSinceEpoch <= 0) {
        // The server did not say. An invented date would be indistinguishable
        // from a real one from 1970.
        return {};
    }
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(secondsSinceEpoch),
                              QLocale::ShortFormat);
}

}  // namespace

bool isSafeLeafName(const QString& name) {
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        return false;
    }
    for (const QChar ch : name) {
        const char16_t unit = ch.unicode();
        if (unit < 0x20 || unit == 0x7f) {
            return false;
        }
        // ':' belongs here as much as the separators do — "C:evil" is a path
        // relative to another drive's current directory, and "file:stream"
        // names an alternate data stream nobody will ever look at again.
        if (unit == u'/' || unit == u'\\' || unit == u':' || unit == u'*' || unit == u'?' ||
            unit == u'"' || unit == u'<' || unit == u'>' || unit == u'|') {
            return false;
        }
    }
    // Win32 strips trailing dots and spaces, so "report.txt." and "report.txt"
    // open the same file while a listing shows them as two.
    if (name.endsWith(u'.') || name.endsWith(u' ')) {
        return false;
    }
    const qsizetype dot = name.indexOf(u'.');
    const QString stem = (dot < 0 ? name : name.left(dot)).toUpper();
    for (const char* const reserved : kReservedNames) {
        if (stem == QLatin1StringView(reserved)) {
            return false;
        }
    }
    return true;
}

bool isShellExecutableName(const QString& name) {
    // A FIXED LIST, deliberately not %PATHEXT%.
    //
    // PATHEXT answers a different question — "what can I type at a prompt
    // without an extension" — and installing Python puts `.PY` in it. Reading
    // it here would refuse `main.py` on a developer's machine and allow it on
    // the build server, which is a worse property than either answer on its
    // own. This is the set whose default OPEN verb executes on a stock Windows:
    // real binaries, the script hosts, and the three indirections (.lnk, .url,
    // .scf) that point at one.
    //
    // .js and .vbs are on it and they are also files people edit. That is the
    // trade: double-clicking either runs it under WScript, which is how half of
    // Windows' malware arrives, and the cost of being wrong that way is much
    // higher than the cost of a banner naming editor.command.
    static constexpr std::array kRunOnOpen{".EXE", ".COM", ".BAT", ".CMD", ".SCR", ".PIF",
                                           ".MSI", ".MSP", ".MSC", ".CPL", ".LNK", ".URL",
                                           ".SCF", ".HTA", ".VBS", ".VBE", ".JS",  ".JSE",
                                           ".WSF", ".WSH", ".WS",  ".REG", ".PS1", ".PSM1"};

    const qsizetype dot = name.lastIndexOf(u'.');
    if (dot < 0) {
        return false;  // no extension: nothing is registered against it
    }
    // The LAST extension, which is the only one Windows acts on — "notes.txt.exe"
    // is an executable however harmless the middle of it looks.
    const QString suffix = name.sliced(dot).toUpper();
    for (const char* const runs : kRunOnOpen) {
        if (suffix == QLatin1StringView(runs)) {
            return true;
        }
    }
    return false;
}

QString remoteJoin(const QString& base, const QString& name) {
    if (base.isEmpty()) {
        return QStringLiteral("/") + name;
    }
    return base.endsWith(u'/') ? base + name : base + u'/' + name;
}

QString remoteParent(const QString& path) {
    const qsizetype slash = path.lastIndexOf(u'/');
    if (slash <= 0) {
        // "/home" and "home" and "/" all stop at the root. An up button that
        // walks off the top is one that has to be recovered from by typing.
        return QStringLiteral("/");
    }
    return path.left(slash);
}

quint64 SftpRequests::add(Kind kind, QString subject) {
    const quint64 id = m_next++;
    m_open.insert(id, Request{.kind = kind, .subject = std::move(subject)});
    return id;
}

std::optional<SftpRequests::Request> SftpRequests::take(quint64 id) {
    const auto it = m_open.constFind(id);
    if (it == m_open.cend()) {
        return std::nullopt;
    }
    const Request request = *it;
    m_open.erase(it);
    return request;
}

const SftpRequests::Request* SftpRequests::find(quint64 id) const {
    const auto it = m_open.constFind(id);
    return it == m_open.cend() ? nullptr : &*it;
}

SftpModel::SftpModel(QObject* parent) : QObject(parent), m_localPath(QDir::homePath()) {}

SftpModel::~SftpModel() {
    // Silent, and not through stopAllEditing(): the temp files hold REMOTE
    // content on a local disk and must go whatever else happens, but a
    // destructor is no place to emit signals at objects that may already be
    // gone.
    for (const Edit& edit : std::as_const(m_edits)) {
        discardEdit(edit);
    }
    if (!m_install.scratchDir.isEmpty()) {
        QDir(m_install.scratchDir).removeRecursively();
    }
}

void SftpModel::setSettings(settings::Registry* registry) {
    m_settings = registry;
}

void SftpModel::attach(net::SshBackend* backend, const QString& host) {
    if (m_backend == backend) {
        return;
    }
    if (m_backend != nullptr) {
        m_backend->disconnect(this);
    }
    m_backend = backend;
    m_host = host;
    // The login directory belonged to the session that just went, and so did
    // every file being watched: an editor save uploading to a host the tab is
    // no longer connected to is the surprise this exists to prevent.
    m_homePath.clear();
    resetInstall();
    stopAllEditing();
    // Everything in flight belonged to the session that just went. The ids are
    // NOT reused (see SftpRequests), so a reply already posted to this object
    // by the old backend's thread now matches nothing and is dropped.
    m_open.clear();
    // Cleared with it, or the id->path map keeps an entry per abandoned
    // transfer for the life of the tab.
    m_editRequests.clear();
    m_remotePath.clear();
    m_remoteEntries.clear();
    clearProgress();
    emit remoteChanged();
    emit progressChanged();
    emit availableChanged();
    if (m_backend == nullptr) {
        return;
    }
    // Explicitly queued (rules/cpp.md): SshBackend emits these from its worker
    // thread, and for a cancel it emits them from this one.
    connect(m_backend, &net::SshBackend::sftpListed, this, &SftpModel::handleListed,
            Qt::QueuedConnection);
    connect(m_backend, &net::SshBackend::sftpResolved, this, &SftpModel::handleResolved,
            Qt::QueuedConnection);
    connect(m_backend, &net::SshBackend::sftpProgress, this, &SftpModel::handleProgress,
            Qt::QueuedConnection);
    connect(m_backend, &net::SshBackend::sftpFinished, this, &SftpModel::handleFinished,
            Qt::QueuedConnection);
}

qreal SftpModel::progress() const {
    if (m_total == 0) {
        return -1.0;
    }
    return static_cast<qreal>(m_done) / static_cast<qreal>(m_total);
}

void SftpModel::start() {
    refreshLocal();
    if (m_backend == nullptr) {
        return;
    }
    if (m_remotePath.isEmpty()) {
        // "." is how SFTP is asked where the login directory is.
        m_backend->sftpResolve(m_open.add(SftpRequests::Kind::Resolve, QStringLiteral(".")),
                               QStringLiteral("."));
        emit progressChanged();
        return;
    }
    refreshRemote();
}

void SftpModel::refreshLocal() {
    const QDir dir(m_localPath);
    m_localEntries.clear();
    // Directories first then by name, the same order the remote side arrives
    // in — two panes that disagree about ordering are two panes you cannot
    // compare at a glance.
    const QFileInfoList infos =
        dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                          QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& info : infos) {
        QVariantMap row;
        row["name"] = info.fileName();
        row["isDir"] = info.isDir();
        row["isLink"] = info.isSymLink();
        row["sizeText"] = info.isDir() ? QString() : sizeText(static_cast<qulonglong>(info.size()));
        row["timeText"] = QLocale().toString(info.lastModified(), QLocale::ShortFormat);
        m_localEntries.append(row);
    }
    emit localChanged();
}

void SftpModel::refreshRemote() {
    if (m_backend == nullptr || m_remotePath.isEmpty()) {
        return;
    }
    m_backend->sftpList(m_open.add(SftpRequests::Kind::List, m_remotePath), m_remotePath);
    emit progressChanged();
}

void SftpModel::enterLocal(const QString& name) {
    if (!checkName(name)) {
        return;
    }
    const QString target = QDir::cleanPath(QDir(m_localPath).filePath(name));
    if (!QFileInfo(target).isDir()) {
        return;
    }
    m_localPath = target;
    refreshLocal();
}

void SftpModel::enterRemote(const QString& name) {
    if (m_backend == nullptr || !checkName(name)) {
        return;
    }
    m_remotePath = remoteJoin(m_remotePath, name);
    emit remoteChanged();
    refreshRemote();
}

void SftpModel::leaveLocal() {
    QDir dir(m_localPath);
    if (!dir.cdUp()) {
        return;
    }
    m_localPath = dir.absolutePath();
    refreshLocal();
}

void SftpModel::leaveRemote() {
    if (m_backend == nullptr || m_remotePath.isEmpty()) {
        return;
    }
    m_remotePath = remoteParent(m_remotePath);
    emit remoteChanged();
    refreshRemote();
}

QVariantMap SftpModel::findEntry(const QVariantList& entries, const QString& name) {
    for (const QVariant& entry : entries) {
        const QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("name")).toString() == name) {
            return row;
        }
    }
    return {};
}

bool SftpModel::checkName(const QString& name) {
    if (isSafeLeafName(name)) {
        return true;
    }
    emit errorRaised(tr("Refused the name “%1”: it is not a plain file name.").arg(name),
                     tr("A name carrying a path separator, a drive letter or a control character "
                        "would decide where the file lands instead of you."));
    return false;
}

void SftpModel::download(const QString& name) {
    if (m_backend == nullptr || !checkName(name)) {
        return;
    }
    const QVariantMap entry = findEntry(m_remoteEntries, name);
    if (entry.isEmpty()) {
        emit errorRaised(tr("“%1” is no longer in this remote folder.").arg(name), QString());
        return;
    }
    if (entry.value(QStringLiteral("isDir")).toBool()) {
        emit errorRaised(tr("Folders are not transferred yet — only files."), QString());
        return;
    }
    m_backend->sftpGet(m_open.add(SftpRequests::Kind::Download, name),
                       remoteJoin(m_remotePath, name), QDir(m_localPath).filePath(name));
    emit progressChanged();
}

void SftpModel::upload(const QString& name) {
    if (m_backend == nullptr || !checkName(name)) {
        return;
    }
    const QVariantMap entry = findEntry(m_localEntries, name);
    if (entry.isEmpty()) {
        emit errorRaised(tr("“%1” is no longer in this folder.").arg(name), QString());
        return;
    }
    if (entry.value(QStringLiteral("isDir")).toBool()) {
        emit errorRaised(tr("Folders are not transferred yet — only files."), QString());
        return;
    }
    m_backend->sftpPut(m_open.add(SftpRequests::Kind::Upload, name),
                       QDir(m_localPath).filePath(name), remoteJoin(m_remotePath, name));
    emit progressChanged();
}

void SftpModel::uploadUrls(const QList<QUrl>& urls) {
    if (m_backend == nullptr) {
        return;
    }
    for (const QUrl& url : urls) {
        const QString local = url.toLocalFile();
        if (local.isEmpty()) {
            emit errorRaised(tr("Only files from this computer can be uploaded."), url.toString());
            continue;
        }
        const QFileInfo info(local);
        if (info.isDir()) {
            emit errorRaised(tr("Folders are not transferred yet — only files."), QString());
            continue;
        }
        // The leaf becomes part of a REMOTE path, so it goes through the same
        // check a remote name does — a dropped URL has been through somebody
        // else's code before it reached us.
        const QString name = info.fileName();
        if (!checkName(name)) {
            continue;
        }
        m_backend->sftpPut(m_open.add(SftpRequests::Kind::Upload, name), local,
                           remoteJoin(m_remotePath, name));
    }
    emit progressChanged();
}

void SftpModel::cancel() {
    if (m_backend != nullptr) {
        m_backend->sftpCancelAll();
    }
}

void SftpModel::handleListed(quint64 requestId, const QString& path, const QVariantList& entries) {
    const SftpRequests::Request* request = m_open.find(requestId);
    if (request == nullptr || request->kind != SftpRequests::Kind::List) {
        return;
    }
    m_remotePath = path;
    m_remoteEntries.clear();
    for (const QVariant& entry : entries) {
        const QVariantMap source = entry.toMap();
        QVariantMap row;
        // Server-controlled. It is rendered as PlainText and never used to
        // compose a path without isSafeLeafName() first.
        row["name"] = source.value(QStringLiteral("name")).toString();
        row["isDir"] = source.value(QStringLiteral("isDir")).toBool();
        row["isLink"] = source.value(QStringLiteral("isLink")).toBool();
        row["sizeText"] = row["isDir"].toBool()
                              ? QString()
                              : sizeText(source.value(QStringLiteral("size")).toULongLong());
        row["timeText"] = timeText(source.value(QStringLiteral("mtime")).toLongLong());
        m_remoteEntries.append(row);
    }
    emit remoteChanged();
}

void SftpModel::handleResolved(quint64 requestId, const QString& path) {
    const SftpRequests::Request* request = m_open.find(requestId);
    if (request == nullptr || request->kind != SftpRequests::Kind::Resolve) {
        return;
    }
    m_remotePath = path;
    // Captured once and never moved again: m_remotePath follows the user around
    // the server, and an rc file does not live wherever they happen to be.
    if (m_homePath.isEmpty()) {
        m_homePath = path;
    }
    emit remoteChanged();
    refreshRemote();
    if (m_install.afterResolve) {
        m_install.afterResolve = false;
        probeNextRc();
    }
}

void SftpModel::handleProgress(quint64 requestId, qulonglong done, qulonglong total) {
    const SftpRequests::Request* request = m_open.find(requestId);
    if (request == nullptr) {
        return;
    }
    m_done = done;
    m_total = total;
    switch (request->kind) {
    case SftpRequests::Kind::Download:
    case SftpRequests::Kind::EditDownload:
        m_activity = tr("Downloading %1").arg(request->subject);
        break;
    case SftpRequests::Kind::EditUpload:
        // Said differently from a plain upload on purpose: this one was started
        // by the user's EDITOR, not by a button they just pressed, and "saving"
        // is the word that connects it to what they did.
        m_activity = tr("Saving %1 back to the server").arg(request->subject);
        break;
    case SftpRequests::Kind::WriteRc:
        m_activity = tr("Writing %1").arg(request->subject);
        break;
    case SftpRequests::Kind::Probe:
    case SftpRequests::Kind::Resolve:
    case SftpRequests::Kind::List:
        m_activity = tr("Reading %1").arg(request->subject);
        break;
    case SftpRequests::Kind::Upload:
        m_activity = tr("Uploading %1").arg(request->subject);
        break;
    }
    emit progressChanged();
}

void SftpModel::clearProgress() {
    m_activity.clear();
    m_done = 0;
    m_total = 0;
}

void SftpModel::handleFinished(quint64 requestId, bool ok, bool cancelled, const QString& message) {
    const std::optional<SftpRequests::Request> request = m_open.take(requestId);
    if (!request.has_value()) {
        // A reply to the session this panel used to be pointed at.
        return;
    }
    if (m_open.empty()) {
        clearProgress();
    }
    emit progressChanged();

    // A FAILED PROBE IS THE ANSWER, not a failure: "there is no ~/.zshrc" is
    // exactly what this request was asking. Handled before anything that could
    // raise a banner, because five banners for five absent files is how a
    // detection step becomes something people cancel out of.
    if (request->kind == SftpRequests::Kind::Probe) {
        if (cancelled) {
            resetInstall();
            return;
        }
        if (ok) {
            QFile staged(QDir(m_install.scratchDir).filePath(QStringLiteral("probe")));
            // CAPPED before readAll(). rules/net.md: cap every remotely
            // influenced allocation. Sftp::get streams to disk so the download
            // itself is bounded by the disk, but pulling it into a QString and
            // then splitting it into a QStringList is not — and a start-up file
            // over a megabyte is not a start-up file.
            if (staged.size() <= kMaxRcBytes && staged.open(QIODevice::ReadOnly)) {
                m_install.found.append(request->subject);
                m_install.contents.insert(request->subject, QString::fromUtf8(staged.readAll()));
            }
        }
        ++m_install.probeIndex;
        probeNextRc();
        return;
    }

    if (request->kind == SftpRequests::Kind::WriteRc) {
        if (!ok) {
            emit errorRaised(cancelled ? tr("The change to %1 was stopped.").arg(m_install.path)
                                       : tr("Could not write %1.").arg(m_install.path),
                             message);
            resetInstall();
            return;
        }
        m_install.title =
            m_install.remove
                ? tr("Removed. Open a new shell on %1 to see the change.").arg(m_host)
                : tr("Installed. Open a new shell on %1 to see the change.").arg(m_host);
        m_install.preview.clear();
        setInstallStage(QStringLiteral("done"));
        return;
    }

    if (request->kind == SftpRequests::Kind::EditDownload ||
        request->kind == SftpRequests::Kind::EditUpload) {
        const QString local = m_editRequests.take(requestId);
        const auto it = m_edits.find(local);
        if (it == m_edits.end()) {
            return;  // the user stopped watching while this was in flight
        }
        if (request->kind == SftpRequests::Kind::EditUpload) {
            it->uploading = false;
            if (!ok && !cancelled) {
                emit errorRaised(tr("Could not save “%1” back to the server.").arg(it->name),
                                 message);
            }
            return;
        }
        if (!ok) {
            if (!cancelled) {
                emit errorRaised(tr("Could not open “%1” for editing.").arg(it->name), message);
            }
            discardEdit(*it);
            m_edits.erase(it);
            rebuildEditingRows();
            return;
        }
        startWatching(*it);
        // By value, not through the iterator: launching an editor reaches the
        // OS, and the banner it can raise reaches QML — either of which may run
        // an event loop, and a Stop watching in there erases the entry this
        // reference points at.
        launchEditor(it->local, it->name);
        rebuildEditingRows();
        return;
    }

    // A resolve that failed while the installer was waiting on it leaves that
    // flow with no login directory to hang an rc path off, so it stops here
    // rather than sitting at "probing" forever.
    if (!ok && m_install.afterResolve && request->kind == SftpRequests::Kind::Resolve) {
        resetInstall();
    }

    // A cancel is not a banner: the user pressed the button, and a banner for
    // something they just asked for is how banners get ignored.
    if (ok || cancelled) {
        if (ok && request->kind == SftpRequests::Kind::Download) {
            refreshLocal();
        } else if (ok && request->kind == SftpRequests::Kind::Upload) {
            refreshRemote();
        }
        return;
    }
    switch (request->kind) {
    case SftpRequests::Kind::Resolve:
    case SftpRequests::Kind::List:
        emit errorRaised(tr("Could not read the remote folder."), message);
        return;
    case SftpRequests::Kind::Download:
        emit errorRaised(tr("Could not download “%1”.").arg(request->subject), message);
        return;
    case SftpRequests::Kind::Upload:
        emit errorRaised(tr("Could not upload “%1”.").arg(request->subject), message);
        return;
    case SftpRequests::Kind::Probe:
    case SftpRequests::Kind::WriteRc:
    case SftpRequests::Kind::EditDownload:
    case SftpRequests::Kind::EditUpload:
        return;  // answered above, and listed so the switch stays exhaustive
    }
}

// ---------------------------------------------------------------------------
// T73 — shell integration, installed onto SOMEONE ELSE'S MACHINE.
//
// Read the file, edit it here, write it back. Not `cat >> ~/.bashrc` through
// the shell channel: an append you cannot show anyone before it happens is not
// something a user can refuse, and it cannot see that a block is already there.
// ---------------------------------------------------------------------------

namespace {

// The candidates, for the banner that says where Krait looked. Naming them is
// what turns "could not find it" into something the user can act on.
QString knownRcPaths() {
    QStringList names;
    for (const ShellTarget& target : shellTargets()) {
        names.append(QStringLiteral("~/") + target.rc);
    }
    return names.join(QStringLiteral(", "));
}

}  // namespace

QString SftpModel::rcRemotePath(const QString& rc) const {
    // No isSafeLeafName here, deliberately: `rc` comes from shellTargets(),
    // which is a table in this build, not from anything the server said. The
    // guard exists for names a server chose, and adding it to a constant would
    // suggest this one was ever in doubt.
    return remoteJoin(m_homePath, rc);
}

void SftpModel::setInstallStage(const QString& stage) {
    m_install.stage = stage;
    emit installChanged();
}

void SftpModel::resetInstall() {
    if (!m_install.scratchDir.isEmpty()) {
        QDir(m_install.scratchDir).removeRecursively();
    }
    m_install = Install{};
    emit installChanged();
}

void SftpModel::proposeShellIntegration(bool remove) {
    if (m_backend == nullptr || !m_install.stage.isEmpty()) {
        return;  // one at a time: the surface is already asking something
    }
    resetInstall();
    m_install.remove = remove;
    m_install.scratchDir = QDir::temp().filePath(
        QStringLiteral("krait-shell-%1").arg(QUuid::createUuid().toString(QUuid::Id128)));
    if (!QDir().mkpath(m_install.scratchDir)) {
        m_install.scratchDir.clear();
        emit errorRaised(tr("Could not make a temporary folder."), QDir::tempPath());
        return;
    }
    m_install.title = tr("Looking for shell start-up files on %1…").arg(m_host);
    setInstallStage(QStringLiteral("probing"));
    if (m_homePath.isEmpty()) {
        // Every rc path is relative to the login directory, so there is nothing
        // to probe until the server has said where that is.
        m_install.afterResolve = true;
        start();
        return;
    }
    probeNextRc();
}

void SftpModel::probeNextRc() {
    if (m_backend == nullptr) {
        resetInstall();
        return;
    }
    const std::span<const ShellTarget> all = shellTargets();
    if (m_install.probeIndex >= all.size()) {
        finishProbe();
        return;
    }
    const QString& rc = all[m_install.probeIndex].rc;
    // Downloading it IS the existence test, and it is the same download the
    // write step needs anyway — a separate stat would be a second round trip
    // that answers less.
    m_backend->sftpGet(m_open.add(SftpRequests::Kind::Probe, rc), rcRemotePath(rc),
                       QDir(m_install.scratchDir).filePath(QStringLiteral("probe")));
    emit progressChanged();
}

void SftpModel::finishProbe() {
    if (m_install.found.size() == 1) {
        chooseShellTarget(m_install.found.constFirst());
        return;
    }
    if (m_install.found.isEmpty()) {
        // NOTHING IS OFFERED HERE, and that is the whole point.
        //
        // A probe that failed does not mean the file is absent — it means we
        // could not read it, and "no such file" and "permission denied" arrive
        // identically. Offering an unreadable target as a file to CREATE would
        // put an empty string into contents, splice a block into nothing, and
        // upload the result over a 200-line .bashrc that was there all along:
        // Sftp::put truncates. Refusing is the only answer that keeps "Krait
        // never clobbers" true, and a user whose machine really has no start-up
        // file can make an empty one and ask again.
        emit errorRaised(
            m_install.remove
                ? tr("There is no shell integration to remove on %1.").arg(m_host)
                : tr("Krait could not read any shell start-up file on %1.").arg(m_host),
            tr("It looked for %1. Create the one your shell uses and try again — "
               "Krait will not write over a file it could not read first.")
                .arg(knownRcPaths()));
        resetInstall();
        return;
    }
    m_install.choices = m_install.found;
    m_install.title = tr("%1 has more than one shell start-up file.").arg(m_host);
    setInstallStage(QStringLiteral("choosing"));
}

void SftpModel::chooseShellTarget(QString rc) {  // NOLINT(performance-unnecessary-value-param)
    const ShellTarget* target = shellTargetFor(rc);
    if (target == nullptr || m_install.stage.isEmpty()) {
        return;  // a surface that has moved on
    }
    const QString existing = m_install.contents.value(rc);
    const BlockState state = blockState(existing);
    if (state == BlockState::Damaged) {
        // NOT touched. A block whose markers do not pair up has no end this
        // code can find, and picking one truncates a file on a machine we are
        // a guest on.
        emit errorRaised(tr("The Krait block in %1 looks edited.").arg(rcRemotePath(rc)),
                         tr("Its start and end markers do not pair up, so Krait cannot tell "
                            "where the block ends. Nothing was changed."));
        resetInstall();
        return;
    }
    if (state == BlockState::Absent && m_install.remove) {
        emit errorRaised(tr("There is no Krait block in %1.").arg(rcRemotePath(rc)), QString());
        resetInstall();
        return;
    }

    QString payload;
    if (!m_install.remove) {
        const QString path = scriptPath(target->script);
        QFile script(path);
        if (path.isEmpty() || !script.open(QIODevice::ReadOnly)) {
            emit errorRaised(tr("Could not read the bundled %1 script.").arg(target->shell),
                             tr("It should be in the shell-integration folder beside Krait."));
            resetInstall();
            return;
        }
        payload = QString::fromUtf8(script.readAll());
    }

    m_install.rc = rc;
    m_install.path = rcRemotePath(rc);
    m_install.preview = payload;
    m_install.choices.clear();
    if (m_install.remove) {
        m_install.title = tr("Remove Krait's block from %1?").arg(m_install.path);
    } else if (state == BlockState::Present) {
        // REPLACE, and only between the markers. Whatever else is in that file
        // is somebody's working shell and is returned byte for byte.
        m_install.title = tr("Replace Krait's block in %1?").arg(m_install.path);
    } else {
        m_install.title = tr("Add Krait's block to %1?").arg(m_install.path);
    }
    setInstallStage(QStringLiteral("proposed"));
}

void SftpModel::confirmShellIntegration() {
    if (m_backend == nullptr || m_install.stage != QStringLiteral("proposed")) {
        return;
    }
    const QString updated = spliceBlock(m_install.contents.value(m_install.rc), m_install.preview);
    const QString staged = QDir(m_install.scratchDir).filePath(QStringLiteral("rc"));
    QFile file(staged);
    // CHECKED, all of it. sftpPut truncates the remote file, so a staged copy
    // that came up short because the local disk filled would replace someone's
    // working .bashrc with the part of it that fit.
    const QByteArray bytes = updated.toUtf8();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(bytes) != bytes.size() || !file.flush()) {
        emit errorRaised(tr("Could not stage the change on this computer."), file.errorString());
        resetInstall();
        return;
    }
    file.close();
    setInstallStage(QStringLiteral("writing"));
    m_backend->sftpPut(m_open.add(SftpRequests::Kind::WriteRc, m_install.rc), staged,
                       m_install.path);
    emit progressChanged();
}

void SftpModel::cancelShellIntegration() {
    resetInstall();
}

// ---------------------------------------------------------------------------
// T73 — the editor round trip.
// ---------------------------------------------------------------------------

void SftpModel::editRemote(const QString& name) {
    if (m_backend == nullptr || !checkName(name)) {
        return;
    }
    const QVariantMap entry = findEntry(m_remoteEntries, name);
    if (entry.isEmpty()) {
        emit errorRaised(tr("“%1” is no longer in this remote folder.").arg(name), QString());
        return;
    }
    if (entry.value(QStringLiteral("isDir")).toBool()) {
        emit errorRaised(tr("Folders cannot be opened in an editor — only files."), QString());
        return;
    }
    const QString remote = remoteJoin(m_remotePath, name);
    for (const Edit& open : std::as_const(m_edits)) {
        if (open.remotePath == remote) {
            emit errorRaised(tr("“%1” is already open for editing.").arg(name),
                             tr("Stop watching it before opening it again."));
            return;
        }
    }

    // A directory of its own per file, with the file keeping its name inside
    // it. Two reasons, and both matter: the directory watch that survives a
    // save-by-rename then only ever hears about THIS file, and the name — which
    // the SERVER chose — has already been through isSafeLeafName above, so it
    // cannot steer where the temp file lands.
    const QString dir = QDir::temp().filePath(
        QStringLiteral("krait-edit-%1").arg(QUuid::createUuid().toString(QUuid::Id128)));
    if (!QDir().mkpath(dir)) {
        emit errorRaised(tr("Could not make a temporary folder."), QDir::tempPath());
        return;
    }
    Edit edit;
    edit.name = name;
    edit.remotePath = remote;
    edit.dir = dir;
    edit.local = QDir(dir).filePath(name);
    m_edits.insert(edit.local, edit);

    const quint64 id = m_open.add(SftpRequests::Kind::EditDownload, name);
    m_editRequests.insert(id, edit.local);
    m_backend->sftpGet(id, remote, edit.local);
    emit progressChanged();
}

void SftpModel::startWatching(Edit& edit) {
    const QFileInfo info(edit.local);
    edit.size = info.size();
    edit.mtimeMs = info.lastModified().toMSecsSinceEpoch();

    if (m_watcher == nullptr) {
        m_watcher = new QFileSystemWatcher(this);  // owned by this
        connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &SftpModel::noteEditActivity);
        connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString& dir) {
            for (const Edit& open : std::as_const(m_edits)) {
                if (open.dir == dir) {
                    noteEditActivity(open.local);
                }
            }
        });
    }
    // BOTH PATHS, and the DIRECTORY is the load-bearing one.
    //
    // Qt documents that a watched file stops being watched the moment it is
    // renamed or removed — and "write a new file, then rename it over the old
    // one" is how most editors save. The file watch then fires exactly once and
    // never again, which is the single most common way a feature like this
    // silently stops working. A directory survives that, and its change signal
    // is what puts the file watch back (see noteEditActivity, which is Qt's own
    // documented recovery for fileChanged).
    m_watcher->addPath(edit.dir);
    m_watcher->addPath(edit.local);
}

void SftpModel::noteEditActivity(const QString& local) {
    if (!m_edits.contains(local)) {
        return;
    }
    if (m_watcher != nullptr && !m_watcher->files().contains(local) && QFile::exists(local)) {
        m_watcher->addPath(local);
    }
    m_editDirty.insert(local);
    if (m_settle == nullptr) {
        m_settle = new QTimer(this);  // owned by this
        m_settle->setSingleShot(true);
        // One save is several file operations — a temp file, a rename, a
        // permissions touch — and each one is a signal. Coalescing them is the
        // difference between one upload per save and four.
        m_settle->setInterval(300);
        connect(m_settle, &QTimer::timeout, this, &SftpModel::flushEdits);
    }
    m_settle->start();
}

void SftpModel::flushEdits() {
    if (m_backend == nullptr) {
        m_editDirty.clear();
        return;
    }
    const QSet<QString> dirty = std::exchange(m_editDirty, {});
    for (const QString& local : dirty) {
        const auto it = m_edits.find(local);
        if (it == m_edits.end()) {
            continue;
        }
        const QFileInfo info(local);
        if (!info.exists()) {
            // Mid-rename. The directory watch says so again the moment the new
            // file lands, so there is nothing to do and nothing to report.
            continue;
        }
        if (it->uploading) {
            m_editDirty.insert(local);  // after the one already in flight
            m_settle->start();
            continue;
        }
        const qint64 size = info.size();
        const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
        if (size == it->size && mtimeMs == it->mtimeMs) {
            // Touched, not changed. Without this every watcher signal that is
            // not a save — and there are several per save — is an upload to a
            // machine somebody else is using.
            continue;
        }
        it->size = size;
        it->mtimeMs = mtimeMs;
        it->uploading = true;
        const quint64 id = m_open.add(SftpRequests::Kind::EditUpload, it->name);
        m_editRequests.insert(id, local);
        m_backend->sftpPut(id, local, it->remotePath);
    }
    emit progressChanged();
}

void SftpModel::launchEditor(const QString& local, const QString& name) {
    const QString command = m_settings == nullptr
                                ? QString()
                                : QString::fromStdString(m_settings->text("editor.command"));
    if (command.isEmpty()) {
        // REFUSED, not opened. The default is the OS association, and asking
        // the OS to open a .exe means asking it to run one — from a temp file
        // this process wrote, so it carries no mark-of-the-web either. "Edit"
        // must never be a way for a server to choose what executes here.
        if (isShellExecutableName(name)) {
            emit errorRaised(
                tr("“%1” is a program, so Krait will not hand it to Windows.").arg(name),
                tr("Opening it with what this computer associates with it would run "
                   "it. Name a text editor in the editor.command setting to open "
                   "files like this one."));
            return;
        }
        // Qt is explicit that true here means only that the OS was ASKED: the
        // editor may still fail to start, and it will not report back. That is
        // also why a save is detected by watching the file and never by a
        // process exiting — most GUI editors hand the file to a window that is
        // already open and return immediately.
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(local))) {
            emit errorRaised(tr("Nothing on this computer opens “%1”.").arg(name),
                             tr("Name the editor you want in the editor.command setting."));
        }
        return;
    }
    // splitCommand, not a split on spaces: an editor under Program Files is a
    // quoted path, and `code --wait` is a program plus a flag.
    QStringList parts = QProcess::splitCommand(command);
    if (parts.isEmpty()) {
        emit errorRaised(tr("The editor.command setting has no program in it."), command);
        return;
    }
    const QString program = parts.takeFirst();
    parts.append(local);
    if (!QProcess::startDetached(program, parts)) {
        emit errorRaised(tr("Could not start the editor."), command);
    }
}

void SftpModel::discardEdit(const Edit& edit) {
    if (m_watcher != nullptr) {
        m_watcher->removePath(edit.local);
        m_watcher->removePath(edit.dir);
    }
    // The file holds REMOTE content on a local disk, so it goes with the watch.
    // An editor still holding it open makes the delete fail on Windows, and
    // that is not worth a banner: the user asked to stop watching, not to fight
    // their editor for a lock, and the OS clears its own temp folder.
    QDir(edit.dir).removeRecursively();
}

void SftpModel::stopEditing(const QString& localPath) {
    // BY LOCAL PATH, not by name. Two remote directories can hold a config.yml
    // each, and both are allowed to be open at once — matching on the leaf
    // would stop whichever one the hash reached first, leaving the file the
    // user meant to stop still uploading and tearing the other's temp file out
    // from under a live editor.
    const auto it = m_edits.find(localPath);
    if (it == m_edits.end()) {
        return;
    }
    m_editDirty.remove(localPath);
    discardEdit(*it);
    m_edits.erase(it);
    rebuildEditingRows();
}

void SftpModel::stopAllEditing() {
    for (const Edit& edit : std::as_const(m_edits)) {
        discardEdit(edit);
    }
    m_edits.clear();
    m_editDirty.clear();
    rebuildEditingRows();
}

void SftpModel::rebuildEditingRows() {
    // Sorted, because a QHash hands them back in whatever order it likes and a
    // list of watched files that reshuffles itself is one nobody can read.
    QStringList names;
    names.reserve(m_edits.size());
    for (const Edit& edit : std::as_const(m_edits)) {
        names.append(edit.local);
    }
    std::ranges::sort(names);

    m_editingRows.clear();
    for (const QString& local : std::as_const(names)) {
        const Edit& edit = m_edits[local];
        QVariantMap row;
        row["name"] = edit.name;
        row["remotePath"] = edit.remotePath;
        row["localPath"] = edit.local;
        m_editingRows.append(row);
    }
    emit editingChanged();
}

}  // namespace krait::app

#include "sftp_model.h"

#include "../net/ssh/ssh_backend.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QUrl>
#include <QVariantMap>

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

void SftpModel::attach(net::SshBackend* backend) {
    if (m_backend == backend) {
        return;
    }
    if (m_backend != nullptr) {
        m_backend->disconnect(this);
    }
    m_backend = backend;
    // Everything in flight belonged to the session that just went. The ids are
    // NOT reused (see SftpRequests), so a reply already posted to this object
    // by the old backend's thread now matches nothing and is dropped.
    m_open.clear();
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
    const QFileInfoList infos = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
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
    emit remoteChanged();
    refreshRemote();
}

void SftpModel::handleProgress(quint64 requestId, qulonglong done, qulonglong total) {
    const SftpRequests::Request* request = m_open.find(requestId);
    if (request == nullptr) {
        return;
    }
    m_done = done;
    m_total = total;
    m_activity = request->kind == SftpRequests::Kind::Download
                     ? tr("Downloading %1").arg(request->subject)
                     : tr("Uploading %1").arg(request->subject);
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
    }
}

}  // namespace krait::app

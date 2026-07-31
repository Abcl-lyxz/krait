#include "tcp_backend.h"

#include "reconnect.h"

#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <utility>

namespace krait::net {
namespace {

// One read's worth. Matches the ConPTY reader's chunk size: the parser is fed
// in chunks and the renderer coalesces damage, so a bigger buffer buys nothing
// and a smaller one costs syscalls.
constexpr qint64 kReadChunk = 65536;

// How much may sit unsent before a reply is dropped.
//
// Qt caps READS (setReadBufferSize) but has no write-buffer cap at all, so
// without this the queue grows until allocation fails. That is reachable from
// the far end and amplified: a server that repeats `IAC SB TERMINAL-TYPE SEND
// IAC SE` (6 bytes) and stops reading gets a 20-byte answer each time, so
// ~40 MB/s of unsendable queue on a 100 Mbps link. rules/net.md: "cap all
// remotely-influenced allocations" and "rate-limit terminal answerbacks".
//
// 1 MB is far above any legitimate burst — the largest thing generated here is
// a terminal-type string — and far below a size that hurts.
constexpr qint64 kMaxPendingWrite = 1 << 20;

}  // namespace

TcpBackend::TcpBackend(TcpConfig config, QObject* parent)
    : IBackend(parent), m_config(std::move(config)) {}

TcpBackend::~TcpBackend() = default;

void TcpBackend::handshake(std::vector<std::uint8_t>* /*reply*/) {}

void TcpBackend::windowChanged(int /*cols*/, int /*rows*/, std::vector<std::uint8_t>* /*reply*/) {}

void TcpBackend::resetCodec() {}

bool TcpBackend::isConnected() const {
    return m_socket != nullptr && m_socket->state() == QAbstractSocket::ConnectedState;
}

bool TcpBackend::start(int cols, int rows) {
    if (m_socket != nullptr) {
        return true;  // idempotent, like every other backend's start
    }
    m_cols = cols;
    m_rows = rows;
    m_stopping = false;
    m_attempt = 0;

    m_connectTimer = new QTimer(this);  // owned by this
    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, [this] {
        if (isConnected() || m_socket == nullptr) {
            return;
        }
        // ABORTED, not merely reported. Leaving the connect running means the
        // OS gives up ~21 s later and raises its own error — a second banner
        // for the same failure — or, worse, succeeds at 16 s and the tab comes
        // alive under a banner saying nothing answered.
        m_sawError = true;
        m_socket->disconnect(this);
        m_socket->abort();
        fail(ErrorCode::ConnectFailed, tr("No answer from %1 after %2 seconds.")
                                           .arg(QString::fromStdString(m_config.host))
                                           .arg(m_config.connectTimeoutSeconds));
    });

    m_retryTimer = new QTimer(this);  // owned by this
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, [this] { openSocket(); });

    openSocket();
    return true;
}

void TcpBackend::openSocket() {
    if (m_stopping) {
        return;
    }
    // A fresh socket per attempt. Reusing one after an error means carrying
    // whatever state put it in that error into the next connection.
    if (m_socket != nullptr) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
    }
    m_sawError = false;
    // BEFORE the socket, so a stateful codec is built with the real grid size.
    // Doing it afterwards meant the first window-size report carried the 80x24
    // default however large the window actually was — and a terminal that only
    // tells the far end its size when the size CHANGES never corrects it.
    resetCodec();
    m_socket = new QTcpSocket(this);  // owned by this
    connect(m_socket, &QTcpSocket::readyRead, this, &TcpBackend::handleReadyRead);
    connect(m_socket, &QTcpSocket::connected, this, &TcpBackend::handleConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &TcpBackend::handleDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &TcpBackend::handleSocketError);

    m_connectTimer->start(m_config.connectTimeoutSeconds * 1000);
    m_socket->connectToHost(QString::fromStdString(m_config.host),
                            static_cast<quint16>(m_config.port));
}

void TcpBackend::handleConnected() {
    m_connectTimer->stop();
    m_everConnected = true;
    m_attempt = 0;
    m_reply.clear();
    // Now and not in start(): writing before the socket is up puts bytes in a
    // buffer that a failed connect throws away, and the server then sees a
    // client that never opened.
    handshake(&m_reply);
    flushReply();
    emit connected();
}

void TcpBackend::handleReadyRead() {
    while (m_socket != nullptr && m_socket->bytesAvailable() > 0) {
        const QByteArray chunk = m_socket->read(kReadChunk);
        if (chunk.isEmpty()) {
            break;
        }
        m_data.clear();
        m_reply.clear();
        // The bytes are HOSTILE and go nowhere near the terminal until the
        // codec has taken the protocol out of them.
        decode({reinterpret_cast<const std::uint8_t*>(chunk.constData()),
                static_cast<std::size_t>(chunk.size())},
               &m_data, &m_reply);
        flushReply();
        if (!m_data.empty()) {
            emit outputReceived(QByteArray(reinterpret_cast<const char*>(m_data.data()),
                                           static_cast<qsizetype>(m_data.size())));
        }
    }
}

void TcpBackend::flushReply() {
    if (m_reply.empty() || m_socket == nullptr) {
        return;
    }
    if (m_socket->bytesToWrite() > kMaxPendingWrite) {
        // The far end has stopped reading while still sending. Dropping the
        // reply is the only bounded answer: the alternative is a queue that
        // grows as fast as the network delivers, and a protocol answer that
        // arrives a gigabyte late is worth nothing anyway.
        m_reply.clear();
        return;
    }
    m_socket->write(reinterpret_cast<const char*>(m_reply.data()),
                    static_cast<qint64>(m_reply.size()));
    m_reply.clear();
}

void TcpBackend::writeInput(const QByteArray& bytes) {
    if (!isConnected() || bytes.isEmpty()) {
        return;
    }
    m_reply.clear();
    encode({reinterpret_cast<const std::uint8_t*>(bytes.constData()),
            static_cast<std::size_t>(bytes.size())},
           &m_reply);
    flushReply();
}

void TcpBackend::resize(int cols, int rows) {
    m_cols = cols;
    m_rows = rows;
    m_reply.clear();
    windowChanged(cols, rows, &m_reply);
    flushReply();
}

void TcpBackend::handleDisconnected() {
    if (m_stopping) {
        return;
    }
    m_connectTimer->stop();
    // A break that already raised an error must not be reported twice:
    // handleSocketError has said what happened and, if the policy allows,
    // scheduled a retry.
    if (m_sawError) {
        return;
    }
    if (m_everConnected) {
        emit exited(0);
        return;
    }
    fail(ErrorCode::ConnectFailed, tr("The server closed the connection immediately."));
}

void TcpBackend::handleSocketError() {
    if (m_stopping || m_socket == nullptr) {
        return;
    }
    m_connectTimer->stop();
    if (m_socket->error() == QAbstractSocket::RemoteHostClosedError && m_everConnected) {
        // MEASURED, not assumed: Qt reports RemoteHostClosedError for a polite
        // FIN and for a reset alike — both were run against the test server,
        // and neither the error code nor the signal order tells them apart.
        // Neither protocol above this carries a logout message either, so
        // there is no layer at which the difference could be learned.
        //
        // Reported as a clean end, which is the choice with the better failure
        // mode: the common case is the far end ending the session, and a
        // banner on every normal logout is precisely how a banner teaches
        // people to ignore banners. The cost is that a genuinely dropped
        // connection also reads as a clean exit — so reconnect covers CONNECT
        // failures, which are identifiable, and not mid-session closes.
        return;
    }
    m_sawError = true;
    // The taxonomy, not the socket's wording: a banner needs a sentence about
    // the connection, and the socket's own text goes in the detail where a bug
    // report can use it.
    const ErrorCode code = m_everConnected ? ErrorCode::PeerClosed : ErrorCode::ConnectFailed;
    fail(code, m_socket->errorString());
}

void TcpBackend::fail(ErrorCode code, const QString& message) {
    emit errorOccurred(errorCodeName(code), message);
    if (isRetryable(code) && m_config.maxReconnectAttempts > 0 &&
        m_attempt < m_config.maxReconnectAttempts) {
        scheduleReconnect();
    }
}

void TcpBackend::scheduleReconnect() {
    ++m_attempt;
    const int delay = backoffDelayMs(m_attempt);
    emit reconnecting(m_attempt, m_config.maxReconnectAttempts, delay);
    m_everConnected = false;
    m_retryTimer->start(delay);
}

void TcpBackend::stop() {
    // The APP calls this from a thread pool (TerminalItem::resetSession), which
    // is correct for SshBackend — its stop() joins a worker that can be inside
    // an uninterruptible libssh call, and doing that on the GUI thread freezes
    // the window. Nothing here blocks, but everything here is a QObject living
    // on the GUI thread: QTimer::stop from another thread is refused outright
    // ("Timers cannot be stopped from another thread") and does NOTHING, so a
    // pending reconnect would survive; and QTcpSocket::abort would rip the
    // socket engine out from under a handleReadyRead() already running.
    //
    // Queued, never BlockingQueued: ~QThreadPool waits on the GUI thread at
    // shutdown, so a blocking hop from a pool thread deadlocks. The
    // deleteLater() the caller issues afterwards is posted after this, so the
    // ordering holds.
    if (thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(this, [this] { stop(); }, Qt::QueuedConnection);
        return;
    }
    m_stopping = true;
    if (m_retryTimer != nullptr) {
        m_retryTimer->stop();
    }
    if (m_connectTimer != nullptr) {
        m_connectTimer->stop();
    }
    if (m_socket != nullptr) {
        // abort(), not disconnectFromHost(): the latter waits for the peer to
        // acknowledge, and a tab being closed must not depend on a server that
        // may be exactly why it is being closed.
        m_socket->disconnect(this);
        m_socket->abort();
    }
}

}  // namespace krait::net

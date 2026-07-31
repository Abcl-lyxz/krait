#include "telnet_backend.h"

#include "../reconnect.h"

#include <QTcpSocket>
#include <QTimer>

#include <utility>

namespace krait::net {
namespace {

// One read's worth. Matches the ConPTY reader's chunk size: the parser is fed
// in chunks and the renderer coalesces damage, so a bigger buffer buys nothing
// and a smaller one costs syscalls.
constexpr qint64 kReadChunk = 65536;

}  // namespace

TelnetBackend::TelnetBackend(TelnetConfig config, QObject* parent)
    : IBackend(parent), m_config(std::move(config)) {}

TelnetBackend::~TelnetBackend() {
    stop();
}

bool TelnetBackend::isConnected() const {
    return m_socket != nullptr && m_socket->state() == QAbstractSocket::ConnectedState;
}

bool TelnetBackend::start(int cols, int rows) {
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
        // QTcpSocket has its own timeout, but it is measured in minutes on some
        // platforms. rules/net.md: every wait has a timeout, and it has to be
        // one the user would recognise as a timeout.
        if (!isConnected()) {
            fail(ErrorCode::ConnectFailed, tr("No answer from %1 after %2 seconds.")
                                               .arg(QString::fromStdString(m_config.host))
                                               .arg(m_config.connectTimeoutSeconds));
        }
    });

    m_retryTimer = new QTimer(this);  // owned by this
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, [this] { openSocket(); });

    openSocket();
    return true;
}

void TelnetBackend::openSocket() {
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
    m_negotiator = std::make_unique<telnet::Negotiator>(m_config.settings);
    m_socket = new QTcpSocket(this);  // owned by this
    connect(m_socket, &QTcpSocket::readyRead, this, &TelnetBackend::handleReadyRead);
    connect(m_socket, &QTcpSocket::connected, this, &TelnetBackend::handleConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &TelnetBackend::handleDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &TelnetBackend::handleSocketError);

    m_connectTimer->start(m_config.connectTimeoutSeconds * 1000);
    m_socket->connectToHost(QString::fromStdString(m_config.host),
                            static_cast<quint16>(m_config.port));
}

void TelnetBackend::handleConnected() {
    m_connectTimer->stop();
    m_everConnected = true;
    m_attempt = 0;
    m_config.settings.cols = m_cols;
    m_config.settings.rows = m_rows;
    m_reply.clear();
    // The opening offer goes out now and not in start(): sending before the
    // socket is up would be writing into a buffer that a failed connect throws
    // away, and the server would see a client that never negotiated.
    m_negotiator->start(&m_reply);
    flushReply();
    emit connected();
}

void TelnetBackend::handleReadyRead() {
    while (m_socket != nullptr && m_socket->bytesAvailable() > 0) {
        const QByteArray chunk = m_socket->read(kReadChunk);
        if (chunk.isEmpty()) {
            break;
        }
        m_data.clear();
        m_reply.clear();
        // The bytes are HOSTILE and go nowhere near the terminal until the
        // negotiator has taken the protocol out of them.
        m_negotiator->feed({reinterpret_cast<const std::uint8_t*>(chunk.constData()),
                            static_cast<std::size_t>(chunk.size())},
                           &m_data, &m_reply);
        flushReply();
        if (!m_data.empty()) {
            emit outputReceived(QByteArray(reinterpret_cast<const char*>(m_data.data()),
                                           static_cast<qsizetype>(m_data.size())));
        }
    }
}

void TelnetBackend::flushReply() {
    if (m_reply.empty() || m_socket == nullptr) {
        return;
    }
    m_socket->write(reinterpret_cast<const char*>(m_reply.data()),
                    static_cast<qint64>(m_reply.size()));
    m_reply.clear();
}

void TelnetBackend::writeInput(const QByteArray& bytes) {
    if (!isConnected() || bytes.isEmpty()) {
        return;
    }
    m_reply.clear();
    m_negotiator->encodeInput({reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                               static_cast<std::size_t>(bytes.size())},
                              &m_reply);
    flushReply();
}

void TelnetBackend::resize(int cols, int rows) {
    m_cols = cols;
    m_rows = rows;
    if (m_negotiator == nullptr) {
        return;
    }
    m_reply.clear();
    m_negotiator->resize(cols, rows, &m_reply);
    flushReply();
}

void TelnetBackend::handleDisconnected() {
    if (m_stopping) {
        return;
    }
    // A telnet server closing the connection POLITELY is how a telnet session
    // ends — there is no exit status to report, and calling it an error would
    // raise a banner every time someone typed `logout`. exited(0) is the honest
    // answer, and the reconnect policy correctly leaves a clean exit alone.
    //
    // A break that already raised an error is not that, and must not be
    // reported twice: handleSocketError has said what happened and, if the
    // policy allows, scheduled a retry.
    m_connectTimer->stop();
    if (m_sawError) {
        return;
    }
    if (m_everConnected) {
        emit exited(0);
        return;
    }
    fail(ErrorCode::ConnectFailed, tr("The server closed the connection immediately."));
}

void TelnetBackend::handleSocketError() {
    if (m_stopping || m_socket == nullptr) {
        return;
    }
    m_connectTimer->stop();
    if (m_socket->error() == QAbstractSocket::RemoteHostClosedError && m_everConnected) {
        // MEASURED, not assumed: Qt reports RemoteHostClosedError for a polite
        // FIN and for a reset alike. Both were run against the test server —
        // disconnectFromHost() and abort() — and neither the error code nor the
        // signal order tells them apart. Telnet has no logout message either,
        // so there is no layer at which this session could learn the
        // difference.
        //
        // So it is reported as a clean end, and that is the choice with the
        // better failure mode. The common case by far is the server ending the
        // session — logout, an idle timeout — and a banner on every normal
        // logout is precisely how a banner teaches people to ignore banners.
        // The cost is that a genuinely dropped telnet connection also reads as
        // a clean exit; the alternative cost was crying wolf every session.
        //
        // Reconnect therefore covers telnet's CONNECT failures, which are
        // identifiable, and not mid-session closes, which are not.
        return;
    }
    m_sawError = true;
    // The taxonomy, not the socket's wording: a banner needs a sentence about
    // the connection, and the socket's own text goes in the detail where a bug
    // report can use it.
    const ErrorCode code = m_everConnected ? ErrorCode::PeerClosed : ErrorCode::ConnectFailed;
    fail(code, m_socket->errorString());
}

void TelnetBackend::fail(ErrorCode code, const QString& message) {
    emit errorOccurred(errorCodeName(code), message);
    if (isRetryable(code) && m_config.maxReconnectAttempts > 0 &&
        m_attempt < m_config.maxReconnectAttempts) {
        scheduleReconnect();
    }
}

void TelnetBackend::scheduleReconnect() {
    ++m_attempt;
    const int delay = backoffDelayMs(m_attempt);
    emit reconnecting(m_attempt, m_config.maxReconnectAttempts, delay);
    m_everConnected = false;
    m_retryTimer->start(delay);
}

void TelnetBackend::stop() {
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

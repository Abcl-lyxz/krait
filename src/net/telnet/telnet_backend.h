#pragma once

#include "../error.h"
#include "../ibackend.h"
#include "telnet_negotiation.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class QTcpSocket;
class QTimer;

namespace krait::net {

struct TelnetConfig {
    std::string host;
    int port = 23;
    // Seconds. A telnet server that accepts the TCP connection and then says
    // nothing is indistinguishable from a slow one, so this bounds the connect
    // only — after that the session is simply idle, which is normal.
    int connectTimeoutSeconds = 15;
    // 0 disables reconnecting. Only failures isRetryable() allows are retried.
    int maxReconnectAttempts = 0;
    telnet::TelnetSettings settings;
};

// Telnet over QTcpSocket (plan T54).
//
// NO WORKER THREAD, unlike SshBackend — and that is not an inconsistency.
// libssh's API is blocking, so it needs a thread of its own; QTcpSocket is
// asynchronous, so every operation here already returns immediately and moving
// it to a thread would add a hand-off to solve a problem that does not exist.
// rules/net.md's rule is that blocking network calls never run on the UI
// thread, and this makes none.
//
// Everything protocol-shaped lives in telnet::Negotiator, which has no socket
// in it. This class is the part that cannot be unit-tested, and it is kept
// small enough that there is little in it to get wrong.
class TelnetBackend : public IBackend {
    Q_OBJECT

  public:
    explicit TelnetBackend(TelnetConfig config,
                           QObject* parent = nullptr);  // owned by parent
    ~TelnetBackend() override;

    bool start(int cols, int rows) override;
    void writeInput(const QByteArray& bytes) override;
    void resize(int cols, int rows) override;
    void stop() override;

    // True once the TCP connection is up. The telnet protocol has no notion of
    // "logged in", so this is as much as can honestly be said.
    bool isConnected() const;

  signals:
    // Same shape as SshBackend's, so the banner code does not care which
    // protocol raised it.
    void connected();
    void reconnecting(int attempt, int ofAttempts, int delayMs);

  private:
    void handleReadyRead();
    void handleConnected();
    void handleSocketError();
    void handleDisconnected();
    // Sends whatever the negotiator produced. Split out because every entry
    // point ends this way, and forgetting it means a negotiation that silently
    // never completes.
    void flushReply();
    void openSocket();
    void scheduleReconnect();
    void fail(ErrorCode code, const QString& message);

    TelnetConfig m_config;
    std::unique_ptr<telnet::Negotiator> m_negotiator;
    QTcpSocket* m_socket = nullptr;    // owned by this (QObject parent)
    QTimer* m_connectTimer = nullptr;  // owned by this
    QTimer* m_retryTimer = nullptr;    // owned by this
    int m_cols = 80;
    int m_rows = 24;
    int m_attempt = 0;
    bool m_stopping = false;
    // Whether handleSocketError already reported this cycle, so a break
    // is not announced twice — once as an error and again as a clean exit.
    bool m_sawError = false;
    // Whether the CURRENT cycle ever connected, so the retry counter resets
    // after a reconnect that worked rather than counting the whole session.
    bool m_everConnected = false;

    // Reused across reads so a steady stream allocates once. Cleared, not
    // destroyed, on each pass.
    std::vector<std::uint8_t> m_data;
    std::vector<std::uint8_t> m_reply;
};

}  // namespace krait::net

#pragma once

#include "error.h"
#include "ibackend.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

class QTcpSocket;
class QTimer;

namespace krait::net {

struct TcpConfig {
    std::string host;
    int port = 0;
    // Seconds. QTcpSocket has its own timeout, but it is measured in minutes on
    // some platforms; rules/net.md wants every wait bounded, and bounded at a
    // duration a user would recognise as a timeout.
    int connectTimeoutSeconds = 15;
    // 0 disables reconnecting. Only failures isRetryable() allows are retried.
    int maxReconnectAttempts = 0;
};

// The half of a TCP backend that is not the protocol: connect, read, write,
// time out, retry, and map failures onto the shared error taxonomy.
//
// Extracted when the SECOND one arrived and not before. rules/cpp.md bans an
// abstraction with a single implementation, and until raw sockets existed this
// was telnet's own code. With two, the alternative is the same socket lifecycle
// written twice — and every lifetime fix then has to be made twice, on the code
// path where getting it wrong means a use-after-free on remote input.
//
// NO WORKER THREAD, unlike SshBackend, and that is not an inconsistency:
// libssh's API is blocking so it needs a thread of its own, while QTcpSocket is
// asynchronous, so every call here already returns immediately. rules/net.md
// forbids blocking network calls on the UI thread; this makes none.
class TcpBackend : public IBackend {
    Q_OBJECT

  public:
    bool start(int cols, int rows) override;
    void writeInput(const QByteArray& bytes) override;
    void resize(int cols, int rows) override;
    void stop() override;

    // True once the TCP connection is up. Neither protocol above this has a
    // notion of "logged in", so it is as much as can honestly be said.
    bool isConnected() const;

  signals:
    // The same shape as SshBackend's, so the banner code does not care which
    // protocol raised it.
    void connected();
    void reconnecting(int attempt, int ofAttempts, int delayMs);

  protected:
    explicit TcpBackend(TcpConfig config, QObject* parent = nullptr);  // owned by parent
    ~TcpBackend() override;

    // Bytes off the wire. Terminal output goes to `data`, anything that must go
    // back to the server goes to `reply`. `data` reaches the VT parser
    // untouched, so a subclass that lets protocol bytes through has put them on
    // the user's screen.
    virtual void decode(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* data,
                        std::vector<std::uint8_t>* reply) = 0;

    // Keystrokes on the way out.
    virtual void encode(std::span<const std::uint8_t> bytes,
                        std::vector<std::uint8_t>* out) const = 0;

    // The socket just came up: emit any opening protocol. Default: none.
    virtual void handshake(std::vector<std::uint8_t>* reply);

    // The grid resized. Default: nothing to tell the far end.
    virtual void windowChanged(int cols, int rows, std::vector<std::uint8_t>* reply);

    // Called before EVERY connection attempt, including retries, so a stateful
    // codec starts each connection with no memory of the last one. A negotiated
    // option carried across a reconnect is an option the new server never
    // agreed to.
    virtual void resetCodec();

    int cols() const { return m_cols; }

    int rows() const { return m_rows; }

  private:
    void handleReadyRead();
    void handleConnected();
    void handleSocketError();
    void handleDisconnected();
    // Sends whatever a codec produced. Every entry point ends this way, and
    // forgetting it means a negotiation that silently never completes.
    void flushReply();
    void openSocket();
    void scheduleReconnect();
    void fail(ErrorCode code, const QString& message);

    TcpConfig m_config;
    QTcpSocket* m_socket = nullptr;    // owned by this (QObject parent)
    QTimer* m_connectTimer = nullptr;  // owned by this
    QTimer* m_retryTimer = nullptr;    // owned by this
    int m_cols = 80;
    int m_rows = 24;
    int m_attempt = 0;
    bool m_stopping = false;
    // Whether handleSocketError already reported this cycle, so a break is not
    // announced twice — once as an error and again as a clean exit.
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

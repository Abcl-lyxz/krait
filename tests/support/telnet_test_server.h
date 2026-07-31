#pragma once

#include <QByteArray>
#include <QObject>

#include <utility>

class QTcpServer;
class QTcpSocket;

namespace krait::test {

// A telnet server that exists to misbehave (plan T54).
//
// In-process and QTcpServer-based for the same reasons ssh_test_server.h gives
// for the SSH one: no Docker on a Windows runner, and a fixture that can be
// made to do the wrong thing on cue. A real telnetd is very good at the happy
// path and offers no way to ask it for an unterminated subnegotiation.
//
// Everything here runs on the test's own event loop — QTcpServer is
// asynchronous, so there is no thread and nothing to join.
class TelnetTestServer : public QObject {
    Q_OBJECT

  public:
    explicit TelnetTestServer(QObject* parent = nullptr);  // owned by parent
    ~TelnetTestServer() override;

    // Listens on loopback and returns the port the OS chose. 0 means failure.
    //
    // An EPHEMERAL port, unlike the SSH fixture's fixed 47222: QTcpServer can
    // report back what it bound and libssh's ssh_bind cannot. A fixed port is a
    // test that fails when something else on the machine happens to hold it.
    quint16 listen();

    // Bytes to send the moment a client connects, before anything it sends is
    // read. This is how a negotiation is scripted.
    void setGreeting(QByteArray greeting) { m_greeting = std::move(greeting); }

    // Drop the connection as soon as the client connects — peer-vanish, the
    // case every backend's reconnect policy is actually about.
    void setDropOnConnect(bool drop) { m_dropOnConnect = drop; }

    // Everything the client has sent, protocol and all.
    QByteArray received() const { return m_received; }

    // Sends `bytes` to the connected client. No-op when nobody is connected.
    void send(const QByteArray& bytes);

    // RESET. The peer vanishing — no FIN, no goodbye — which is the failure a
    // reconnect policy exists for.
    void dropClient();

    // A polite FIN, which is what a telnetd does when you type `logout`. The
    // pair of these is the only way to prove the backend tells them apart.
    void closeClientGracefully();

    bool hasClient() const { return m_client != nullptr; }

    int connectionCount() const { return m_connections; }

  signals:
    void clientConnected();
    void clientDataReceived();

  private:
    QTcpServer* m_server = nullptr;  // owned by this
    QTcpSocket* m_client = nullptr;  // owned by m_server
    QByteArray m_greeting;
    QByteArray m_received;
    bool m_dropOnConnect = false;
    int m_connections = 0;
};

}  // namespace krait::test

#include "telnet_test_server.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

namespace krait::test {

TelnetTestServer::TelnetTestServer(QObject* parent) : QObject(parent) {}

TelnetTestServer::~TelnetTestServer() = default;

quint16 TelnetTestServer::listen() {
    m_server = new QTcpServer(this);  // owned by this
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    connect(m_server, &QTcpServer::newConnection, this, [this] {
        QTcpSocket* socket = m_server->nextPendingConnection();
        if (socket == nullptr) {
            return;
        }
        ++m_connections;
        if (m_dropOnConnect) {
            // abort(), not close(): a graceful FIN is a clean end-of-session,
            // and the case worth testing is the one where the peer vanishes.
            socket->abort();
            socket->deleteLater();
            return;
        }
        m_client = socket;
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            m_received.append(socket->readAll());
            emit clientDataReceived();
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            if (m_client == socket) {
                m_client = nullptr;
            }
            socket->deleteLater();
        });
        if (!m_greeting.isEmpty()) {
            socket->write(m_greeting);
        }
        emit clientConnected();
    });
    return m_server->serverPort();
}

void TelnetTestServer::send(const QByteArray& bytes) {
    if (m_client != nullptr) {
        m_client->write(bytes);
        m_client->flush();
    }
}

void TelnetTestServer::dropClient() {
    if (m_client != nullptr) {
        QTcpSocket* socket = m_client;
        m_client = nullptr;
        socket->abort();
        socket->deleteLater();
    }
}

void TelnetTestServer::closeClientGracefully() {
    if (m_client != nullptr) {
        QTcpSocket* socket = m_client;
        m_client = nullptr;
        // disconnectFromHost, not abort: this sends a FIN and lets the client
        // see an orderly shutdown.
        socket->disconnectFromHost();
    }
}

}  // namespace krait::test

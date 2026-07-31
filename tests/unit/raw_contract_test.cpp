// T55: the IBackend contract for a raw socket.
//
// The interesting assertions here are all NEGATIVE. Raw mode's entire contract
// is that nothing is interpreted, so what matters is that the bytes telnet
// would have rewritten — 0xFF, a bare CR, anything that looks like IAC — come
// through untouched in both directions. A raw backend that helpfully escapes
// something is worse than no raw backend, because the tool exists precisely to
// show what is really on the wire.
//
// The server fixture is TelnetTestServer: it is a scriptable TCP listener with
// nothing telnet-specific in it, and a second copy under another name would be
// the same file twice.

#include "raw/raw_backend.h"
#include "telnet_test_server.h"
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QSignalSpy>
#include <QVariant>

#include <thread>

namespace {

void ensureApp() {
    if (QCoreApplication::instance() == nullptr) {
        static int argc = 1;
        static char name[] = "krait-tests";
        static char* argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
    }
}

template <typename Predicate>
bool pump(Predicate ready, int timeoutMs = 5000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!ready() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return ready();
}

QByteArray bytes(std::initializer_list<int> values) {
    QByteArray out;
    for (const int value : values) {
        out.append(static_cast<char>(value));
    }
    return out;
}

}  // namespace

TEST_CASE("raw passes every byte through in both directions", "[net][contract][raw]") {
    ensureApp();
    krait::test::TelnetTestServer server;
    const quint16 port = server.listen();
    REQUIRE(port != 0);

    krait::net::TcpConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    krait::net::RawBackend backend(config);

    QSignalSpy output(&backend, &krait::net::IBackend::outputReceived);
    QSignalSpy connected(&backend, &krait::net::RawBackend::connected);
    REQUIRE(backend.start(80, 24));
    REQUIRE(pump([&] { return connected.count() > 0; }));

    // Nothing is sent on connect. A raw backend that opened with a handshake
    // would be putting bytes on a wire the user is watching in order to find
    // out what is on it.
    CHECK(server.received().isEmpty());

    // Inbound: the exact bytes telnet would have eaten. 0xFF 0xFF stays two
    // bytes, and a lone 0xFF followed by what telnet reads as DO is data.
    const QByteArray hostile = bytes({0xFF, 0xFF, 0xFF, 0xFD, 0x18, '\r', 0x00, 'h', 'i'});
    server.send(hostile);
    REQUIRE(pump([&] {
        QByteArray seen;
        for (const QList<QVariant>& call : output) {
            seen.append(call.at(0).toByteArray());
        }
        return seen.size() >= hostile.size();
    }));
    QByteArray seen;
    for (const QList<QVariant>& call : output) {
        seen.append(call.at(0).toByteArray());
    }
    CHECK(seen == hostile);

    // Outbound: no doubling, and no CR NUL. Both are telnet's rules and both
    // would corrupt a binary protocol someone is poking at by hand.
    backend.writeInput(bytes({0xFF, '\r', 'x'}));
    REQUIRE(pump([&] { return server.received().size() >= 3; }));
    CHECK(server.received() == bytes({0xFF, '\r', 'x'}));

    backend.stop();
}

TEST_CASE("raw reports a refused connection and retries by policy", "[net][contract][raw]") {
    ensureApp();
    quint16 port = 0;
    {
        krait::test::TelnetTestServer probe;
        port = probe.listen();
        REQUIRE(port != 0);
    }

    krait::net::TcpConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.connectTimeoutSeconds = 2;
    config.maxReconnectAttempts = 2;
    krait::net::RawBackend backend(config);
    QSignalSpy errors(&backend, &krait::net::IBackend::errorOccurred);
    QSignalSpy retries(&backend, &krait::net::RawBackend::reconnecting);

    REQUIRE(backend.start(80, 24));
    REQUIRE(pump([&] { return errors.count() > 0; }));
    CHECK(errors.at(0).at(0).toString() == QStringLiteral("connect-failed"));
    REQUIRE(pump([&] { return retries.count() > 0; }));

    backend.stop();
    const auto seen = retries.count();
    pump([] { return false; }, 1500);
    CHECK(retries.count() == seen);
}

TEST_CASE("stop from another thread still cancels", "[net][contract][raw]") {
    // TerminalItem::resetSession tears a backend down on a thread pool, because
    // SshBackend::stop() can block. Everything in TcpBackend is a QObject
    // living on the GUI thread, and QTimer::stop() from another thread is
    // REFUSED silently — so without the hop in stop(), a pending reconnect
    // survives the tab being closed and reopens the connection under nobody.
    ensureApp();
    quint16 port = 0;
    {
        krait::test::TelnetTestServer probe;
        port = probe.listen();
        REQUIRE(port != 0);
    }

    krait::net::TcpConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.connectTimeoutSeconds = 2;
    config.maxReconnectAttempts = 5;
    krait::net::RawBackend backend(config);
    QSignalSpy retries(&backend, &krait::net::RawBackend::reconnecting);
    REQUIRE(backend.start(80, 24));
    REQUIRE(pump([&] { return retries.count() > 0; }));

    std::thread([&backend] { backend.stop(); }).join();
    // The hop is queued, so the event loop has to turn before it takes effect.
    pump([] { return false; }, 300);
    const auto seen = retries.count();
    pump([] { return false; }, 2500);
    CHECK(retries.count() == seen);
}

// T54: the IBackend contract for telnet, against a server built to misbehave.
//
// rules/net.md: "Every backend implements IBackend and passes the shared
// contract tests: connect, auth flows, half-close, peer-vanish, flood, reconnect
// policy, and error taxonomy mapping." Telnet has no auth of its own — whatever
// the far end asks for arrives as terminal output like anything else — so that
// row is absent rather than skipped.
//
// The negotiation itself is covered byte-for-byte in telnet_test.cpp with no
// socket involved. What is here is only what needs a real connection.

#include "telnet/telnet_backend.h"
#include "telnet_test_server.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QSignalSpy>
#include <QVariant>

namespace tn = krait::net::telnet;

namespace {

// Catch2 owns main(), so the application is made on first use and outlives the
// run. Same shape as ssh_contract_test.cpp's.
void ensureApp() {
    if (QCoreApplication::instance() == nullptr) {
        static int argc = 1;
        static char name[] = "krait-tests";
        static char* argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
    }
}

// Pumps the event loop until `ready` or the deadline. Returns what `ready`
// said last — never asserts, so a caller can test for the ABSENCE of something
// as well as its arrival.
//
// Every wait in this file is bounded (rules/net.md), and 5 s is chosen to be
// far longer than loopback needs while still failing a CI run rather than
// hanging it.
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

TEST_CASE("connect, negotiate, and carry data", "[net][contract][telnet]") {
    ensureApp();
    krait::test::TelnetTestServer server;
    const quint16 port = server.listen();
    REQUIRE(port != 0);
    // The server opens by asking for the two options a real telnetd asks for.
    server.setGreeting(
        bytes({tn::kIac, tn::kDo, tn::kTerminalType, tn::kIac, tn::kWill, tn::kSuppressGoAhead}));

    krait::net::TelnetConfig config;
    config.tcp.host = "127.0.0.1";
    config.tcp.port = port;
    krait::net::TelnetBackend backend(config);

    QSignalSpy output(&backend, &krait::net::IBackend::outputReceived);
    QSignalSpy connected(&backend, &krait::net::TelnetBackend::connected);
    REQUIRE(backend.start(80, 24));
    REQUIRE(pump([&] { return connected.count() > 0; }));

    // The greeting is protocol only: not one byte of it may reach the terminal.
    REQUIRE(pump([&] { return server.received().size() >= 12; }));
    CHECK(output.count() == 0);

    const QByteArray got = server.received();
    // Our opening offer, then the answers to the server's. The exact bytes are
    // asserted in telnet_test.cpp; here it is enough that a negotiation
    // happened at all and that NAWS carried the size start() was given.
    CHECK(got.contains(bytes({tn::kIac, tn::kWill, tn::kNaws})));
    // And NOT the size itself: this server never answered DO NAWS, so the
    // option is still being negotiated and RFC 1143 forbids using its effects.
    // Sending the window size to a server that has not agreed to receive it is
    // four bytes of terminal output. The agreed case is the next test.
    CHECK_FALSE(got.contains(bytes({tn::kIac, tn::kSb, tn::kNaws})));
    CHECK(got.contains(bytes({tn::kIac, tn::kWill, tn::kTerminalType})));
    CHECK(got.contains(bytes({tn::kIac, tn::kDo, tn::kSuppressGoAhead})));

    // Application data, with a doubled IAC in it, arrives as one literal byte.
    server.send(bytes({'h', 'i', tn::kIac, tn::kIac, '!'}));
    REQUIRE(pump([&] { return output.count() > 0; }));
    QByteArray seen;
    for (const QList<QVariant>& call : output) {
        seen.append(call.at(0).toByteArray());
    }
    CHECK(seen == bytes({'h', 'i', 0xFF, '!'}));

    backend.stop();
}

TEST_CASE("a resize sends NAWS once the option is agreed", "[net][contract][telnet]") {
    ensureApp();
    krait::test::TelnetTestServer server;
    const quint16 port = server.listen();
    REQUIRE(port != 0);
    server.setGreeting(bytes({tn::kIac, tn::kDo, tn::kNaws}));

    krait::net::TelnetConfig config;
    config.tcp.host = "127.0.0.1";
    config.tcp.port = port;
    krait::net::TelnetBackend backend(config);
    QSignalSpy connected(&backend, &krait::net::TelnetBackend::connected);
    REQUIRE(backend.start(80, 24));
    REQUIRE(pump([&] { return connected.count() > 0; }));
    REQUIRE(pump([&] { return !server.received().isEmpty(); }));

    backend.resize(132, 43);
    REQUIRE(pump([&] {
        return server.received().contains(
            bytes({tn::kIac, tn::kSb, tn::kNaws, 0, 132, 0, 43, tn::kIac, tn::kSe}));
    }));
    backend.stop();
}

TEST_CASE("input is escaped on the way out", "[net][contract][telnet]") {
    ensureApp();
    krait::test::TelnetTestServer server;
    const quint16 port = server.listen();
    REQUIRE(port != 0);

    krait::net::TelnetConfig config;
    config.tcp.host = "127.0.0.1";
    config.tcp.port = port;
    krait::net::TelnetBackend backend(config);
    QSignalSpy connected(&backend, &krait::net::TelnetBackend::connected);
    REQUIRE(backend.start(80, 24));
    REQUIRE(pump([&] { return connected.count() > 0; }));
    REQUIRE(pump([&] { return !server.received().isEmpty(); }));

    const qsizetype before = server.received().size();
    backend.writeInput(bytes({'l', 's', 0xFF, '\r'}));
    REQUIRE(pump([&] { return server.received().size() > before; }));
    // 0xFF doubled and CR given its NVT NUL — the server sees what RFC 854 says
    // it should, not what the user's keyboard produced.
    CHECK(server.received().mid(before) == bytes({'l', 's', 0xFF, 0xFF, '\r', 0x00}));
    backend.stop();
}

TEST_CASE("a close after connect is a clean exit, however it happened", "[net][contract][telnet]") {
    // BOTH closes, in one case, because the finding is that they are the same
    // to us. Qt reports RemoteHostClosedError for a polite FIN and for a reset
    // alike — measured, by running exactly these two paths — and telnet has no
    // logout message, so nothing at any layer here can tell them apart.
    //
    // Reported as a clean end because that is the better failure mode: the
    // common case is the server ending the session, and a banner on every
    // normal logout is how a banner teaches people to ignore banners. The
    // backend's comment records the cost of the choice.
    //
    // If this test ever starts failing on the abort() leg, Qt has gained a way
    // to distinguish them and the backend should start using it.
    ensureApp();
    const bool graceful = GENERATE(true, false);
    INFO("close was " << (graceful ? "a polite FIN" : "a reset"));

    krait::test::TelnetTestServer server;
    const quint16 port = server.listen();
    REQUIRE(port != 0);

    krait::net::TelnetConfig config;
    config.tcp.host = "127.0.0.1";
    config.tcp.port = port;
    krait::net::TelnetBackend backend(config);
    QSignalSpy errors(&backend, &krait::net::IBackend::errorOccurred);
    QSignalSpy exits(&backend, &krait::net::IBackend::exited);
    QSignalSpy connected(&backend, &krait::net::TelnetBackend::connected);
    REQUIRE(backend.start(80, 24));
    REQUIRE(pump([&] { return connected.count() > 0; }));

    if (graceful) {
        server.closeClientGracefully();
    } else {
        server.dropClient();
    }
    REQUIRE(pump([&] { return errors.count() > 0 || exits.count() > 0; }));
    CHECK(exits.count() == 1);
    CHECK(exits.at(0).at(0).toInt() == 0);
    CHECK(errors.count() == 0);
    backend.stop();
}

TEST_CASE("a refused connection reports connect-failed and retries by policy",
          "[net][contract][telnet]") {
    ensureApp();
    // Nothing is listening: bind a server, take its port, and let it go.
    quint16 port = 0;
    {
        krait::test::TelnetTestServer probe;
        port = probe.listen();
        REQUIRE(port != 0);
    }

    krait::net::TelnetConfig config;
    config.tcp.host = "127.0.0.1";
    config.tcp.port = port;
    config.tcp.connectTimeoutSeconds = 2;
    config.tcp.maxReconnectAttempts = 2;
    krait::net::TelnetBackend backend(config);
    QSignalSpy errors(&backend, &krait::net::IBackend::errorOccurred);
    QSignalSpy retries(&backend, &krait::net::TelnetBackend::reconnecting);

    REQUIRE(backend.start(80, 24));
    REQUIRE(pump([&] { return errors.count() > 0; }));
    CHECK(errors.at(0).at(0).toString() == QStringLiteral("connect-failed"));
    // Retryable, so the policy schedules one — and says so, because a terminal
    // that looks alive while it is not is the failure this reports around.
    REQUIRE(pump([&] { return retries.count() > 0; }));
    CHECK(retries.at(0).at(1).toInt() == 2);  // "of 2"

    // stop() cancels the pending retry: a closed tab must not reconnect.
    backend.stop();
    const auto seen = retries.count();
    pump([] { return false; }, 1500);
    CHECK(retries.count() == seen);
}

TEST_CASE("stop is idempotent and safe before start", "[net][contract][telnet]") {
    ensureApp();
    krait::net::TelnetConfig config;
    config.tcp.host = "127.0.0.1";
    config.tcp.port = 1;
    krait::net::TelnetBackend backend(config);
    // Cancel at every lifecycle stage (rules/net.md), including the one before
    // there is anything to cancel.
    backend.stop();
    backend.stop();
    CHECK_FALSE(backend.isConnected());
}

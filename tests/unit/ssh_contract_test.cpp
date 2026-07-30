#include "ssh/ssh_backend.h"
#include "ssh_test_server.h"
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>

#include <filesystem>
#include <string>

using namespace krait::net;
using krait::test::SshTestServer;

namespace {

// Queued signals need an event loop, and a backend that emits from a worker
// thread is exactly why. One per process; Catch2's main() gives us none.
void ensureApp() {
    if (QCoreApplication::instance() == nullptr) {
        static int argc = 1;
        static char name[] = "krait-tests";
        static char* argv[] = {name, nullptr};
        // Deliberately leaked: it has to outlive every case in the binary, and
        // tearing a QCoreApplication down mid-run is worse than leaking one.
        new QCoreApplication(argc, argv);
    }
}

// Spins the event loop until `done` or the timeout. Nothing here sleeps a fixed
// amount and hopes — that is how a suite becomes flaky on a busy machine.
template <typename Predicate>
bool waitFor(Predicate done, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

class KnownHosts {
  public:
    KnownHosts() : m_path((std::filesystem::temp_directory_path() / "krait-known-hosts").string()) {
        std::filesystem::remove(m_path);
    }

    ~KnownHosts() { std::filesystem::remove(m_path); }

    KnownHosts(const KnownHosts&) = delete;
    KnownHosts& operator=(const KnownHosts&) = delete;

    const std::string& path() const { return m_path; }

  private:
    std::string m_path;
};

SshConfig configFor(const SshTestServer& server, const KnownHosts& hosts) {
    SshConfig config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.user = "tester";
    config.knownHostsPath = hosts.path();
    config.auth = SshAuthPreference::Password;
    config.connectTimeoutSeconds = 5;
    config.keepaliveSeconds = 0;      // no keepalive noise in a short test
    config.maxReconnectAttempts = 0;  // per-test opt-in
    return config;
}

}  // namespace

TEST_CASE("connect, authenticate, echo, and stop", "[net][ssh][contract]") {
    ensureApp();
    SshTestServer server;
    REQUIRE(server.start({}));
    KnownHosts hosts;

    SshBackend backend(configFor(server, hosts), nullptr);
    IBackend& seam = backend;  // driven through the seam, like a real consumer

    QSignalSpy output(&seam, &IBackend::outputReceived);
    QSignalSpy errors(&seam, &IBackend::errorOccurred);
    QSignalSpy prompts(&backend, &SshBackend::hostKeyPrompt);
    QSignalSpy creds(&backend, &SshBackend::credentialPrompt);

    // Trust on first use, and answer the password prompt — the two places the
    // backend hands control to a human mid-connect.
    //
    // Qt::DirectConnection deliberately: it runs the answer INSIDE the emit, on
    // the worker thread, which is the worst-case ordering — the answer arrives
    // before the worker reaches its wait. That found a real bug the first time
    // this ran: waitForAnswer used to clear the answer flag after the emit, so a
    // prompt answer was thrown away and the connection hung for the full
    // five-minute timeout. The app's real path is queued through QML, where the
    // same race is just less likely rather than impossible.
    QObject::connect(&backend, &SshBackend::hostKeyPrompt, &backend,
                     [&backend](int, const QString&) { backend.respondHostKey(true); }, Qt::DirectConnection);
    QObject::connect(&backend, &SshBackend::credentialPrompt, &backend,
                     [&backend](const QString&, bool) {
                         backend.respondCredential(QStringLiteral("correct-horse"), false);
                     }, Qt::DirectConnection);

    REQUIRE(seam.start(80, 24));
    // Wait on EITHER outcome, then report the failure text. Waiting only on
    // success turns every connect bug into "timed out" with no reason attached.
    const bool settled = waitFor([&] { return backend.isConnected() || errors.count() > 0; });
    for (const auto& call : errors) {
        UNSCOPED_INFO("backend error [" << call.at(0).toString().toStdString()
                                        << "]: " << call.at(1).toString().toStdString());
    }
    UNSCOPED_INFO("host-key prompts: " << prompts.count()
                                       << ", credential prompts: " << creds.count());
    REQUIRE(settled);
    REQUIRE(errors.isEmpty());
    REQUIRE(backend.isConnected());
    REQUIRE(waitFor([&] { return output.count() > 0; }));

    QString banner;
    for (const auto& call : output) {
        banner += QString::fromUtf8(call.at(0).toByteArray());
    }
    CHECK(banner.contains(QStringLiteral("krait-test-server ready")));

    // The write path: bytes handed to the backend on THIS thread have to cross
    // into the worker's queue and reach the wire. The server echoes, so both
    // directions are proved at once.
    seam.writeInput(QByteArrayLiteral("hello\r"));
    REQUIRE(waitFor([&] { return server.received().find("hello\r") != std::string::npos; }));

    seam.resize(120, 40);  // must not throw or wedge the pump
    CHECK(errors.isEmpty());

    seam.stop();
    seam.stop();  // idempotent
    CHECK_FALSE(backend.isConnected());
    server.stop();
}

TEST_CASE("a refused password lands on auth-failed, not a retry", "[net][ssh][contract]") {
    ensureApp();
    SshTestServer server;
    SshTestServer::Options options;
    options.refuseAuth = true;
    REQUIRE(server.start(options));
    KnownHosts hosts;

    SshConfig config = configFor(server, hosts);
    config.maxReconnectAttempts = 3;  // even so, an auth failure must not retry
    SshBackend backend(config, nullptr);

    QSignalSpy errors(&backend, &IBackend::errorOccurred);
    QSignalSpy retries(&backend, &SshBackend::reconnecting);
    QObject::connect(&backend, &SshBackend::hostKeyPrompt, &backend,
                     [&backend](int, const QString&) { backend.respondHostKey(true); }, Qt::DirectConnection);
    QObject::connect(&backend, &SshBackend::credentialPrompt, &backend,
                     [&backend](const QString&, bool) {
                         backend.respondCredential(QStringLiteral("wrong"), false);
                     }, Qt::DirectConnection);

    REQUIRE(backend.start(80, 24));
    REQUIRE(waitFor([&] { return errors.count() > 0; }));
    CHECK(errors.at(0).at(0).toString() == QStringLiteral("auth-failed"));
    // The reason isRetryable excludes it: five retries of a wrong password is
    // how an account gets locked out.
    CHECK(retries.isEmpty());

    backend.stop();
    server.stop();
}

TEST_CASE("a changed host key blocks and never reconnects", "[net][ssh][contract]") {
    ensureApp();
    KnownHosts hosts;

    {
        // First contact: accept the key, which writes it to known_hosts.
        SshTestServer server;
        REQUIRE(server.start({}));
        SshBackend backend(configFor(server, hosts), nullptr);
        QObject::connect(&backend, &SshBackend::hostKeyPrompt, &backend,
                         [&backend](int, const QString&) { backend.respondHostKey(true); }, Qt::DirectConnection);
        QObject::connect(&backend, &SshBackend::credentialPrompt, &backend,
                         [&backend](const QString&, bool) {
                             backend.respondCredential(QStringLiteral("correct-horse"), false);
                         });
        REQUIRE(backend.start(80, 24));
        REQUIRE(waitFor([&] { return backend.isConnected(); }));
        backend.stop();
        server.stop();
    }

    // Same host and port, different key. This is the scenario net.md treats as
    // an attack until proven otherwise.
    SshTestServer server;
    SshTestServer::Options rotated;
    rotated.rotateHostKey = true;
    REQUIRE(server.start(rotated));

    SshConfig config = configFor(server, hosts);
    config.maxReconnectAttempts = 3;
    SshBackend backend(config, nullptr);

    QSignalSpy errors(&backend, &IBackend::errorOccurred);
    QSignalSpy prompts(&backend, &SshBackend::hostKeyPrompt);
    QSignalSpy retries(&backend, &SshBackend::reconnecting);
    // Answering "yes" must not matter. There is no answer that continues.
    QObject::connect(&backend, &SshBackend::hostKeyPrompt, &backend,
                     [&backend](int, const QString&) { backend.respondHostKey(true); }, Qt::DirectConnection);

    REQUIRE(backend.start(80, 24));
    REQUIRE(waitFor([&] { return errors.count() > 0; }));

    CHECK(errors.at(0).at(0).toString() == QStringLiteral("host-key-changed"));
    CHECK_FALSE(backend.isConnected());
    // Retrying would be retrying INTO the thing the banner is warning about.
    CHECK(retries.isEmpty());

    // The prompt still fires, because the user is owed the fingerprint and the
    // picture even when there is nothing to decide.
    REQUIRE(prompts.count() == 1);
    CHECK(prompts.at(0).at(0).toInt() == static_cast<int>(HostKeyState::Changed));
    CHECK(prompts.at(0).at(1).toString().contains(QStringLiteral("SHA256")));

    backend.stop();
    server.stop();
}

TEST_CASE("a peer that vanishes is retried, with the numbers said out loud",
          "[net][ssh][contract]") {
    ensureApp();
    SshTestServer server;
    SshTestServer::Options options;
    options.dropAfterShell = true;
    REQUIRE(server.start(options));
    KnownHosts hosts;

    SshConfig config = configFor(server, hosts);
    config.maxReconnectAttempts = 2;
    SshBackend backend(config, nullptr);

    QSignalSpy errors(&backend, &IBackend::errorOccurred);
    QSignalSpy exits(&backend, &IBackend::exited);
    QSignalSpy retries(&backend, &SshBackend::reconnecting);
    QObject::connect(&backend, &SshBackend::hostKeyPrompt, &backend,
                     [&backend](int, const QString&) { backend.respondHostKey(true); }, Qt::DirectConnection);
    QObject::connect(&backend, &SshBackend::credentialPrompt, &backend,
                     [&backend](const QString&, bool) {
                         backend.respondCredential(QStringLiteral("correct-horse"), false);
                     }, Qt::DirectConnection);

    REQUIRE(backend.start(80, 24));
    REQUIRE(waitFor([&] { return retries.count() > 0; }, 25000));

    // The banner needs all three numbers to say "reconnecting in 1 s (1 of 2)".
    CHECK(retries.at(0).at(0).toInt() == 1);
    CHECK(retries.at(0).at(1).toInt() == 2);
    CHECK(retries.at(0).at(2).toInt() == 1000);
    CHECK_FALSE(errors.isEmpty());
    // A vanished peer is NOT an exit. Telling someone their shell exited when
    // the network dropped is how a banner trains people to ignore banners.
    CHECK(exits.isEmpty());

    backend.stop();  // must return promptly even mid-backoff
    server.stop();
}

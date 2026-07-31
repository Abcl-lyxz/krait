#include "error.h"
#include "ibackend.h"
#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>

using namespace krait::net;

namespace {

// The smallest thing that is a backend. It exists to prove the SEAM: everything
// below talks to `IBackend*` and never to this type, so the test fails if the
// signal contract stops living on the base class.
class FakeBackend : public IBackend {
    Q_OBJECT

  public:
    bool start(int cols, int rows) override {
        if (cols <= 0 || rows <= 0) {
            emit errorOccurred(errorCodeName(ErrorCode::SpawnFailed), QStringLiteral("no grid"));
            return false;
        }
        m_started = true;
        m_cols = cols;
        m_rows = rows;
        return true;
    }

    void writeInput(const QByteArray& bytes) override {
        if (m_started) {
            emit outputReceived(bytes.toUpper());
        }
    }

    void resize(int cols, int rows) override {
        m_cols = cols;
        m_rows = rows;
    }

    void stop() override {
        if (!m_started) {
            return;  // idempotent: net.md requires stop() survive a double call
        }
        m_started = false;
        ++m_stops;
        emit exited(0);
    }

    void dropConnection() { emit errorOccurred(errorCodeName(ErrorCode::PeerClosed), QString()); }

    int stops() const { return m_stops; }

    int cols() const { return m_cols; }

    int rows() const { return m_rows; }

  private:
    bool m_started = false;
    int m_cols = 0;
    int m_rows = 0;
    int m_stops = 0;
};

}  // namespace

TEST_CASE("a consumer drives a backend entirely through IBackend", "[net][contract]") {
    FakeBackend fake;
    IBackend& backend = fake;  // the only handle used from here on

    QSignalSpy output(&backend, &IBackend::outputReceived);
    QSignalSpy errors(&backend, &IBackend::errorOccurred);
    QSignalSpy exits(&backend, &IBackend::exited);

    REQUIRE(backend.start(80, 24));
    backend.writeInput(QByteArrayLiteral("ls"));

    REQUIRE(output.count() == 1);
    CHECK(output.at(0).at(0).toByteArray() == QByteArrayLiteral("LS"));
    CHECK(errors.isEmpty());

    backend.resize(120, 40);
    CHECK(fake.cols() == 120);
    CHECK(fake.rows() == 40);

    backend.stop();
    CHECK(exits.count() == 1);
}

TEST_CASE("stop is idempotent", "[net][contract]") {
    FakeBackend fake;
    IBackend& backend = fake;

    backend.stop();  // never started
    REQUIRE(fake.stops() == 0);

    REQUIRE(backend.start(80, 24));
    backend.stop();
    backend.stop();
    CHECK(fake.stops() == 1);
}

TEST_CASE("a failed start reports a taxonomy code, not free text", "[net][contract]") {
    FakeBackend fake;
    IBackend& backend = fake;
    QSignalSpy errors(&backend, &IBackend::errorOccurred);

    CHECK_FALSE(backend.start(0, 0));
    REQUIRE(errors.count() == 1);
    CHECK(errors.at(0).at(0).toString() == QStringLiteral("spawn-failed"));
}

TEST_CASE("every error code has a name, and peer-closed is not an exit", "[net][contract]") {
    // A code that falls through errorCodeName lands on "unknown", and a banner
    // saying "unknown" is a banner nobody can act on.
    for (const ErrorCode code : {ErrorCode::PtyCreateFailed, ErrorCode::SpawnFailed,
                                 ErrorCode::IoFailed, ErrorCode::PeerClosed}) {
        CHECK(errorCodeName(code) != QStringLiteral("unknown"));
    }

    FakeBackend fake;
    IBackend& backend = fake;
    QSignalSpy errors(&backend, &IBackend::errorOccurred);
    QSignalSpy exits(&backend, &IBackend::exited);

    fake.dropConnection();
    CHECK(errors.count() == 1);
    CHECK(errors.at(0).at(0).toString() == QStringLiteral("peer-closed"));
    // The distinction error.h exists to keep: a dropped peer is an error, a
    // shell that ran `exit` is not, and they must not arrive on the same signal.
    CHECK(exits.isEmpty());
}

#include "backend_contract_test.moc"

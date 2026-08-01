#include "ssh/algorithms.h"
#include "ssh/sftp.h"
#include "ssh_test_server.h"
#include <catch2/catch_test_macros.hpp>

#include <libssh/libssh.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using krait::net::Sftp;
using krait::net::SftpEntry;
using krait::test::SshTestServer;

namespace {

constexpr const char* kUser = "tester";
constexpr const char* kPassword = "correct-horse";

// Enough to need more than one 64 KiB chunk, which is the only way the
// chunking loop in Sftp::get/put is actually under test. One chunk exactly
// would pass every assertion here while an off-by-one on the second chunk sat
// undetected.
constexpr std::size_t kBigFileSize = std::size_t{200} * 1024;

// Deterministic bytes. A fixed LCG rather than <random>: when a byte comparison
// fails, the seed alone has to be enough to reproduce the exact file, and a
// generator seeded from a clock turns a real corruption bug into a flake nobody
// can chase.
std::vector<char> pseudoRandomBytes(std::size_t count, std::uint32_t seed) {
    std::vector<char> bytes(count);
    std::uint32_t state = seed;
    for (char& byte : bytes) {
        state = (state * 1664525U) + 1013904223U;
        byte = static_cast<char>((state >> 16) & 0xFFU);
    }
    return bytes;
}

void writeFile(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::vector<char> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A temp directory that removes itself. Same shape as KnownHosts in
// ssh_contract_test.cpp and for the same reason: the error_code overload of
// remove_all is noexcept, but nothing else in the destructor is, and an
// exception leaving a destructor terminates the whole run — turning a teardown
// hiccup into a suite with no results. A leftover temp directory is not worth
// that.
class TempDir {
  public:
    explicit TempDir(const std::string& name)
        : m_path(std::filesystem::temp_directory_path() / ("krait-sftp-" + name)) {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
        std::filesystem::create_directories(m_path, ignored);
    }

    ~TempDir() {
        try {
            std::error_code ignored;
            std::filesystem::remove_all(m_path, ignored);
        } catch (...) {  // NOLINT(bugprone-empty-catch): see above
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return m_path; }

  private:
    std::filesystem::path m_path;
};

// A real libssh client session against the fixture.
//
// Deliberately NOT SshBackend: these cases are about Sftp, and driving them
// through the backend would put the host-key UX, the credential ladder and a
// worker thread between the assertion and the thing it is asserting about.
// SSH_OPTIONS_KNOWNHOSTS points at a throwaway file because libssh reads
// known_hosts during key exchange to order the host-key algorithms — leaving it
// unset would make a test run touch the developer's real one.
class SshClient {
  public:
    SshClient() = default;

    ~SshClient() {
        if (m_session != nullptr) {
            ssh_disconnect(m_session);
            ssh_free(m_session);
        }
    }

    SshClient(const SshClient&) = delete;
    SshClient& operator=(const SshClient&) = delete;

    bool connect(int port, const std::filesystem::path& knownHosts) {
        m_session = ssh_new();
        if (m_session == nullptr) {
            return false;
        }
        const std::string hosts = knownHosts.string();
        const int portValue = port;
        // Short, and bounded like every other wait in this tree: a fixture that
        // hangs takes the whole ctest run with it.
        const long timeout = 5;
        // The product's own key-exchange list, not libssh's default. The
        // default leads with the ML-KEM hybrid that algorithms.h documents as
        // broken in this build ("Failed to construct client init buffer"), so a
        // test client on the defaults never gets past kex — and would be
        // testing a configuration Krait never ships.
        ssh_options_set(m_session, SSH_OPTIONS_KEY_EXCHANGE, krait::net::kKeyExchange);
        ssh_options_set(m_session, SSH_OPTIONS_HOST, "127.0.0.1");
        ssh_options_set(m_session, SSH_OPTIONS_PORT, &portValue);
        ssh_options_set(m_session, SSH_OPTIONS_USER, kUser);
        ssh_options_set(m_session, SSH_OPTIONS_KNOWNHOSTS, hosts.c_str());
        ssh_options_set(m_session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, hosts.c_str());
        ssh_options_set(m_session, SSH_OPTIONS_TIMEOUT, &timeout);

        if (ssh_connect(m_session) != SSH_OK) {
            m_error = ssh_get_error(m_session);
            return false;
        }
        if (ssh_userauth_password(m_session, nullptr, kPassword) != SSH_AUTH_SUCCESS) {
            m_error = ssh_get_error(m_session);
            return false;
        }
        return true;
    }

    ssh_session session() const { return m_session; }

    const std::string& error() const { return m_error; }

  private:
    ssh_session m_session = nullptr;
    std::string m_error;
};

// One case's world: a served directory, a local scratch directory, a running
// server, a connected client and an open Sftp.
//
// Member order is the teardown order reversed, and it matters: the Sftp has to
// let go of the session before the session is freed, and the server has to
// outlive the client that is still talking to it. The server binds a fixed port
// and serves one connection at a time, so a case that left one running would
// break the NEXT case rather than itself — hence one fixture per case, torn down
// with it.
class SftpFixture {
  public:
    // `options` arrives from the case so the hostile-server knobs can be set per
    // case; sftpRoot is filled in here because only the fixture knows where the
    // temp directory landed.
    explicit SftpFixture(const std::string& name, SshTestServer::Options options = {})
        : m_dir(name) {
        std::error_code ignored;
        std::filesystem::create_directories(remote(), ignored);
        std::filesystem::create_directories(local(), ignored);

        options.sftpRoot = remote().string();
        // Stage by stage, each keeping its own reason. "the fixture is not
        // ready" tells whoever reads the failure nothing about which of the
        // three steps stopped, and this is the part most likely to break.
        if (!m_server.start(std::move(options))) {
            m_why = "server did not start: " + m_server.error();
            return;
        }
        if (!m_client.connect(m_server.port(), m_dir.path() / "known-hosts")) {
            m_why = "client did not connect: " + m_client.error();
            return;
        }
        if (!m_sftp.open(m_client.session())) {
            m_why = "sftp did not open: " + m_sftp.lastError();
            return;
        }
        m_ok = true;
    }

    SftpFixture(const SftpFixture&) = delete;
    SftpFixture& operator=(const SftpFixture&) = delete;

    bool ok() const { return m_ok; }

    const std::string& why() const { return m_why; }

    Sftp& sftp() { return m_sftp; }

    // What the server serves. Tests seed it directly, which is what makes
    // "exactly these entries" and "exactly this size" assertable.
    std::filesystem::path remote() const { return m_dir.path() / "remote"; }

    // The other end of a transfer.
    std::filesystem::path local() const { return m_dir.path() / "local"; }

  private:
    TempDir m_dir;
    SshTestServer m_server;
    SshClient m_client;
    Sftp m_sftp;
    bool m_ok = false;
    std::string m_why;
};

// Fails the case with the reason attached rather than with a bare `false`.
void requireReady(const SftpFixture& fixture) {
    UNSCOPED_INFO("fixture: " << fixture.why());
    REQUIRE(fixture.ok());
}

}  // namespace

TEST_CASE("sftp: realpath answers with a path", "[net][ssh][sftp]") {
    SftpFixture fixture("realpath");
    requireReady(fixture);

    // "." is how a file panel asks where it landed, and it is the first thing
    // it does — an empty answer here is a panel with nowhere to start.
    std::string resolved;
    REQUIRE(fixture.sftp().realpath(".", &resolved));
    CHECK_FALSE(resolved.empty());
}

TEST_CASE("sftp: listDir returns exactly the entries, directories first", "[net][ssh][sftp]") {
    SftpFixture fixture("listdir");
    requireReady(fixture);

    writeFile(fixture.remote() / "beta.txt", {'b'});
    writeFile(fixture.remote() / "alpha.txt", {'a'});
    std::error_code ignored;
    std::filesystem::create_directory(fixture.remote() / "zeta-dir", ignored);

    std::vector<SftpEntry> entries;
    REQUIRE(fixture.sftp().listDir("/", &entries));

    // Three, not five: the fixture sends "." and ".." the way a real server
    // does, so their absence is the client's filter working rather than the
    // fixture never having offered them.
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].name == "zeta-dir");
    CHECK(entries[0].isDir);
    CHECK(entries[1].name == "alpha.txt");
    CHECK_FALSE(entries[1].isDir);
    CHECK(entries[2].name == "beta.txt");
    CHECK(entries[2].size == 1);
}

TEST_CASE("sftp: stat reports the exact size", "[net][ssh][sftp]") {
    SftpFixture fixture("stat");
    requireReady(fixture);

    // Not a round number: a size that happens to match a buffer boundary would
    // hide an off-by-one in the attribute encoding.
    const std::vector<char> bytes = pseudoRandomBytes(4097, 7);
    writeFile(fixture.remote() / "sized.bin", bytes);

    SftpEntry entry;
    REQUIRE(fixture.sftp().stat("/sized.bin", &entry));
    CHECK(entry.size == bytes.size());
    CHECK_FALSE(entry.isDir);
}

TEST_CASE("sftp: a put/get round trip is byte for byte identical", "[net][ssh][sftp]") {
    SftpFixture fixture("roundtrip");
    requireReady(fixture);

    const std::vector<char> bytes = pseudoRandomBytes(kBigFileSize, 0x5EED);
    const std::filesystem::path source = fixture.local() / "upload.bin";
    writeFile(source, bytes);

    REQUIRE(fixture.sftp().put(source.string(), "/round-trip.bin", {}));
    std::error_code failed;
    CHECK(std::filesystem::file_size(fixture.remote() / "round-trip.bin", failed) == bytes.size());

    const std::filesystem::path sink = fixture.local() / "download.bin";
    REQUIRE(fixture.sftp().get("/round-trip.bin", sink.string(), {}));

    const std::vector<char> got = readFile(sink);
    REQUIRE(got.size() == bytes.size());
    // Compared, not spot-checked. This is the milestone's acceptance criterion,
    // and a chunk written at the wrong offset produces a file of exactly the
    // right size — a size check would call that a pass.
    const auto [mine, theirs] = std::mismatch(got.begin(), got.end(), bytes.begin());
    if (mine != got.end()) {
        UNSCOPED_INFO("first differing byte at offset " << (mine - got.begin()));
    }
    CHECK(mine == got.end());
}

TEST_CASE("sftp: a missing remote file fails with a reason and leaves nothing behind",
          "[net][ssh][sftp]") {
    SftpFixture fixture("missing");
    requireReady(fixture);

    const std::filesystem::path sink = fixture.local() / "never-written.bin";
    CHECK_FALSE(fixture.sftp().get("/not-there.bin", sink.string(), {}));
    // A banner with no sentence in it is a banner nobody can act on.
    CHECK_FALSE(fixture.sftp().lastError().empty());
    // Not a cancellation: the two reach the user differently, and calling this
    // one "cancelled" would suppress the banner the user needs.
    CHECK_FALSE(fixture.sftp().cancelled());
    CHECK_FALSE(std::filesystem::exists(sink));
}

TEST_CASE("sftp: cancelling a download removes the partial file", "[net][ssh][sftp]") {
    SftpFixture fixture("cancel");
    requireReady(fixture);

    writeFile(fixture.remote() / "big.bin", pseudoRandomBytes(kBigFileSize, 3));

    const std::filesystem::path sink = fixture.local() / "partial.bin";
    int calls = 0;
    const bool ok =
        fixture.sftp().get("/big.bin", sink.string(), [&calls](std::uint64_t done, std::uint64_t) {
            ++calls;
            // Stop only once a chunk has landed,
            // so there IS a partial file to not
            // leave behind. Cancelling at zero
            // would pass without ever reaching
            // the cleanup path.
            return done == 0;
        });

    CHECK_FALSE(ok);
    CHECK(fixture.sftp().cancelled());
    CHECK(calls >= 2);
    // A half-written file with the right name is worse than no file at all,
    // because whatever reads it next cannot tell.
    CHECK_FALSE(std::filesystem::exists(sink));
}

TEST_CASE("sftp: progress never goes backwards and ends at the file size", "[net][ssh][sftp]") {
    SftpFixture fixture("progress");
    requireReady(fixture);

    const std::vector<char> bytes = pseudoRandomBytes(kBigFileSize, 11);
    writeFile(fixture.remote() / "progress.bin", bytes);

    std::vector<std::uint64_t> steps;
    std::uint64_t reportedTotal = 0;
    REQUIRE(fixture.sftp().get("/progress.bin", (fixture.local() / "progress.bin").string(),
                               [&steps, &reportedTotal](std::uint64_t done, std::uint64_t total) {
                                   steps.push_back(done);
                                   reportedTotal = total;
                                   return true;
                               }));

    // More than a first and a last call, which is what proves the loop ran more
    // than once for a file this size.
    REQUIRE(steps.size() > 2);
    // A bar that jumps backwards is a bar people stop believing.
    CHECK(std::is_sorted(steps.begin(), steps.end()));
    CHECK(steps.front() == 0);
    CHECK(steps.back() == bytes.size());
    // The total comes from an fstat on the open handle; 0 here would leave the
    // bar stuck at "unknown" for the whole transfer.
    CHECK(reportedTotal == bytes.size());
}

// --- Hostile servers ------------------------------------------------------
//
// rules/net.md: all remote input is hostile, and new message handling ships
// with malformed-input coverage. Everything below drives Sftp against a server
// that is answering wrong ON PURPOSE — the reason the fixture exists at all.

TEST_CASE("sftp: a name the server weaponised never reaches the listing", "[net][ssh][sftp]") {
    SshTestServer::Options options;
    // Each of these is a name no real server would send and every one of them
    // is a local path the panel would otherwise compose. The separators are
    // both of them: the panel runs on Windows, where a backslash walks out of
    // the download directory exactly as well as a slash does.
    options.injectNames = {
        "../escape",
        "..\\escape",
        "sub/dir",
        // Built rather than written as one literal: a hex escape swallows every
        // hex digit that follows it, so "\x07name" is a puzzle for the next
        // reader even though it happens to stop where it should here.
        std::string("bell") + '\x07' + "name",
        std::string("del") + '\x7f' + "name",
    };
    SftpFixture fixture("unsafe-names", options);
    requireReady(fixture);

    writeFile(fixture.remote() / "real.txt", {'r'});

    std::vector<SftpEntry> entries;
    REQUIRE(fixture.sftp().listDir("/", &entries));
    // The legitimate file arrived in the SAME reply as the five bad ones, and
    // it is still here. A filter that dropped the whole listing would satisfy
    // "no unsafe names present" while making the panel useless.
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().name == "real.txt");
}

TEST_CASE("sftp: dot and dot-dot never appear in a listing", "[net][ssh][sftp]") {
    SshTestServer::Options options;
    // The fixture already sends both the way a real server does. Injecting them
    // a second time says so out loud, and proves they are dropped by name
    // rather than by the position a real server happens to put them in.
    options.injectNames = {".", ".."};
    SftpFixture fixture("dots", options);
    requireReady(fixture);

    writeFile(fixture.remote() / "only.txt", {'o'});

    std::vector<SftpEntry> entries;
    REQUIRE(fixture.sftp().listDir("/", &entries));
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().name == "only.txt");
}

TEST_CASE("sftp: a flood of rejected names stops at the cap instead of spinning",
          "[net][ssh][sftp]") {
    SshTestServer::Options options;
    // Comfortably past Sftp::kMaxEntries, and not one of them is a name the
    // client will keep. That is the regression this case exists for: the cap
    // counts ITERATIONS, and a cap that counted KEPT entries would sit at zero
    // here while the server streamed names forever — on the one ssh worker
    // thread, which stop() joins. Not a slow listing: the whole app, hung by a
    // peer.
    options.floodRejectedNames = 70000;
    SftpFixture fixture("flood", options);
    requireReady(fixture);

    const auto started = std::chrono::steady_clock::now();
    std::vector<SftpEntry> entries;
    const bool ok = fixture.sftp().listDir("/", &entries);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    CHECK_FALSE(ok);
    // Refused, not truncated: a short listing that looks complete is how
    // someone concludes a file is not there.
    CHECK_FALSE(fixture.sftp().lastError().empty());
    // Specifically the CAP said no. Both of listDir's failure paths return
    // false with a message, so a client that choked on the first oversized
    // reply would pass every other assertion here while leaving the iteration
    // cap exactly as untested as it was before this case existed.
    UNSCOPED_INFO("error: " << fixture.sftp().lastError());
    CHECK(fixture.sftp().lastError().find(std::to_string(Sftp::kMaxEntries)) != std::string::npos);
    // The point of the case is that it FINISHED. Bounded so a regression shows
    // up as a failing assertion rather than as a ctest that never returns and a
    // CI job killed by its own timeout with nothing to read.
    UNSCOPED_INFO("listDir took " << elapsed.count() << " ms");
    CHECK(elapsed < std::chrono::seconds(10));
}

TEST_CASE("sftp: a listing that stops early fails instead of returning a short one",
          "[net][ssh][sftp]") {
    SshTestServer::Options options;
    options.readdirFailsEarly = true;
    SftpFixture fixture("readdir-fails", options);
    requireReady(fixture);

    writeFile(fixture.remote() / "one.txt", {'1'});
    writeFile(fixture.remote() / "two.txt", {'2'});

    // Seeded, so "the caller's vector was left alone" is provable rather than
    // assumed. The failure being tested is not a missing error — it is a
    // truncated listing handed back as a successful one.
    std::vector<SftpEntry> entries(1);
    entries.front().name = "untouched";

    CHECK_FALSE(fixture.sftp().listDir("/", &entries));
    CHECK_FALSE(fixture.sftp().lastError().empty());
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().name == "untouched");
}

TEST_CASE("sftp: a download the server cuts short fails and leaves no file", "[net][ssh][sftp]") {
    SshTestServer::Options options;
    options.truncateReads = true;
    SftpFixture fixture("truncated", options);
    requireReady(fixture);

    // Bigger than one chunk, so the size fstat promises and the bytes that
    // actually arrive genuinely disagree. At one chunk the server would have
    // told the truth by accident and this case would pass with the check gone.
    writeFile(fixture.remote() / "cut.bin", pseudoRandomBytes(kBigFileSize, 23));

    const std::filesystem::path sink = fixture.local() / "cut.bin";
    CHECK_FALSE(fixture.sftp().get("/cut.bin", sink.string(), {}));
    CHECK_FALSE(fixture.sftp().lastError().empty());
    // Nobody pressed anything. Calling this a cancellation would suppress the
    // banner that is the only sign the file is incomplete.
    CHECK_FALSE(fixture.sftp().cancelled());
    // The file the server half-delivered is gone, same as every other failure
    // path here: a partial file with the right name is worse than no file,
    // because whatever reads it next cannot tell.
    CHECK_FALSE(std::filesystem::exists(sink));
}

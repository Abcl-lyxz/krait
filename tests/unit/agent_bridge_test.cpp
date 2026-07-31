// T60: the bridge between libssh's agent client and the Windows OpenSSH agent.
//
// These run against a REAL named pipe with a fake agent behind it, not against
// a mocked transport. The whole point of the bridge is that the two transports
// are different — a test that stubbed the pipe out would be testing the part
// that was never in doubt.
//
// What is NOT covered, and is written down rather than implied: no test drives
// libssh all the way through an agent-signed authentication. Doing that needs
// a fake agent that can really sign, and the primitives it would need
// (ssh_pki_export_pubkey_blob and friends) live in pki.h, which libssh does not
// install. So the seam is asserted at the two ends that can be observed — the
// relay moves the bytes, and libssh accepts the socket.

#include "ssh/agent_bridge.h"
#include <catch2/catch_test_macros.hpp>

#include <libssh/libssh.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using krait::net::AgentBridge;

namespace {

using Bytes = std::vector<std::uint8_t>;

// draft-miller-ssh-agent, section 5.1.
constexpr std::uint8_t kRequestIdentities = 11;
constexpr std::uint8_t kIdentitiesAnswer = 12;

// A per-process pipe name: the cases must not collide with each other, and must
// never collide with the real agent.
std::string uniquePipeName(const char* suffix) {
    return R"(\\.\pipe\krait-test-agent-)" + std::to_string(::GetCurrentProcessId()) + "-" + suffix;
}

Bytes framed(const Bytes& payload) {
    const auto length = static_cast<std::uint32_t>(payload.size());
    Bytes out{static_cast<std::uint8_t>(length >> 24U), static_cast<std::uint8_t>(length >> 16U),
              static_cast<std::uint8_t>(length >> 8U), static_cast<std::uint8_t>(length)};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// SSH2_AGENT_IDENTITIES_ANSWER carrying zero keys: the type byte then a uint32
// count of 0. Enough to prove the relay without a fake agent that can sign.
Bytes emptyIdentitiesAnswer() {
    return framed(Bytes{kIdentitiesAnswer, 0, 0, 0, 0});
}

// A fake agent: creates the pipe, waits for one client, and answers whatever it
// is asked with a canned reply. `chunkSize` splits that reply across several
// writes, because a byte-mode pipe is allowed to do that and the relay has to
// reassemble it.
class FakeAgent {
  public:
    FakeAgent(const std::string& pipeName, Bytes reply, DWORD chunkSize)
        : m_reply(std::move(reply)), m_chunk(chunkSize) {
        const std::wstring wide(pipeName.begin(), pipeName.end());
        m_pipe = ::CreateNamedPipeW(wide.c_str(), PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096,
                                    0, nullptr);
        if (m_pipe != INVALID_HANDLE_VALUE) {
            m_thread = std::thread([this] { serve(); });
        }
    }

    ~FakeAgent() {
        if (m_pipe != INVALID_HANDLE_VALUE) {
            ::CancelIoEx(m_pipe, nullptr);
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_pipe != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_pipe);
        }
    }

    FakeAgent(const FakeAgent&) = delete;
    FakeAgent& operator=(const FakeAgent&) = delete;

    bool listening() const { return m_pipe != INVALID_HANDLE_VALUE; }

    // The request body the agent actually received, or empty if it never got
    // one. Read after the answer has come back, so the write is finished.
    Bytes request() const { return m_served.load() ? m_request : Bytes{}; }

    // Waits until whatever reply this agent had has been written. Needed by the
    // cases that destroy the agent on purpose: without it the destructor can
    // win the race and the test would be exercising "the agent vanished before
    // answering" rather than the malformed answer it meant to send.
    bool waitReplied(int milliseconds) const {
        for (int waited = 0; waited < milliseconds && !m_replied.load(); waited += 5) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return m_replied.load();
    }

  private:
    void serve() {
        // ERROR_PIPE_CONNECTED means the client beat us to it, which is a
        // connection and not a failure.
        if (::ConnectNamedPipe(m_pipe, nullptr) == 0 && ::GetLastError() != ERROR_PIPE_CONNECTED) {
            return;
        }
        std::uint8_t header[4] = {};
        DWORD got = 0;
        if (::ReadFile(m_pipe, header, 4, &got, nullptr) == 0 || got != 4) {
            return;
        }
        const std::uint32_t length = (static_cast<std::uint32_t>(header[0]) << 24U) |
                                     (static_cast<std::uint32_t>(header[1]) << 16U) |
                                     (static_cast<std::uint32_t>(header[2]) << 8U) |
                                     static_cast<std::uint32_t>(header[3]);
        // Looped, like the bridge's own reads: a byte-mode pipe may hand back
        // less than asked for, and trusting one ReadFile here would make the
        // byte-for-byte assertion in the round-trip case flake rather than fail.
        Bytes body(length);
        for (std::uint32_t filled = 0; filled < length;) {
            if (::ReadFile(m_pipe, body.data() + filled, length - filled, &got, nullptr) == 0 ||
                got == 0) {
                return;
            }
            filled += got;
        }
        m_request = body;
        m_served.store(true);

        for (std::size_t at = 0; at < m_reply.size();) {
            const auto want =
                static_cast<DWORD>(std::min<std::size_t>(m_chunk, m_reply.size() - at));
            DWORD put = 0;
            if (::WriteFile(m_pipe, m_reply.data() + at, want, &put, nullptr) == 0 || put == 0) {
                return;
            }
            at += put;
        }
        m_replied.store(true);
    }

    Bytes m_reply;
    DWORD m_chunk;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    std::thread m_thread;
    Bytes m_request;
    std::atomic<bool> m_served{false};
    std::atomic<bool> m_replied{false};
};

// Writes on the socket the bridge would have handed libssh. This stands in for
// libssh's atomicio, which does exactly this with the same framing.
bool writeAll(std::uintptr_t socket, const Bytes& bytes) {
    const auto handle = static_cast<SOCKET>(socket);
    int done = 0;
    while (done < static_cast<int>(bytes.size())) {
        const int put = ::send(handle, reinterpret_cast<const char*>(bytes.data()) + done,
                               static_cast<int>(bytes.size()) - done, 0);
        if (put <= 0) {
            return false;
        }
        done += put;
    }
    return true;
}

// Returns fewer bytes than asked for when the peer closes, which is how the
// refusal cases are told apart from the working ones.
Bytes readUpTo(std::uintptr_t socket, int count) {
    const auto handle = static_cast<SOCKET>(socket);
    Bytes out;
    while (static_cast<int>(out.size()) < count) {
        std::uint8_t chunk[512] = {};
        const int want =
            std::min<int>(static_cast<int>(sizeof(chunk)), count - static_cast<int>(out.size()));
        const int got = ::recv(handle, reinterpret_cast<char*>(chunk), want, 0);
        if (got <= 0) {
            break;
        }
        out.insert(out.end(), chunk, chunk + got);
    }
    return out;
}

}  // namespace

TEST_CASE("a request and its answer cross the bridge unchanged", "[agent]") {
    const std::string name = uniquePipeName("roundtrip");
    FakeAgent agent(name, emptyIdentitiesAnswer(), 4096);
    REQUIRE(agent.listening());

    AgentBridge bridge;
    REQUIRE(bridge.start(name));

    REQUIRE(writeAll(bridge.socket(), framed(Bytes{kRequestIdentities})));

    const Bytes answer =
        readUpTo(bridge.socket(), static_cast<int>(emptyIdentitiesAnswer().size()));
    CHECK(answer == emptyIdentitiesAnswer());
    // And the agent saw the request byte-for-byte rather than a re-framed
    // version of it: the relay must not rewrite what it carries.
    CHECK(agent.request() == Bytes{kRequestIdentities});
}

TEST_CASE("a reply split across several pipe writes is reassembled", "[agent]") {
    // A byte-mode pipe — the mode the real agent creates — may hand back any
    // number of bytes per read. Reading once and trusting the count is the bug
    // this asserts against.
    const std::string name = uniquePipeName("chunked");
    FakeAgent agent(name, emptyIdentitiesAnswer(), 1);
    REQUIRE(agent.listening());

    AgentBridge bridge;
    REQUIRE(bridge.start(name));
    REQUIRE(writeAll(bridge.socket(), framed(Bytes{kRequestIdentities})));

    const Bytes answer =
        readUpTo(bridge.socket(), static_cast<int>(emptyIdentitiesAnswer().size()));
    CHECK(answer == emptyIdentitiesAnswer());
}

TEST_CASE("no agent is a refusal, not a crash", "[agent]") {
    AgentBridge bridge;
    // A pipe nobody created. This is the ordinary case on a machine where the
    // agent service was never started, so it has to be quiet about it.
    CHECK_FALSE(bridge.start(uniquePipeName("absent")));
    CHECK_FALSE(bridge.error().empty());
    CHECK(bridge.socket() == AgentBridge::kNoSocket);
}

TEST_CASE("an oversized length prefix is refused rather than allocated", "[agent]") {
    const std::string name = uniquePipeName("huge");
    FakeAgent agent(name, emptyIdentitiesAnswer(), 4096);
    REQUIRE(agent.listening());

    AgentBridge bridge;
    REQUIRE(bridge.start(name));

    // 0xFFFFFFFF bytes. Nothing legitimate sends this; a bug or a hostile local
    // process does, and the answer must be to stop rather than reserve 4 GB.
    REQUIRE(writeAll(bridge.socket(), Bytes{0xFF, 0xFF, 0xFF, 0xFF, kRequestIdentities}));

    // The relay gives up, which reaches this side as a closed socket.
    CHECK(readUpTo(bridge.socket(), 1).empty());
}

TEST_CASE("a zero-length message is refused", "[agent]") {
    // Length 0 leaves no room for the type byte every agent message has. The
    // protocol never produces one, so accepting it would only be a way to make
    // the relay spin on nothing.
    const std::string name = uniquePipeName("empty");
    FakeAgent agent(name, emptyIdentitiesAnswer(), 4096);
    REQUIRE(agent.listening());

    AgentBridge bridge;
    REQUIRE(bridge.start(name));
    REQUIRE(writeAll(bridge.socket(), Bytes{0, 0, 0, 0}));
    CHECK(readUpTo(bridge.socket(), 1).empty());
}

TEST_CASE("cancel releases a relay waiting on an agent that never answers", "[agent]") {
    // The defect this exists to prevent. libssh's agent client blocks in recv()
    // on the socket it was handed, so an agent that takes its time — a FIDO2
    // key waiting for a touch, a service that has wedged — parks the SSH worker
    // thread inside ssh_userauth_agent. Closing a tab joins that worker, from
    // the GUI thread. Nothing in the auth ladder polls a shutdown flag while it
    // is down there, so cancel() ending the relay is the only thing that
    // reaches it.
    //
    // An empty reply is a FakeAgent that reads the request, keeps the pipe
    // open, and says nothing — which is what "wedged" looks like from here.
    const std::string name = uniquePipeName("stall");
    FakeAgent agent(name, Bytes{}, 4096);
    REQUIRE(agent.listening());

    AgentBridge bridge;
    REQUIRE(bridge.start(name));
    const std::uintptr_t clientEnd = bridge.socket();
    REQUIRE(writeAll(clientEnd, framed(Bytes{kRequestIdentities})));

    // On another thread, because that is the shape of the bug: the blocked read
    // is on the SSH worker and the cancel comes from the GUI.
    auto reading = std::async(std::launch::async, [clientEnd] { readUpTo(clientEnd, 1); });
    bridge.cancel();
    const bool released = reading.wait_for(std::chrono::seconds(5)) == std::future_status::ready;

    // Before the CHECK: on failure this is what lets the reader finish, so a
    // failing assertion reports rather than hanging the suite in ~future.
    bridge.stop();
    CHECK(released);
}

TEST_CASE("a cancel that arrives while starting is not swallowed", "[agent]") {
    // start() used to clear the stop flag on its way out, which erased a
    // cancel() that landed while it was still opening the pipe — and openPipe
    // can sit in WaitNamedPipeW for a second. The relay would then be running
    // with nobody able to stop it, and the GUI thread would hang in the join
    // that follows. So a cancel before the relay exists has to make start()
    // refuse rather than be forgotten.
    const std::string name = uniquePipeName("cancel-race");
    FakeAgent agent(name, emptyIdentitiesAnswer(), 4096);
    REQUIRE(agent.listening());

    AgentBridge bridge;
    bridge.cancel();
    CHECK_FALSE(bridge.start(name));
    CHECK(bridge.socket() == AgentBridge::kNoSocket);

    // And stop() clears it again, so the next connection of a reconnecting
    // backend still gets an agent — one AgentBridge serves every cycle, and a
    // flag left set would leave the second connection with no agent at all.
    //
    // A second fixture, because the first one serves a single connection and
    // the refused start above already consumed it.
    bridge.stop();
    const std::string second = uniquePipeName("cancel-race-again");
    FakeAgent restarted(second, emptyIdentitiesAnswer(), 4096);
    REQUIRE(restarted.listening());
    CHECK(bridge.start(second));
    CHECK(bridge.socket() != AgentBridge::kNoSocket);
}

TEST_CASE("a bad length from the pipe is refused on the length alone", "[agent]") {
    // The pipe side is the untrusted one — the socket side is our own libssh,
    // while whatever answers the pipe is only ASSUMED to be the agent. These
    // two need nothing further to decide: the length itself is impossible.
    struct Case {
        const char* name;
        Bytes reply;
    };

    const Case cases[] = {
        {"zero-length", Bytes{0, 0, 0, 0}},
        {"over the cap", Bytes{0x00, 0x04, 0x00, 0x01, kIdentitiesAnswer}},  // 256 KiB + 1
    };

    for (const Case& one : cases) {
        INFO(one.name);
        const std::string name = uniquePipeName(one.name);
        FakeAgent agent(name, one.reply, 4096);
        REQUIRE(agent.listening());

        AgentBridge bridge;
        REQUIRE(bridge.start(name));
        REQUIRE(writeAll(bridge.socket(), framed(Bytes{kRequestIdentities})));
        // The relay gives up, which reaches this side as a closed socket.
        CHECK(readUpTo(bridge.socket(), 1).empty());
    }
}

TEST_CASE("an agent that dies mid-reply does not leave the relay hanging", "[agent]") {
    // A reply whose header promises more than arrives. The relay CANNOT refuse
    // these on the length: a byte-mode pipe is allowed to deliver a reply in
    // pieces, so waiting for the rest is the correct thing to do right up until
    // the agent goes away — which is what happens here, when the fixture is
    // destroyed and takes its pipe with it, exactly as a crashed agent would.
    struct Case {
        const char* name;
        Bytes reply;
    };

    const Case cases[] = {
        {"header promises five, sends none", Bytes{0, 0, 0, 5}},
        {"body cut short", Bytes{0, 0, 0, 5, kIdentitiesAnswer, 0}},
        {"header itself cut short", Bytes{0, 0}},
    };

    for (const Case& one : cases) {
        INFO(one.name);
        const std::string name = uniquePipeName(one.name);
        AgentBridge bridge;
        {
            FakeAgent agent(name, one.reply, 4096);
            REQUIRE(agent.listening());
            REQUIRE(bridge.start(name));
            REQUIRE(writeAll(bridge.socket(), framed(Bytes{kRequestIdentities})));
            // Waited for, so this is a HALF reply rather than a fixture that
            // never got as far as writing one — otherwise the case would be
            // testing "the agent vanished", which is a different test.
            CHECK(agent.waitReplied(5000));
        }
        CHECK(readUpTo(bridge.socket(), 1).empty());
    }
}

TEST_CASE("stopping is safe before starting and after stopping", "[agent]") {
    AgentBridge bridge;
    bridge.stop();
    CHECK(bridge.socket() == AgentBridge::kNoSocket);

    const std::string name = uniquePipeName("stop");
    FakeAgent agent(name, emptyIdentitiesAnswer(), 4096);
    REQUIRE(agent.listening());
    REQUIRE(bridge.start(name));
    bridge.stop();
    bridge.stop();
    CHECK(bridge.socket() == AgentBridge::kNoSocket);
}

TEST_CASE("libssh accepts the bridge socket as its agent", "[agent]") {
    // The other end of the seam. ssh_set_agent_socket() is what makes libssh's
    // ssh_agent_is_running() true — it checks only that the agent socket HAS an
    // fd — and from there libssh talks its normal agent protocol down it. If
    // this ever stops returning SSH_OK the bridge is relaying to nobody.
    const std::string name = uniquePipeName("libssh");
    FakeAgent agent(name, emptyIdentitiesAnswer(), 4096);
    REQUIRE(agent.listening());

    AgentBridge bridge;
    REQUIRE(bridge.start(name));

    ssh_session session = ssh_new();
    REQUIRE(session != nullptr);
    CHECK(ssh_set_agent_socket(session, static_cast<socket_t>(bridge.socket())) == SSH_OK);
    bridge.releaseSocket();  // ssh_free closes it from here on
    ssh_free(session);
}

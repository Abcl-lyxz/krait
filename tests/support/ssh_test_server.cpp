#include "ssh_test_server.h"

#include <libssh/libssh.h>
#include <libssh/server.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

namespace krait::test {

namespace {

// Stage tracing for the server thread, on when KRAIT_TEST_SERVER_TRACE is set.
// A hung contract test is otherwise a black box: the client log says the client
// finished, and nothing says which of the server's blocking calls it is parked
// in.
void trace(const char* stage) {
    // _dupenv_s rather than getenv: MSVC deprecates the latter and warnings are
    // errors here, same as everywhere else in the tree.
    static const bool on = [] {
        char* value = nullptr;
        std::size_t size = 0;
        const bool present =
            _dupenv_s(&value, &size, "KRAIT_TEST_SERVER_TRACE") == 0 && value != nullptr;
        std::free(value);
        return present;
    }();
    if (on) {
        std::fprintf(stderr, "[test-server] %s\n", stage);
        std::fflush(stderr);
    }
}

// Wakes a thread parked in ssh_bind_accept by connecting to the port and
// hanging up. ssh_bind_set_blocking(bind, 0) does not make accept non-blocking
// on Windows — verified: the FIRST accept returns because a client is already
// arriving, and the second parks forever, so stop() joins a thread that never
// comes back. A self-connect is the portable way out: accept returns, the
// handshake fails, and the loop re-checks the shutdown flag.
//
// WSAStartup has already been done by libssh's own init by the time any of this
// runs; if it somehow has not, socket() fails and stop() is no worse off than
// it was.
void wakeAccept(int port) {
    const SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(static_cast<u_short>(port));
    ::InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr);
    ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::closesocket(sock);
}

// Fixed rather than ephemeral. libssh's ssh_bind gives no way to read back the
// port the OS chose, and a contract test has to know where to connect. The
// number is high and odd enough to be free on a developer box and a runner.
constexpr int kPort = 47222;

// How long a wait runs before the loop re-checks the shutdown flag. Every wait
// in this file is bounded, same rule as the production code.
constexpr int kAcceptTimeoutMs = 200;

}  // namespace

struct SshTestServer::Impl {
    ssh_bind bind = nullptr;
    ssh_session session = nullptr;
    ssh_channel channel = nullptr;
    SshTestServer::Options options;
    std::string hostKeyPath;
    mutable std::mutex mutex;
    std::string received;
};

SshTestServer::SshTestServer() : m_impl(std::make_unique<Impl>()) {}

SshTestServer::~SshTestServer() {
    stop();
}

bool SshTestServer::start(Options options) {
    m_impl->options = std::move(options);
    m_shutdown = false;
    m_error.clear();

    // A throwaway host key per run, in the temp dir. `rotateHostKey` gives it a
    // different name so the key genuinely differs from the previous run's — the
    // whole point of the changed-key test is that the bytes are not the same.
    const std::string suffix = m_impl->options.rotateHostKey ? "-rotated" : "";
    m_impl->hostKeyPath =
        (std::filesystem::temp_directory_path() / ("krait-test-hostkey" + suffix)).string();
    std::filesystem::remove(m_impl->hostKeyPath);

    ssh_key hostKey = nullptr;
    if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &hostKey) != SSH_OK) {
        m_error = "could not generate a host key";
        return false;
    }
    const int exported = ssh_pki_export_privkey_file(hostKey, nullptr, nullptr, nullptr,
                                                     m_impl->hostKeyPath.c_str());
    ssh_key_free(hostKey);
    if (exported != SSH_OK) {
        m_error = "could not write the host key";
        return false;
    }

    m_impl->bind = ssh_bind_new();
    if (m_impl->bind == nullptr) {
        m_error = "ssh_bind_new failed";
        return false;
    }
    const char* address = "127.0.0.1";
    int port = kPort;
    ssh_bind_options_set(m_impl->bind, SSH_BIND_OPTIONS_BINDADDR, address);
    ssh_bind_options_set(m_impl->bind, SSH_BIND_OPTIONS_BINDPORT, &port);
    ssh_bind_options_set(m_impl->bind, SSH_BIND_OPTIONS_HOSTKEY, m_impl->hostKeyPath.c_str());

    if (ssh_bind_listen(m_impl->bind) != SSH_OK) {
        m_error = ssh_get_error(m_impl->bind);
        ssh_bind_free(m_impl->bind);
        m_impl->bind = nullptr;
        return false;
    }
    m_port = kPort;
    m_thread = std::thread([this] { serve(); });
    return true;
}

void SshTestServer::serve() {
    while (!m_shutdown.load()) {
        ssh_session session = ssh_new();
        if (session == nullptr) {
            return;
        }
        // Blocking. stop() wakes it with a self-connect; see wakeAccept.
        if (ssh_bind_accept(m_impl->bind, session) != SSH_OK) {
            ssh_free(session);
            if (m_shutdown.load()) {
                return;
            }
            continue;
        }
        if (m_shutdown.load()) {
            // The wake-up connection, not a client.
            ssh_free(session);
            return;
        }
        // The accepted session inherits the bind's non-blocking mode, and every
        // call below is written as blocking. Put it back before the key
        // exchange, or ssh_handle_key_exchange returns SSH_AGAIN and the
        // connection is dropped as a failure.
        trace("accepted");
        ssh_set_blocking(session, 1);
        // ...but blocking with a BOUND. Without this, the teardown below
        // (ssh_channel_close writes a close packet) blocks forever once the
        // client has already gone, and stop() then joins a thread that will
        // never return. Same rule as the production code: every wait has a
        // timeout.
        long sessionTimeout = 2;
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &sessionTimeout);
        m_impl->session = session;

        trace("kex-start");
        if (ssh_handle_key_exchange(session) != SSH_OK) {
            ssh_disconnect(session);
            ssh_free(session);
            m_impl->session = nullptr;
            continue;
        }

        // Auth. Only "password" is offered, so the client's ladder has exactly
        // one rung it can use and the test knows which path it took.
        trace("auth-loop");
        bool authenticated = false;
        while (!authenticated && !m_shutdown.load()) {
            ssh_message message = ssh_message_get(session);
            if (message == nullptr) {
                break;
            }
            if (ssh_message_type(message) == SSH_REQUEST_AUTH &&
                ssh_message_subtype(message) == SSH_AUTH_METHOD_PASSWORD) {
                const char* user = ssh_message_auth_user(message);
                const char* password = ssh_message_auth_password(message);
                const bool ok = !m_impl->options.refuseAuth && user != nullptr &&
                                password != nullptr && m_impl->options.user == user &&
                                m_impl->options.password == password;
                if (ok) {
                    authenticated = true;
                    ssh_message_auth_reply_success(message, 0);
                } else {
                    ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                }
            } else {
                ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_PASSWORD);
                ssh_message_reply_default(message);
            }
            ssh_message_free(message);
        }
        if (!authenticated) {
            ssh_disconnect(session);
            ssh_free(session);
            m_impl->session = nullptr;
            continue;
        }

        // Channel, then pty, then shell — the same three the client asks for.
        trace("channel-loop");
        ssh_channel channel = nullptr;
        while (channel == nullptr && !m_shutdown.load()) {
            ssh_message message = ssh_message_get(session);
            if (message == nullptr) {
                break;
            }
            if (ssh_message_type(message) == SSH_REQUEST_CHANNEL_OPEN &&
                ssh_message_subtype(message) == SSH_CHANNEL_SESSION) {
                channel = ssh_message_channel_request_open_reply_accept(message);
            } else {
                ssh_message_reply_default(message);
            }
            ssh_message_free(message);
        }
        if (channel == nullptr) {
            ssh_disconnect(session);
            ssh_free(session);
            m_impl->session = nullptr;
            continue;
        }
        m_impl->channel = channel;

        trace("shell-loop");
        bool shell = false;
        while (!shell && !m_shutdown.load()) {
            ssh_message message = ssh_message_get(session);
            if (message == nullptr) {
                break;
            }
            if (ssh_message_type(message) == SSH_REQUEST_CHANNEL &&
                (ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_PTY ||
                 ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_SHELL)) {
                shell = ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_SHELL;
                ssh_message_channel_request_reply_success(message);
            } else {
                ssh_message_reply_default(message);
            }
            ssh_message_free(message);
        }

        if (m_impl->options.dropAfterShell) {
            // No EOF, no exit status, no disconnect message: the socket just
            // goes. That is what a peer vanishing looks like, and it has to
            // reach the user as something other than "the shell exited".
            ssh_silent_disconnect(session);
            ssh_free(session);
            m_impl->session = nullptr;
            m_impl->channel = nullptr;
            continue;
        }

        // A banner, so the client has something to render, then echo whatever
        // arrives. The echo is what proves the write path reached the wire.
        trace("echo-start");
        const std::string greeting = "krait-test-server ready\r\n";
        ssh_channel_write(channel, greeting.data(), static_cast<std::uint32_t>(greeting.size()));

        char buffer[1024];
        while (!m_shutdown.load() && ssh_channel_is_open(channel) != 0 &&
               ssh_channel_is_eof(channel) == 0) {
            const int n =
                ssh_channel_read_timeout(channel, buffer, sizeof(buffer), 0, kAcceptTimeoutMs);
            if (n == SSH_ERROR) {
                break;
            }
            if (n > 0) {
                {
                    const std::lock_guard lock(m_impl->mutex);
                    m_impl->received.append(buffer, static_cast<std::size_t>(n));
                }
                ssh_channel_write(channel, buffer, static_cast<std::uint32_t>(n));
            }
        }

        trace("close-channel");
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        m_impl->channel = nullptr;
        trace("disconnect");
        ssh_disconnect(session);
        trace("disconnected");
        ssh_free(session);
        trace("session-freed");
        m_impl->session = nullptr;
    }
    trace("serve-exit");
}

void SshTestServer::stop() {
    trace("stop-enter");
    m_shutdown = true;
    if (m_thread.joinable()) {
        wakeAccept(m_port);
        m_thread.join();
    }
    trace("stop-joined");
    if (m_impl->bind != nullptr) {
        ssh_bind_free(m_impl->bind);
        m_impl->bind = nullptr;
    }
    if (!m_impl->hostKeyPath.empty()) {
        std::filesystem::remove(m_impl->hostKeyPath);
        m_impl->hostKeyPath.clear();
    }
}

std::string SshTestServer::received() const {
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->received;
}

}  // namespace krait::test

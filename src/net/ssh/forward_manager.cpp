#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "forward_manager.h"

#include "socks5.h"
#include <winsock2.h>
#include <ws2tcpip.h>

#include <libssh/libssh.h>

#include <algorithm>
#include <array>

namespace krait::net {
namespace {

// One pass's worth in each direction. Small on purpose: this runs inside the
// shell's poll loop, and a large read here delays the terminal.
constexpr int kChunk = 16 * 1024;

// A tunnel that has queued this much unsent is a peer that has stopped reading.
// Same reasoning as TcpBackend's write cap: the queue is fed by the far end, so
// without a bound it is a remote allocation primitive.
constexpr std::size_t kMaxPending = 1 << 20;

void closeSocket(SOCKET& socket) {
    if (socket != INVALID_SOCKET) {
        ::closesocket(socket);
        socket = INVALID_SOCKET;
    }
}

// Non-blocking, because every socket here is serviced from a poll loop that
// must not stall the terminal.
bool setNonBlocking(SOCKET socket) {
    u_long mode = 1;
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
}

// Binds a listener. An EMPTY bind address means loopback and not INADDR_ANY:
// exposing a tunnel to the network has to be something the user wrote down.
// forwards.h leaves the field empty for the three-field spec precisely so this
// decision lives in one place.
SOCKET openListener(const std::string& bindAddress, int port, std::string* whyNot) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    const std::string service = std::to_string(port);
    const char* node = bindAddress.empty() ? "127.0.0.1" : bindAddress.c_str();
    addrinfo* resolved = nullptr;
    if (::getaddrinfo(node, service.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
        *whyNot = "cannot resolve " + std::string(node);
        return INVALID_SOCKET;
    }

    SOCKET listener = INVALID_SOCKET;
    for (addrinfo* at = resolved; at != nullptr; at = at->ai_next) {
        listener = ::socket(at->ai_family, at->ai_socktype, at->ai_protocol);
        if (listener == INVALID_SOCKET) {
            continue;
        }
        // Deliberately NOT SO_REUSEADDR. On Windows it lets a second process
        // steal a port that is already bound, which for a forward means someone
        // else's listener quietly receiving traffic meant for this tunnel.
        if (::bind(listener, at->ai_addr, static_cast<int>(at->ai_addrlen)) == 0 &&
            ::listen(listener, SOMAXCONN) == 0 && setNonBlocking(listener)) {
            break;
        }
        closeSocket(listener);
    }
    ::freeaddrinfo(resolved);

    if (listener == INVALID_SOCKET) {
        *whyNot = "port " + service + " is not available";
    }
    return listener;
}

// One live connection: a local socket paired with an SSH channel, plus the
// bytes in flight each way.
struct Tunnel {
    SOCKET socket = INVALID_SOCKET;
    ssh_channel channel = nullptr;
    std::size_t forwardIndex = 0;
    // Set for a dynamic forward until its SOCKS handshake completes; the
    // channel is not opened until the client has said where it wants to go.
    std::unique_ptr<socks5::Handshake> handshake;
    std::vector<std::uint8_t> toSocket;   // read from SSH, not yet written locally
    std::vector<std::uint8_t> toChannel;  // read locally, not yet written to SSH
    bool socketClosed = false;
};

struct Listener {
    SOCKET socket = INVALID_SOCKET;
    std::size_t forwardIndex = 0;
};

}  // namespace

struct ForwardManager::Impl {
    std::vector<TunnelStatus> statuses;
    std::vector<Listener> listeners;
    std::vector<Tunnel> tunnels;
    // Remote forwards are accepted from the SESSION rather than a socket, so
    // they are polled separately.
    bool anyRemote = false;
};

ForwardManager::ForwardManager() : m_impl(std::make_unique<Impl>()) {}

ForwardManager::~ForwardManager() {
    stop();
}

void ForwardManager::publish() {
    if (m_onStatus) {
        m_onStatus(m_impl->statuses);
    }
}

std::vector<TunnelStatus> ForwardManager::status() const {
    return m_impl->statuses;
}

void ForwardManager::start(void* sessionHandle, const std::vector<Forward>& forwards) {
    auto session = static_cast<ssh_session>(sessionHandle);
    m_impl->statuses.clear();
    m_impl->statuses.reserve(forwards.size());

    for (std::size_t index = 0; index < forwards.size(); ++index) {
        const Forward& forward = forwards[index];
        TunnelStatus status;
        status.forward = forward;

        if (forward.kind == ForwardKind::Remote) {
            // The SERVER listens. A bound port of 0 would mean "you choose",
            // which we never ask for: a tunnel whose port the user cannot
            // predict is a tunnel they cannot point anything at.
            int bound = 0;
            const char* address =
                forward.bindAddress.empty() ? nullptr : forward.bindAddress.c_str();
            if (ssh_channel_listen_forward(session, address, forward.bindPort, &bound) != SSH_OK) {
                status.state = TunnelState::Failed;
                // sshd refuses these when GatewayPorts is off and the bind
                // address is not loopback. Worth naming, because it otherwise
                // looks like a Krait failure.
                status.detail = "the server refused the remote forward";
            } else {
                status.state = TunnelState::Listening;
                m_impl->anyRemote = true;
            }
            m_impl->statuses.push_back(std::move(status));
            continue;
        }

        std::string whyNot;
        const SOCKET listener = openListener(forward.bindAddress, forward.bindPort, &whyNot);
        if (listener == INVALID_SOCKET) {
            // Per-tunnel, not fatal. The session is what the user asked for;
            // one port already in use must not cost them the shell.
            status.state = TunnelState::Failed;
            status.detail = whyNot;
        } else {
            status.state = TunnelState::Listening;
            m_impl->listeners.push_back({listener, index});
        }
        m_impl->statuses.push_back(std::move(status));
    }
    publish();
}

bool ForwardManager::service(void* sessionHandle) {
    auto session = static_cast<ssh_session>(sessionHandle);
    bool changed = false;

    // 1. New local connections.
    for (const Listener& listener : m_impl->listeners) {
        while (true) {
            SOCKET accepted = ::accept(listener.socket, nullptr, nullptr);
            if (accepted == INVALID_SOCKET) {
                break;  // WSAEWOULDBLOCK: nothing waiting, which is the norm
            }
            if (!setNonBlocking(accepted)) {
                closeSocket(accepted);
                continue;
            }
            Tunnel tunnel;
            tunnel.socket = accepted;
            tunnel.forwardIndex = listener.forwardIndex;

            const Forward& forward = m_impl->statuses[listener.forwardIndex].forward;
            if (forward.kind == ForwardKind::Dynamic) {
                // The channel waits until SOCKS says where to point it.
                tunnel.handshake = std::make_unique<socks5::Handshake>();
            } else {
                tunnel.channel = ssh_channel_new(session);
                if (tunnel.channel == nullptr ||
                    ssh_channel_open_forward(tunnel.channel, forward.destHost.c_str(),
                                             forward.destPort, "127.0.0.1",
                                             forward.bindPort) != SSH_OK) {
                    if (tunnel.channel != nullptr) {
                        ssh_channel_free(tunnel.channel);
                        tunnel.channel = nullptr;
                    }
                    closeSocket(tunnel.socket);
                    continue;
                }
            }
            ++m_impl->statuses[listener.forwardIndex].totalConnections;
            m_impl->tunnels.push_back(std::move(tunnel));
            changed = true;
        }
    }

    // 2. Remote forwards the SERVER opened towards us.
    if (m_impl->anyRemote) {
        // Timeout 0: poll, never block. The shell's own read paces this loop,
        // and blocking here would stall the terminal for a tunnel.
        int destPort = 0;
        ssh_channel incoming = ssh_channel_accept_forward(session, 0, &destPort);
        while (incoming != nullptr) {
            // Match it to the forward that asked for this port, so the pane
            // attributes the connection to the right row.
            std::size_t index = 0;
            for (std::size_t i = 0; i < m_impl->statuses.size(); ++i) {
                if (m_impl->statuses[i].forward.kind == ForwardKind::Remote &&
                    m_impl->statuses[i].forward.bindPort == destPort) {
                    index = i;
                    break;
                }
            }
            const Forward& forward = m_impl->statuses[index].forward;

            // WE connect out, because a remote forward's destination is local
            // to us.
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            const std::string service = std::to_string(forward.destPort);
            addrinfo* resolved = nullptr;
            SOCKET outbound = INVALID_SOCKET;
            if (::getaddrinfo(forward.destHost.c_str(), service.c_str(), &hints, &resolved) == 0 &&
                resolved != nullptr) {
                outbound =
                    ::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
                if (outbound != INVALID_SOCKET &&
                    ::connect(outbound, resolved->ai_addr,
                              static_cast<int>(resolved->ai_addrlen)) != 0) {
                    closeSocket(outbound);
                }
                ::freeaddrinfo(resolved);
            }
            if (outbound == INVALID_SOCKET) {
                ssh_channel_close(incoming);
                ssh_channel_free(incoming);
            } else {
                setNonBlocking(outbound);
                Tunnel tunnel;
                tunnel.socket = outbound;
                tunnel.channel = incoming;
                tunnel.forwardIndex = index;
                ++m_impl->statuses[index].totalConnections;
                m_impl->tunnels.push_back(std::move(tunnel));
                changed = true;
            }
            incoming = ssh_channel_accept_forward(session, 0, &destPort);
        }
    }

    // 3. Move bytes.
    std::array<char, kChunk> buffer{};
    for (Tunnel& tunnel : m_impl->tunnels) {
        // Local -> (SOCKS or channel)
        if (!tunnel.socketClosed && tunnel.toChannel.size() < kMaxPending) {
            const int n = ::recv(tunnel.socket, buffer.data(), kChunk, 0);
            if (n == 0) {
                tunnel.socketClosed = true;
            } else if (n > 0) {
                if (tunnel.handshake) {
                    std::vector<std::uint8_t> reply;
                    const auto* bytes = reinterpret_cast<const std::uint8_t*>(buffer.data());
                    const socks5::Phase phase =
                        tunnel.handshake->feed({bytes, static_cast<std::size_t>(n)}, &reply);
                    if (!reply.empty()) {
                        ::send(tunnel.socket, reinterpret_cast<const char*>(reply.data()),
                               static_cast<int>(reply.size()), 0);
                    }
                    if (phase == socks5::Phase::Failed) {
                        tunnel.socketClosed = true;
                    } else if (phase == socks5::Phase::Ready) {
                        // Now we know where. Open the channel and tell the
                        // client whether it worked — a SOCKS client that never
                        // gets its reply hangs rather than failing.
                        tunnel.channel = ssh_channel_new(session);
                        const bool opened = tunnel.channel != nullptr &&
                                            ssh_channel_open_forward(
                                                tunnel.channel, tunnel.handshake->host().c_str(),
                                                tunnel.handshake->port(), "127.0.0.1", 0) == SSH_OK;
                        std::vector<std::uint8_t> settled;
                        tunnel.handshake->finish(opened, &settled);
                        if (!settled.empty()) {
                            ::send(tunnel.socket, reinterpret_cast<const char*>(settled.data()),
                                   static_cast<int>(settled.size()), 0);
                        }
                        if (!opened) {
                            if (tunnel.channel != nullptr) {
                                ssh_channel_free(tunnel.channel);
                                tunnel.channel = nullptr;
                            }
                            tunnel.socketClosed = true;
                        }
                        tunnel.handshake.reset();
                        changed = true;
                    }
                } else if (tunnel.channel != nullptr) {
                    tunnel.toChannel.insert(tunnel.toChannel.end(), buffer.data(),
                                            buffer.data() + n);
                }
            }
        }

        if (tunnel.channel == nullptr) {
            continue;  // still handshaking, or already torn down
        }

        // -> channel
        while (!tunnel.toChannel.empty()) {
            const int written =
                ssh_channel_write(tunnel.channel, tunnel.toChannel.data(),
                                  static_cast<std::uint32_t>(tunnel.toChannel.size()));
            if (written <= 0) {
                break;
            }
            tunnel.toChannel.erase(tunnel.toChannel.begin(), tunnel.toChannel.begin() + written);
        }

        // channel -> local
        if (tunnel.toSocket.size() < kMaxPending) {
            const int n = ssh_channel_read_nonblocking(tunnel.channel, buffer.data(), kChunk, 0);
            if (n > 0) {
                tunnel.toSocket.insert(tunnel.toSocket.end(), buffer.data(), buffer.data() + n);
            } else if (n == SSH_ERROR) {
                tunnel.socketClosed = true;
            }
        }
        while (!tunnel.toSocket.empty()) {
            const int sent =
                ::send(tunnel.socket, reinterpret_cast<const char*>(tunnel.toSocket.data()),
                       static_cast<int>(tunnel.toSocket.size()), 0);
            if (sent <= 0) {
                break;
            }
            tunnel.toSocket.erase(tunnel.toSocket.begin(), tunnel.toSocket.begin() + sent);
        }
    }

    // 4. Reap. A tunnel is finished when one side is closed AND everything
    // still buffered for the other side has been delivered — dropping the tail
    // is how a download ends up a few bytes short.
    const std::size_t before = m_impl->tunnels.size();
    std::erase_if(m_impl->tunnels, [](Tunnel& tunnel) {
        const bool channelDone =
            tunnel.channel != nullptr && ssh_channel_is_eof(tunnel.channel) != 0;
        const bool drained = tunnel.toSocket.empty() && tunnel.toChannel.empty();
        if (!((tunnel.socketClosed || channelDone) && drained)) {
            return false;
        }
        if (tunnel.channel != nullptr) {
            ssh_channel_send_eof(tunnel.channel);
            ssh_channel_close(tunnel.channel);
            ssh_channel_free(tunnel.channel);
            tunnel.channel = nullptr;
        }
        closeSocket(tunnel.socket);
        return true;
    });
    changed = changed || m_impl->tunnels.size() != before;

    // 5. Recount, so the pane shows what is actually open.
    if (changed) {
        for (TunnelStatus& status : m_impl->statuses) {
            status.connections = 0;
        }
        for (const Tunnel& tunnel : m_impl->tunnels) {
            ++m_impl->statuses[tunnel.forwardIndex].connections;
        }
        for (TunnelStatus& status : m_impl->statuses) {
            if (status.state == TunnelState::Failed) {
                continue;
            }
            status.state = status.connections > 0 ? TunnelState::Active : TunnelState::Listening;
        }
        publish();
    }
    return true;
}

void ForwardManager::stop() {
    for (Tunnel& tunnel : m_impl->tunnels) {
        if (tunnel.channel != nullptr) {
            ssh_channel_close(tunnel.channel);
            ssh_channel_free(tunnel.channel);
            tunnel.channel = nullptr;
        }
        closeSocket(tunnel.socket);
    }
    m_impl->tunnels.clear();
    for (Listener& listener : m_impl->listeners) {
        closeSocket(listener.socket);
    }
    m_impl->listeners.clear();
    m_impl->anyRemote = false;
    m_impl->statuses.clear();
}

}  // namespace krait::net

#pragma once

#include "forwards.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace krait::net {

// What the tunnel pane shows for one forward.
enum class TunnelState : std::uint8_t {
    Opening,    // requested, not yet listening
    Listening,  // ready, nothing connected through it yet
    Active,     // at least one connection is open
    Failed,     // could not be established; `detail` says why
};

struct TunnelStatus {
    Forward forward;
    TunnelState state = TunnelState::Opening;
    // Connections currently open through this tunnel.
    int connections = 0;
    // Total since the session started, so a tunnel that is working but idle
    // looks different from one nothing has ever used.
    int totalConnections = 0;
    std::string detail;
};

// Port forwarding over an established SSH session (plan T59).
//
// EVERYTHING here runs on the SSH worker thread, because a libssh session is
// not thread-safe and the alternative is a lock around every call plus the bug
// that shows up a week later. That is also why this is not built on
// QTcpServer: the worker is a raw std::thread with no event loop, so the
// listeners are Winsock sockets polled from the same loop that reads the shell.
//
// ponytail: serviced from the pump loop rather than its own event system, so a
// tunnel's latency is bounded by the shell poll interval (20 ms). Fine for a
// terminal's tunnels; if someone starts pushing bulk traffic through one, the
// upgrade path is libssh's ssh_event API rather than a second thread — a second
// thread would mean the lock this design exists to avoid.
class ForwardManager {
  public:
    // Called whenever any tunnel's state changes, on the WORKER thread. The
    // backend queues it to the GUI; nothing here touches Qt.
    using StatusCallback = std::function<void(const std::vector<TunnelStatus>&)>;

    ForwardManager();
    ~ForwardManager();

    ForwardManager(const ForwardManager&) = delete;
    ForwardManager& operator=(const ForwardManager&) = delete;

    void setStatusCallback(StatusCallback callback) { m_onStatus = std::move(callback); }

    // Opens every forward in `forwards`. Failures are per-tunnel: one port
    // already in use must not take the session down, because the session is
    // what the user actually asked for and the tunnel is a convenience.
    //
    // `session` is an ssh_session, as void* so libssh.h stays out of headers.
    void start(void* session, const std::vector<Forward>& forwards);

    // One non-blocking pass: accept new local connections, move bytes both
    // ways, reap what closed. Returns false only if the SESSION died, which is
    // the caller's business rather than a tunnel's.
    bool service(void* session);

    // Closes every listener and every open tunnel. Idempotent.
    void stop();

    std::vector<TunnelStatus> status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    StatusCallback m_onStatus;

    void publish();
};

}  // namespace krait::net

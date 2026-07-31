#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace krait::net {

// The pipe OpenSSH for Windows' ssh-agent listens on. From the agent's own
// source (contrib/win32/win32compat/ssh-agent/agent.c, AGENT_PIPE_ID); Windows
// documents key management but never names this pipe, so the citation is
// OpenSSH's, not Microsoft's.
inline constexpr const char* kOpenSshAgentPipe = R"(\\.\pipe\openssh-ssh-agent)";

// libssh's agent client and the Windows agent do not speak the same transport,
// and left alone they never meet.
//
// libssh's src/agent.c has exactly one way to reach an agent: read
// SSH_AUTH_SOCK (or SSH_OPTIONS_IDENTITY_AGENT), then ssh_socket_unix() it —
// socket(AF_UNIX) + connect(). There is no named-pipe path in it on any
// platform. The OpenSSH agent that ships with Windows listens on a named pipe
// and nothing else. So ssh_userauth_agent() on a stock Windows box fails, and
// it fails as SSH_AUTH_DENIED — the SAME code libssh returns when the server
// refused every key. That is why this was believed to work: the ladder falls
// through to a password prompt and nothing looks broken.
//
// The bridge is the missing transport. libssh does expose one seam,
// ssh_set_agent_socket(session, fd), which drops an fd straight into the agent
// socket; ssh_agent_is_running() is then satisfied by that socket merely
// HAVING an fd, and libssh talks its normal agent protocol down it. So: hand
// libssh one end of a socket pair, and relay between the other end and the
// pipe.
//
// The relay is framing-aware rather than a dumb byte pump. The agent protocol
// is strictly one request then one reply (uint32 big-endian length, then that
// many bytes), so following the framing costs ONE thread where a blind copy
// needs two, and it puts the length cap somewhere — a length prefix is a
// number from outside this process deciding how much we allocate, whether the
// process on the far end is trusted or not (rules/net.md).
//
// Nothing here includes libssh: the caller does the ssh_set_agent_socket()
// call, which is what lets the tests drive the whole relay without a session.
//
// KNOWN GAP, deliberately not closed: whoever answers the pipe is trusted to be
// the agent. If the agent service is not running, a local process can create
// that name first and answer as it. The damage is bounded — SECURITY_
// IDENTIFICATION stops it impersonating us, and a squatter can only OFFER keys,
// which the server still has to have authorised — so it is a nuisance, not a
// credential leak. Checking the server process's owner SID would close it, and
// would also break every setup where the agent runs as something other than
// what we guessed. OpenSSH's own Windows client carries the same exposure.
//
// stop() is expected to be called from one thread at a time. cancel() may be
// called from any thread, at any time, including while stop() is joining.
class AgentBridge {
  public:
    AgentBridge() = default;
    ~AgentBridge();
    AgentBridge(const AgentBridge&) = delete;
    AgentBridge& operator=(const AgentBridge&) = delete;

    // An unset socket. SOCKET is UINT_PTR and INVALID_SOCKET is ~0, spelled
    // here so the header does not need winsock2.h.
    static constexpr std::uintptr_t kNoSocket = static_cast<std::uintptr_t>(-1);

    // Opens the pipe and starts relaying. False means no agent is reachable,
    // which on a machine where nobody started one is the ordinary case and not
    // worth a banner — the auth ladder simply moves on to the next method.
    bool start(const std::string& pipeName = kOpenSshAgentPipe);

    // The end to give ssh_set_agent_socket(). Still owned here until
    // releaseSocket().
    std::uintptr_t socket() const { return m_libsshEnd; }

    // Says libssh owns the socket now: it closes it in ssh_free(), by way of
    // ssh_socket_close(). Without this the destructor would close it too, and
    // the second close would land on whatever fd Windows had since handed out.
    void releaseSocket() { m_libsshEnd = kNoSocket; }

    // Unblocks the relay WITHOUT waiting for it, from any thread.
    //
    // This is what a closing tab needs. libssh's agent client blocks in recv()
    // on the socket it was handed, so once the bridge is live the SSH worker
    // thread can be parked inside ssh_userauth_agent for as long as the agent
    // takes to answer — which for a FIDO2 key is "until somebody touches it",
    // and for a wedged agent service is forever. Joining that worker without
    // cancelling first hangs whoever called stop(), and for a tab close that is
    // the GUI thread.
    //
    // Ending the relay shuts its side of the socket pair, so libssh's recv()
    // returns 0 and the auth attempt fails instead of hanging.
    void cancel();

    // cancel(), then joins the relay and closes what is still ours. Safe on a
    // bridge that never started, and safe after the relay has already ended by
    // itself — which is the usual way it ends, when libssh closes its side.
    void stop();

    // Why start() failed. Diagnostics, not a banner.
    const std::string& error() const { return m_error; }

  private:
    // Pumps request/reply pairs until either side closes. Runs on m_relay.
    void pump();
    // One overlapped read or write on the pipe, abandoned the moment
    // m_stopEvent is set. `read` false means write.
    bool pipeIo(bool read, void* buffer, std::uint32_t length, std::uint32_t* moved);
    // The cancel half of stop(), with m_mutex already held.
    void cancelLocked();

    // Guards start/cancel/stop against each other — the GUI thread cancels
    // while the SSH worker thread starts or tears down. start() holds it from
    // the moment it publishes a handle, so a cancel() either lands before that
    // (and start() refuses) or after (and reaches a live relay); nothing in
    // between. NEVER held across the join in stop(), or a cancel() arriving
    // during that join would wait for the very thread it was supposed to
    // release. pump() takes it not at all: everything it touches is set before
    // the thread starts and cleared only after it is joined.
    std::mutex m_mutex;
    std::thread m_relay;
    // Set once, and never cleared while the relay runs: pump() checks it before
    // issuing each pipe operation, which is the window CancelIoEx alone cannot
    // close — CancelIoEx marks only the I/O already outstanding, so a read
    // issued a microsecond later is not cancelled by it.
    std::atomic<bool> m_stopping{false};
    // Manual-reset, and the other half of that: it is what the overlapped waits
    // in pipeIo() wake on.
    std::uintptr_t m_stopEvent = 0;
    // HANDLE and SOCKET, as integers, so windows.h stays in the .cpp — this
    // header is reached from ssh_backend.cpp, where windows.h ahead of libssh
    // has cost this project real repairs (NOMINMAX).
    std::uintptr_t m_pipe = 0;
    // The overlapped event pipeIo() waits on. One per bridge, reused, because
    // only ever one operation is in flight — the protocol is request/reply.
    std::uintptr_t m_ioEvent = 0;
    std::uintptr_t m_libsshEnd = kNoSocket;
    std::uintptr_t m_relayEnd = kNoSocket;
    std::string m_error;
};

}  // namespace krait::net

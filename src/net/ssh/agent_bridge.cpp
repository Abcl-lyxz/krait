#include "agent_bridge.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
// After winsock2.h, always: windows.h pulls the ancient winsock.h otherwise and
// the two disagree about every name in this file.
#include <windows.h>

#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace krait::net {

namespace {

// OpenSSH's own ceiling on one agent message (AGENT_MAX_LEN, authfd.c). The
// agent is a local service rather than a remote peer, but the length prefix is
// still a number from outside this process deciding how much we allocate, and
// net.md does not carve out an exception for friendly senders.
constexpr std::uint32_t kMaxAgentMessage = 256U * 1024U;

// The pipe may exist and still have every instance busy, which is a wait and
// not a failure. OpenSSH's own client retries the same way.
constexpr DWORD kPipeBusyWaitMs = 1000;

// Winsock has to be started before socket() will answer. Qt does it too, but
// only once a QAbstractSocket exists, and this runs on a worker thread that may
// well be the first thing in the process to want a socket.
void ensureWinsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        // Never cleaned up: WSACleanup on a process that still has Qt sockets
        // open would break them, and the pairing is process-lifetime anyway.
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

bool sameAddress(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr;
}

// How long a blocking recv on the relay socket may sit before it comes up for
// air. It is not a deadline on anything — the relay is SUPPOSED to wait
// indefinitely for libssh's next request — it is only how often the stop flag
// gets looked at.
//
// It exists because shutdown() is not enough on Windows. Documented behaviour
// is that recv calls made AFTER a shutdown return 0; a recv already pending
// inside the kernel is not guaranteed to come back. It usually does, which is
// the worst way for this to behave: the join in stop() then hangs occasionally,
// on someone else's machine, in the destructor of a closing tab.
constexpr int kRelayRecvTimeoutMs = 200;

// Reads exactly `len` bytes, or fails. A short read is not an error to retry
// past here: the framing depends on getting the whole header.
bool recvExactly(SOCKET socket, std::uint8_t* out, std::uint32_t len,
                 const std::atomic<bool>& stopping) {
    std::uint32_t done = 0;
    while (done < len) {
        const int got =
            ::recv(socket, reinterpret_cast<char*>(out) + done, static_cast<int>(len - done), 0);
        if (got == SOCKET_ERROR && ::WSAGetLastError() == WSAETIMEDOUT) {
            // The only reason the timeout is set. Nothing has gone wrong.
            if (stopping.load()) {
                return false;
            }
            continue;
        }
        if (got <= 0) {
            return false;
        }
        done += static_cast<std::uint32_t>(got);
    }
    return true;
}

bool sendAll(SOCKET socket, const std::uint8_t* data, std::uint32_t len) {
    std::uint32_t done = 0;
    while (done < len) {
        const int put = ::send(socket, reinterpret_cast<const char*>(data) + done,
                               static_cast<int>(len - done), 0);
        if (put <= 0) {
            return false;
        }
        done += static_cast<std::uint32_t>(put);
    }
    return true;
}

std::uint32_t readBigEndian32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

// Windows has no socketpair(), and libssh needs a real SOCKET: it hands the fd
// straight to recv()/send() (its atomicio), so a pipe HANDLE cannot stand in.
//
// The loopback listener below is reachable by every process on the machine for
// the moment it is up, and what sits behind it is the user's ssh-agent. So the
// accepted connection is CHECKED to be the one we dialled, in both directions,
// before either end is used — a local process that wins the race gets a closed
// socket instead of a signing oracle. Matching only one direction would still
// let an impostor that guessed our source port through.
bool loopbackPair(SOCKET* forLibssh, SOCKET* forRelay) {
    *forLibssh = INVALID_SOCKET;
    *forRelay = INVALID_SOCKET;

    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int addressLen = static_cast<int>(sizeof(address));

    SOCKET dialled = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    bool ok = false;

    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), addressLen) == 0 &&
        ::listen(listener, 1) == 0 &&
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLen) == 0) {
        dialled = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (dialled != INVALID_SOCKET &&
            ::connect(dialled, reinterpret_cast<const sockaddr*>(&address),
                      static_cast<int>(sizeof(address))) == 0) {
            accepted = ::accept(listener, nullptr, nullptr);
        }
    }

    if (accepted != INVALID_SOCKET) {
        sockaddr_in dialledLocal{};
        sockaddr_in acceptedPeer{};
        int oneLen = static_cast<int>(sizeof(dialledLocal));
        int otherLen = static_cast<int>(sizeof(acceptedPeer));
        ok = ::getsockname(dialled, reinterpret_cast<sockaddr*>(&dialledLocal), &oneLen) == 0 &&
             ::getpeername(accepted, reinterpret_cast<sockaddr*>(&acceptedPeer), &otherLen) == 0 &&
             sameAddress(dialledLocal, acceptedPeer);
    }

    ::closesocket(listener);
    if (!ok) {
        if (accepted != INVALID_SOCKET) {
            ::closesocket(accepted);
        }
        if (dialled != INVALID_SOCKET) {
            ::closesocket(dialled);
        }
        return false;
    }

    // Only on OUR end. libssh's end must keep blocking forever, because libssh
    // has no idea a timeout is a thing that can happen to it — its atomicio
    // treats anything other than EAGAIN as the agent having failed.
    const DWORD timeout = kRelayRecvTimeoutMs;
    ::setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));

    *forLibssh = dialled;
    *forRelay = accepted;
    return true;
}

// FILE_FLAG_OVERLAPPED, and that is the load-bearing part. A synchronous
// ReadFile on a pipe can only be cancelled by CancelIoEx, which marks the I/O
// ALREADY OUTSTANDING — so a relay sitting between "wrote the request" and
// "about to read the reply" would issue an uncancellable read a moment later
// and hang the join that was waiting for it. Overlapped reads let pipeIo() wait
// on the stop event as well, which has no such window.
HANDLE openPipe(const std::wstring& name) {
    // SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION is copied from OpenSSH's
    // own Windows client (win32compat/fileio.c): it lets the agent identify us
    // without letting it IMPERSONATE us, and the agent's ACL check expects
    // exactly that level. Opening the pipe without it is the kind of thing that
    // works on the developer's box and fails on a locked-down one.
    constexpr DWORD kFlags = FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION;
    HANDLE pipe = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, kFlags, nullptr);
    if (pipe == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_PIPE_BUSY &&
        ::WaitNamedPipeW(name.c_str(), kPipeBusyWaitMs) != 0) {
        // Every instance was in use, which is a wait rather than a failure:
        // reporting "no agent" to a user who plainly has one is worse than the
        // second of delay.
        pipe = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             kFlags, nullptr);
    }
    return pipe;
}

}  // namespace

AgentBridge::~AgentBridge() {
    stop();
}

bool AgentBridge::start(const std::string& pipeName) {
    if (m_relay.joinable()) {
        m_error = "already started";
        return false;
    }
    ensureWinsock();

    // Pipe names are ASCII in practice and this one is a literal; a wide
    // conversion that understood UTF-8 would be dead code for a fixed string.
    const std::wstring wide(pipeName.begin(), pipeName.end());

    HANDLE pipe = openPipe(wide);
    if (pipe == INVALID_HANDLE_VALUE) {
        m_error = "no agent on " + pipeName;
        return false;
    }

    SOCKET libsshEnd = INVALID_SOCKET;
    SOCKET relayEnd = INVALID_SOCKET;
    if (!loopbackPair(&libsshEnd, &relayEnd)) {
        ::CloseHandle(pipe);
        m_error = "could not make a socket pair for the agent relay";
        return false;
    }

    // Both manual-reset. The stop event stays set once cancelled, so a relay
    // that has not reached its next wait yet still sees it; the I/O event is
    // reset by pipeIo() before each operation.
    HANDLE stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE ioEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent == nullptr || ioEvent == nullptr) {
        if (stopEvent != nullptr) {
            ::CloseHandle(stopEvent);
        }
        if (ioEvent != nullptr) {
            ::CloseHandle(ioEvent);
        }
        ::closesocket(libsshEnd);
        ::closesocket(relayEnd);
        ::CloseHandle(pipe);
        m_error = "could not create the agent relay's events";
        return false;
    }

    // Everything above this point built handles nobody else can see yet.
    // Publishing them is what makes them cancellable, so it happens under the
    // lock — and the flag is CHECKED here rather than cleared.
    //
    // Clearing it would swallow a cancel() that arrived while this function was
    // still working. That window is not theoretical: openPipe can sit in
    // WaitNamedPipeW for a second, and a tab closed in that second would set
    // the flag while every handle cancel() looks at is still zero, so it does
    // nothing and returns. Wiping the flag afterwards would then start a relay
    // nobody can stop, and the GUI thread would hang in the join that follows.
    // stop() is what resets the flag, once the relay it belongs to is gone.
    const std::lock_guard<std::mutex> guard(m_mutex);
    if (m_stopping.load()) {
        ::CloseHandle(stopEvent);
        ::CloseHandle(ioEvent);
        ::closesocket(libsshEnd);
        ::closesocket(relayEnd);
        ::CloseHandle(pipe);
        m_error = "cancelled while starting";
        return false;
    }

    m_pipe = reinterpret_cast<std::uintptr_t>(pipe);
    m_stopEvent = reinterpret_cast<std::uintptr_t>(stopEvent);
    m_ioEvent = reinterpret_cast<std::uintptr_t>(ioEvent);
    m_libsshEnd = static_cast<std::uintptr_t>(libsshEnd);
    m_relayEnd = static_cast<std::uintptr_t>(relayEnd);
    m_error.clear();

    m_relay = std::thread([this] { pump(); });
    SetThreadDescription(m_relay.native_handle(), L"krait-agent-bridge");
    return true;
}

bool AgentBridge::pipeIo(bool read, void* buffer, std::uint32_t length, std::uint32_t* moved) {
    *moved = 0;
    // Checked BEFORE issuing, which is the half CancelIoEx cannot do: it
    // cancels outstanding I/O, and an operation not yet begun is not
    // outstanding.
    if (m_stopping.load()) {
        return false;
    }
    HANDLE pipe = reinterpret_cast<HANDLE>(m_pipe);
    OVERLAPPED overlapped{};
    overlapped.hEvent = reinterpret_cast<HANDLE>(m_ioEvent);
    ::ResetEvent(overlapped.hEvent);

    // lpNumberOfBytesTransferred is deliberately null. Windows documents it as
    // possibly holding an erroneous value when lpOverlapped is not null, so
    // GetOverlappedResult below is the only source of the count — reading it
    // from both is how an overlapped loop ends up waiting for bytes it already
    // has.
    DWORD done = 0;
    const BOOL started = read ? ::ReadFile(pipe, buffer, length, nullptr, &overlapped)
                              : ::WriteFile(pipe, buffer, length, nullptr, &overlapped);
    if (started == 0 && ::GetLastError() != ERROR_IO_PENDING) {
        return false;
    }
    if (started == 0) {
        const HANDLE waits[2] = {overlapped.hEvent, reinterpret_cast<HANDLE>(m_stopEvent)};
        if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
            // Stopping. The operation still owns `buffer`, so it has to be
            // cancelled AND waited for — returning while the kernel may still
            // write into a stack buffer is how this becomes a memory bug
            // instead of a shutdown.
            ::CancelIoEx(pipe, &overlapped);
            ::GetOverlappedResult(pipe, &overlapped, &done, TRUE);
            return false;
        }
    }
    // Waiting here costs nothing: either the operation already finished
    // synchronously, or the event above says it has.
    if (::GetOverlappedResult(pipe, &overlapped, &done, TRUE) == 0) {
        return false;
    }
    *moved = done;
    return done > 0;
}

void AgentBridge::pump() {
    const SOCKET socket = static_cast<SOCKET>(m_relayEnd);

    // One buffer for both directions, grown to whatever the longest message so
    // far needed. Agent messages are a few hundred bytes in practice; the cap
    // is what stops a bad length from becoming a 4 GB reserve.
    std::vector<std::uint8_t> message;
    std::array<std::uint8_t, 4> header{};

    // Strictly request then reply, which is what the protocol is: libssh writes
    // one message and blocks reading the answer. Following that shape is what
    // lets one thread do the work of two.
    //
    // A lambda so that EVERY way out — including the ones nested two loops deep
    // — passes through the shutdown below it. Leaving by return from in there
    // would skip it, and that is precisely the case where it matters: the agent
    // has stopped answering while libssh is blocked waiting for the reply.
    const auto relay = [&] {
        while (true) {
            if (!recvExactly(socket, header.data(), 4, m_stopping)) {
                // libssh closed its end — the ordinary way this ends.
                break;
            }
            std::uint32_t length = readBigEndian32(header.data());
            if (length == 0 || length > kMaxAgentMessage) {
                break;
            }
            message.resize(static_cast<std::size_t>(length) + 4);
            std::memcpy(message.data(), header.data(), header.size());
            if (!recvExactly(socket, message.data() + 4, length, m_stopping)) {
                break;
            }

            // A pipe write can be short too, so it loops like the reads.
            std::uint32_t sent = 0;
            while (sent < message.size()) {
                std::uint32_t moved = 0;
                if (!pipeIo(false, message.data() + sent,
                            static_cast<std::uint32_t>(message.size()) - sent, &moved)) {
                    return;
                }
                sent += moved;
            }

            // The reply, framed the same way. A byte-mode pipe — which is what the
            // agent creates — is free to hand back less than was asked for, so both
            // reads loop.
            std::uint32_t filled = 0;
            while (filled < 4) {
                std::uint32_t got = 0;
                if (!pipeIo(true, header.data() + filled, 4 - filled, &got)) {
                    return;
                }
                filled += got;
            }
            length = readBigEndian32(header.data());
            if (length == 0 || length > kMaxAgentMessage) {
                break;
            }
            message.resize(static_cast<std::size_t>(length) + 4);
            std::memcpy(message.data(), header.data(), header.size());
            filled = 0;
            while (filled < length) {
                std::uint32_t got = 0;
                if (!pipeIo(true, message.data() + 4 + filled, length - filled, &got)) {
                    return;
                }
                filled += got;
            }
            if (!sendAll(socket, message.data(), static_cast<std::uint32_t>(message.size()))) {
                break;
            }
        }
    };
    relay();

    // Close our side so libssh's next recv() returns 0 instead of hanging on a
    // relay that has already given up. Reached however the loop ended, which is
    // the whole reason the loop is a lambda.
    ::shutdown(socket, SD_BOTH);
}

void AgentBridge::cancelLocked() {
    m_stopping.store(true);
    if (m_stopEvent != 0) {
        // Manual-reset and never reset until the next start(), so a relay that
        // has not reached its next wait yet still sees it.
        ::SetEvent(reinterpret_cast<HANDLE>(m_stopEvent));
    }
    if (m_relayEnd != kNoSocket) {
        // Unblocks the relay if it is parked in recv() waiting for libssh's
        // next request, which is where it spends nearly all of its life. It
        // also reaches the SSH worker: closing this side sends a FIN to the end
        // libssh holds, so its own blocking recv() returns 0.
        ::shutdown(static_cast<SOCKET>(m_relayEnd), SD_BOTH);
    }
    if (m_pipe != 0) {
        // The I/O already in flight. The operations NOT yet issued are covered
        // by m_stopping, which pipeIo() checks before each one — CancelIoEx
        // alone leaves that window open.
        ::CancelIoEx(reinterpret_cast<HANDLE>(m_pipe), nullptr);
    }
}

void AgentBridge::cancel() {
    const std::lock_guard<std::mutex> guard(m_mutex);
    cancelLocked();
}

void AgentBridge::stop() {
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        cancelLocked();
    }
    // Joined OUTSIDE the lock, deliberately. A cancel() arriving from the GUI
    // thread while this join is waiting must not block on the mutex — it would
    // then be waiting for the very thread it was called to release, and the two
    // would hold each other there.
    if (m_relay.joinable()) {
        m_relay.join();
    }

    const std::lock_guard<std::mutex> guard(m_mutex);
    if (m_relayEnd != kNoSocket) {
        ::closesocket(static_cast<SOCKET>(m_relayEnd));
        m_relayEnd = kNoSocket;
    }
    if (m_libsshEnd != kNoSocket) {
        // Only reached when releaseSocket() was never called — the auth ladder
        // never got as far as handing this to libssh.
        ::closesocket(static_cast<SOCKET>(m_libsshEnd));
        m_libsshEnd = kNoSocket;
    }
    if (m_pipe != 0) {
        ::CloseHandle(reinterpret_cast<HANDLE>(m_pipe));
        m_pipe = 0;
    }
    if (m_ioEvent != 0) {
        ::CloseHandle(reinterpret_cast<HANDLE>(m_ioEvent));
        m_ioEvent = 0;
    }
    if (m_stopEvent != 0) {
        ::CloseHandle(reinterpret_cast<HANDLE>(m_stopEvent));
        m_stopEvent = 0;
    }
    // Cleared last, so the object can be started again. It is: one AgentBridge
    // serves every reconnect cycle of a backend, and a stale flag would leave
    // the second connection's relay refusing to do anything.
    m_stopping.store(false);
}

}  // namespace krait::net

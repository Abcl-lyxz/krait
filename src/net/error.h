#pragma once

#include <QString>

namespace krait::net {

// Error taxonomy seed (net.md: per-tab banner codes, never dialogs). Grows
// with the ssh/telnet backends; ConPTY uses the first three.
enum class ErrorCode {
    PtyCreateFailed,
    SpawnFailed,
    IoFailed,
    // The other end went away without the session ending cleanly: conhost
    // killed, the pipe broken from outside, and later the SSH cases. Distinct
    // from a shell that ran `exit`, which is not an error at all — telling
    // someone their connection "failed" when they typed exit is how a banner
    // trains people to ignore banners.
    PeerClosed,
    // SSH (T39). Split rather than one "ssh-failed", because each of these
    // wants a different banner and a different next action: retry, look at the
    // key, or fix the credential.
    ConnectFailed,
    // The server presented a DIFFERENT key than the one we know. Blocking, and
    // deliberately not the same code as a rejected new key: one is "you have
    // never met this host", the other is "something changed" (rules/net.md).
    HostKeyChanged,
    HostKeyRejected,
    AuthFailed,
};

struct BackendError {
    ErrorCode code;
    QString message;  // human text; never contains secrets (net.md)
};

inline QString errorCodeName(ErrorCode code) {
    switch (code) {
    case ErrorCode::PtyCreateFailed:
        return QStringLiteral("pty-create-failed");
    case ErrorCode::SpawnFailed:
        return QStringLiteral("spawn-failed");
    case ErrorCode::IoFailed:
        return QStringLiteral("io-failed");
    case ErrorCode::PeerClosed:
        return QStringLiteral("peer-closed");
    case ErrorCode::ConnectFailed:
        return QStringLiteral("connect-failed");
    case ErrorCode::HostKeyChanged:
        return QStringLiteral("host-key-changed");
    case ErrorCode::HostKeyRejected:
        return QStringLiteral("host-key-rejected");
    case ErrorCode::AuthFailed:
        return QStringLiteral("auth-failed");
    }
    return QStringLiteral("unknown");
}

}  // namespace krait::net

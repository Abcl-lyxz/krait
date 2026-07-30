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
    }
    return QStringLiteral("unknown");
}

}  // namespace krait::net

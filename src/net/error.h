#pragma once

#include <QString>

namespace krait::net {

// Error taxonomy seed (net.md: per-tab banner codes, never dialogs). Grows
// with the ssh/telnet backends; ConPTY uses the first three.
enum class ErrorCode {
    PtyCreateFailed,
    SpawnFailed,
    IoFailed,
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
    }
    return QStringLiteral("unknown");
}

}  // namespace krait::net

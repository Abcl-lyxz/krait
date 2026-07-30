#pragma once

#include "error.h"

namespace krait::net {

// Reconnect POLICY, kept away from the backend that enforces it so both halves
// can be tested without a server: what is worth retrying, and how long to wait.

// Retrying is only ever right for a failure that might have been the network.
//
// The three exclusions matter more than the inclusions:
//  - HostKeyChanged: retrying is retrying INTO the thing net.md treats as an
//    attack. There is no delay that makes it safe.
//  - HostKeyRejected: the user said no. Asking again in four seconds is how a
//    security prompt becomes something people click through.
//  - AuthFailed: a wrong password retried five times locks the account out, and
//    the credential is not going to fix itself in the meantime.
//
// A clean `exited` never reaches here at all: a shell that ran `exit` is not a
// failure, and reopening the session under someone would be a bug.
constexpr bool isRetryable(ErrorCode code) {
    switch (code) {
    case ErrorCode::ConnectFailed:
    case ErrorCode::IoFailed:
    case ErrorCode::PeerClosed:
        return true;
    case ErrorCode::PtyCreateFailed:
    case ErrorCode::SpawnFailed:
    case ErrorCode::HostKeyChanged:
    case ErrorCode::HostKeyRejected:
    case ErrorCode::AuthFailed:
        return false;
    }
    return false;
}

// Exponential, capped. `attempt` is 1-based: the first retry waits `baseMs`.
//
// ponytail: no jitter. Jitter exists to stop a fleet of clients retrying in
// lockstep and hammering a recovering server; one person's terminal is not a
// fleet, and a deterministic delay is one a user can predict and a test can
// assert. Revisit if broadcast-to-many-hosts (M4) ever reconnects in bulk.
constexpr int backoffDelayMs(int attempt, int baseMs = 1000, int capMs = 30000) {
    if (attempt <= 1) {
        return baseMs > capMs ? capMs : baseMs;
    }
    long long delay = baseMs;
    for (int i = 1; i < attempt; ++i) {
        delay *= 2;
        if (delay >= capMs) {
            return capMs;
        }
    }
    return static_cast<int>(delay);
}

}  // namespace krait::net

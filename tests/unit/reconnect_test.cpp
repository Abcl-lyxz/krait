#include "reconnect.h"
#include <catch2/catch_test_macros.hpp>

using namespace krait::net;

TEST_CASE("only failures that might have been the network are retried", "[net][reconnect]") {
    CHECK(isRetryable(ErrorCode::ConnectFailed));
    CHECK(isRetryable(ErrorCode::IoFailed));
    CHECK(isRetryable(ErrorCode::PeerClosed));

    // These three are the reason this is a function and not a bool. Retrying a
    // CHANGED host key is retrying into the thing net.md treats as an attack;
    // re-asking after a rejection is how a security prompt becomes something
    // people click through; and a wrong password retried five times locks the
    // account out.
    CHECK_FALSE(isRetryable(ErrorCode::HostKeyChanged));
    CHECK_FALSE(isRetryable(ErrorCode::HostKeyRejected));
    CHECK_FALSE(isRetryable(ErrorCode::AuthFailed));

    // A local pty that could not be created will not create itself either.
    CHECK_FALSE(isRetryable(ErrorCode::PtyCreateFailed));
    CHECK_FALSE(isRetryable(ErrorCode::SpawnFailed));
}

TEST_CASE("the policy is decidable at compile time", "[net][reconnect]") {
    // static_assert, not CHECK: the switch is exhaustive and warnings are
    // errors, so a future ErrorCode added without a case here fails the BUILD
    // instead of quietly defaulting to one answer or the other.
    static_assert(!isRetryable(ErrorCode::HostKeyChanged));
    static_assert(isRetryable(ErrorCode::ConnectFailed));
    static_assert(backoffDelayMs(1) == 1000);
    SUCCEED();
}

TEST_CASE("backoff doubles and then stops", "[net][reconnect]") {
    CHECK(backoffDelayMs(1, 1000, 30000) == 1000);
    CHECK(backoffDelayMs(2, 1000, 30000) == 2000);
    CHECK(backoffDelayMs(3, 1000, 30000) == 4000);
    CHECK(backoffDelayMs(4, 1000, 30000) == 8000);
    CHECK(backoffDelayMs(5, 1000, 30000) == 16000);
    // Capped, and it STAYS capped rather than overflowing into something small
    // — which is the failure mode that turns a backoff into a busy loop.
    CHECK(backoffDelayMs(6, 1000, 30000) == 30000);
    CHECK(backoffDelayMs(40, 1000, 30000) == 30000);
    CHECK(backoffDelayMs(1000, 1000, 30000) == 30000);
}

TEST_CASE("degenerate arguments do not produce a busy loop", "[net][reconnect]") {
    // Attempt 0 or negative must not mean "retry immediately, forever".
    CHECK(backoffDelayMs(0, 1000, 30000) == 1000);
    CHECK(backoffDelayMs(-5, 1000, 30000) == 1000);
    // A base above the cap is a misconfiguration, and the cap wins.
    CHECK(backoffDelayMs(1, 60000, 30000) == 30000);
}

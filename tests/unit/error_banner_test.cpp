// T33 — the contract between the backend's error taxonomy and what the user
// reads. The plan's verification is "contract test: PeerClosed -> banner, no
// dialog", and the "no dialog" half is structural: describeError returns BANNER
// properties, so there is no code path that could raise a modal. rules/ui.md
// bans QMessageBox and every app-modal surface in session flows.

#include "app/error_banner.h"
#include "net/error.h"
#include <catch2/catch_test_macros.hpp>

using krait::app::describeError;
using krait::net::ErrorCode;
using krait::net::errorCodeName;

TEST_CASE("PeerClosed produces a banner, not a dialog", "[errors]") {
    const auto banner = describeError(ErrorCode::PeerClosed);
    // Banner.qml's severity vocabulary. The only two surfaces are the warning
    // and error banners; there is nothing else this could become.
    CHECK(banner.severity == "error");
    CHECK_FALSE(banner.message.isEmpty());
    CHECK(banner.sessionEnded);
    // The hint says what happened rather than what to click: there is no
    // recovery action for a console host that died under us.
    CHECK(banner.hint.contains("console host"));
}

TEST_CASE("every taxonomy code has a message a user can act on", "[errors]") {
    for (const ErrorCode code : {ErrorCode::PtyCreateFailed, ErrorCode::SpawnFailed,
                                 ErrorCode::IoFailed, ErrorCode::PeerClosed}) {
        INFO("code: " << errorCodeName(code).toStdString());
        const auto banner = describeError(code);
        CHECK(banner.severity == "error");
        CHECK_FALSE(banner.message.isEmpty());
        // Every one of these ends the session; none may offer a retry that
        // would do nothing.
        CHECK(banner.sessionEnded);
    }
}

TEST_CASE("the two failures a user can actually fix say how", "[errors]") {
    // A hint is only worth showing when there is something to do. An invented
    // suggestion is worse than none, because somebody will follow it.
    CHECK_FALSE(describeError(ErrorCode::PtyCreateFailed).hint.isEmpty());
    CHECK_FALSE(describeError(ErrorCode::SpawnFailed).hint.isEmpty());
    // ...and a generic I/O failure has nothing honest to suggest.
    CHECK(describeError(ErrorCode::IoFailed).hint.isEmpty());
}

TEST_CASE("the wire name maps back to the same banner", "[errors]") {
    // The backend signal carries the NAME, not the enum, so the round trip has
    // to hold or the UI shows a different error from the one that happened.
    for (const ErrorCode code : {ErrorCode::PtyCreateFailed, ErrorCode::SpawnFailed,
                                 ErrorCode::IoFailed, ErrorCode::PeerClosed}) {
        const auto byCode = describeError(code);
        const auto byName = describeError(errorCodeName(code));
        INFO("code: " << errorCodeName(code).toStdString());
        CHECK(byName.message == byCode.message);
        CHECK(byName.hint == byCode.hint);
        CHECK(byName.severity == byCode.severity);
    }
}

TEST_CASE("an unknown code still reaches the user", "[errors]") {
    // A backend that grows a code the UI has not learned yet must not fail
    // silently — the user would see nothing at all and conclude the terminal
    // simply stopped.
    const auto banner = describeError(QStringLiteral("ssh-host-key-changed"));
    CHECK(banner.severity == "error");
    CHECK_FALSE(banner.message.isEmpty());
    CHECK(banner.hint == "ssh-host-key-changed");
}

TEST_CASE("code names are stable and distinct", "[errors]") {
    // These strings cross a signal boundary and end up in logs and bug reports.
    CHECK(errorCodeName(ErrorCode::PtyCreateFailed) == "pty-create-failed");
    CHECK(errorCodeName(ErrorCode::SpawnFailed) == "spawn-failed");
    CHECK(errorCodeName(ErrorCode::IoFailed) == "io-failed");
    CHECK(errorCodeName(ErrorCode::PeerClosed) == "peer-closed");
}

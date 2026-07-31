#pragma once

#include "net/error.h"
#include "net/ssh/ssh_backend.h"  // HostKeyState

#include <QCoreApplication>
#include <QString>

namespace krait::app {

// How a backend error is presented (plan T33).
//
// rules/ui.md: "Errors are per-tab banners with error-taxonomy codes from the
// backend layer. QMessageBox and any app-modal surface are banned in session
// flows." rules/net.md adds that the message never carries secrets.
//
// Header-only and pure: the mapping from code to what the user reads is the
// part worth testing, and it must not need a window to assert.
struct ErrorBanner {
    // Matches Banner.qml's `severity` property.
    QString severity;
    QString message;
    // What to do about it. Empty when there is nothing useful to say — an
    // invented suggestion is worse than none, because someone will follow it.
    QString hint;
    // Whether the code names a session that is over. A banner for a dead
    // session must not offer "Retry" as though the tab were still live.
    bool sessionEnded = false;
};

inline ErrorBanner describeError(net::ErrorCode code) {
    // QCoreApplication::translate is called DIRECTLY at each site, not through
    // a local helper. lupdate extracts string literals passed straight to the
    // call and cannot see through one more level of indirection — with a
    // `translate(...)` lambda every one of these silently never reaches the
    // .ts file, and ships untranslated with nothing to show that it did.
    switch (code) {
    case net::ErrorCode::PtyCreateFailed:
        return {
            .severity = QStringLiteral("error"),
            .message =
                QCoreApplication::translate("ErrorBanner", "Could not create the pseudoconsole."),
            .hint = QCoreApplication::translate(
                "ErrorBanner", "The bundled OpenConsole may be missing from the openconsole "
                               "folder beside Krait."),
            .sessionEnded = true,
        };
    case net::ErrorCode::SpawnFailed:
        return {
            .severity = QStringLiteral("error"),
            .message = QCoreApplication::translate("ErrorBanner", "Could not start the shell."),
            .hint = QCoreApplication::translate(
                "ErrorBanner", "Check that the configured shell exists and is executable."),
            .sessionEnded = true,
        };
    case net::ErrorCode::IoFailed:
        return {
            .severity = QStringLiteral("error"),
            .message = QCoreApplication::translate("ErrorBanner", "Lost contact with the shell."),
            .hint = {},
            .sessionEnded = true,
        };
    case net::ErrorCode::PeerClosed:
        return {
            .severity = QStringLiteral("error"),
            .message =
                QCoreApplication::translate("ErrorBanner", "The session ended unexpectedly."),
            .hint = QCoreApplication::translate(
                "ErrorBanner", "The console host closed while the shell was still running."),
            .sessionEnded = true,
        };
    case net::ErrorCode::ConnectFailed:
        return {
            .severity = QStringLiteral("error"),
            .message = QCoreApplication::translate("ErrorBanner", "Could not reach the server."),
            .hint = QCoreApplication::translate(
                "ErrorBanner", "Check the host name and port, and that the network is up."),
            .sessionEnded = true,
        };
    case net::ErrorCode::HostKeyChanged:
        // The one banner in the app that is a WARNING rather than a report.
        // rules/net.md: a changed host key is blocking, and the explanation is
        // in plain language — someone who does not know what a host key is has
        // to be able to tell "the admin rebuilt the box" from "do not type your
        // password". No hint suggests continuing, because there is no way to.
        return {
            .severity = QStringLiteral("danger"),
            .message = QCoreApplication::translate(
                "ErrorBanner",
                "This server is presenting a different identity than the one Krait remembers."),
            .hint = QCoreApplication::translate(
                "ErrorBanner",
                "That happens when a server is rebuilt or its key is replaced — and it is also "
                "what an interception looks like. Ask whoever runs the server before you "
                "connect again, and do not type a password until you have."),
            .sessionEnded = true,
        };
    case net::ErrorCode::HostKeyRejected:
        return {
            .severity = QStringLiteral("error"),
            .message = QCoreApplication::translate(
                "ErrorBanner", "The server's identity was not accepted, so nothing was sent "
                               "to it."),
            .hint = QCoreApplication::translate(
                "ErrorBanner",
                "Compare the fingerprint with one you got from a source other than this "
                "connection, then try again."),
            .sessionEnded = true,
        };
    case net::ErrorCode::AuthFailed:
        return {
            .severity = QStringLiteral("error"),
            .message = QCoreApplication::translate("ErrorBanner",
                                                   "The server did not accept these credentials."),
            .hint = QCoreApplication::translate(
                "ErrorBanner", "Check the user name, and whether this profile should be using a "
                               "key or the agent instead of a password."),
            .sessionEnded = true,
        };
    }
    // Reached only for a value outside the enum. There is no `default:` label
    // on purpose: adding a code has to break this switch's exhaustiveness check
    // at COMPILE time rather than land here at runtime with a vague message.
    return {
        .severity = QStringLiteral("error"),
        .message = QCoreApplication::translate("ErrorBanner", "The session failed."),
        .hint = {},
        .sessionEnded = true,
    };
}

// A host key that needs a human (plan T52). Separate from ErrorBanner because
// the question is different in kind: an error reports something that already
// happened, and this asks for a decision that has not been made yet.
struct HostKeyPrompt {
    QString severity;
    QString message;
    // rules/net.md: a CHANGED key is never a yes/no question, and there is no
    // setting anywhere that turns it back into one. False here means the banner
    // shows no Trust button at all — not a Trust button that quietly refuses,
    // which teaches people the warning is theatre.
    bool askable = false;
};

inline HostKeyPrompt describeHostKey(net::HostKeyState state) {
    switch (state) {
    case net::HostKeyState::Unknown:
    case net::HostKeyState::NoFile:
        return {
            .severity = QStringLiteral("warning"),
            .message = QCoreApplication::translate(
                "ErrorBanner", "Krait has not seen this server before. Check the fingerprint "
                               "below against one you got from somewhere other than this "
                               "connection, then decide."),
            .askable = true,
        };
    case net::HostKeyState::Changed:
        return {
            .severity = QStringLiteral("danger"),
            .message = QCoreApplication::translate(
                "ErrorBanner",
                "This server is presenting a different identity than the one Krait remembers, "
                "so the connection was stopped before anything was sent."),
            .askable = false,
        };
    case net::HostKeyState::OtherType:
        return {
            .severity = QStringLiteral("danger"),
            .message = QCoreApplication::translate(
                "ErrorBanner", "This server offered a key of a different type than the one "
                               "Krait has on record for it, so the connection was stopped."),
            .askable = false,
        };
    case net::HostKeyState::Ok:
    case net::HostKeyState::Error:
        break;
    }
    // Ok never prompts, and Error means we could not tell — which is refused
    // rather than asked about, because "we do not know who this is" is not a
    // question a user can answer.
    return {
        .severity = QStringLiteral("error"),
        .message = QCoreApplication::translate(
            "ErrorBanner", "Krait could not check this server's identity, so it did not connect."),
        .askable = false,
    };
}

// The same mapping keyed by the wire name the backend signal carries, so QML
// and the item never have to parse the code themselves.
inline ErrorBanner describeError(const QString& codeName) {
    for (const net::ErrorCode code : {net::ErrorCode::PtyCreateFailed, net::ErrorCode::SpawnFailed,
                                      net::ErrorCode::IoFailed, net::ErrorCode::PeerClosed}) {
        if (net::errorCodeName(code) == codeName) {
            return describeError(code);
        }
    }
    ErrorBanner banner = describeError(net::ErrorCode::IoFailed);
    // An unknown code still reaches the user rather than being swallowed: a
    // backend that grows a code the UI has not learned yet must not fail
    // silently, so the raw name goes in the hint.
    banner.hint = codeName;
    return banner;
}

}  // namespace krait::app

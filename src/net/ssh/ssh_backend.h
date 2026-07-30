#pragma once

#include "../error.h"
#include "../ibackend.h"
#include "../vault/vault.h"

#include <QByteArray>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace krait::net {

// What the profile hands the backend. No secrets: a password or passphrase is
// fetched from the Vault by `vaultKey`, or asked for interactively.
struct SshConfig {
    std::string host;
    int port = 22;
    std::string user;
    // "" = libssh's default (~/.ssh/known_hosts). Set in tests so a run cannot
    // touch the developer's real file.
    std::string knownHostsPath;
    std::string keyPath;
    // Vault key prefix — the profile id. ":password" and ":passphrase" are
    // appended (rules/net.md: nothing plaintext leaves the vault).
    std::string vaultKey;
    // TERM. xterm-256color rather than "krait": a name no termcap database has
    // heard of makes remote curses apps fall back to dumb, and the first thing
    // anyone would then do is set TERM by hand.
    std::string termType = "xterm-256color";
    int connectTimeoutSeconds = 15;
};

// What ssh_session_is_known_server() answered, mapped to something the UI can
// show without knowing libssh exists.
enum class HostKeyState {
    Ok,         // known and unchanged — no prompt
    Unknown,    // first contact: TOFU
    NoFile,     // no known_hosts yet; same decision as Unknown
    Changed,    // BLOCKING. Never a yes/no prompt (rules/net.md)
    OtherType,  // a key of a different type exists: also blocking
    Error,
};

// SSH over libssh (ADR-0002). The whole libssh session lives on ONE worker
// thread, because a libssh session is not thread-safe and the alternative is a
// lock around every call plus the bug that shows up a week later.
//
// Host-key trust and credentials need a human, mid-connect, on a thread that
// cannot show UI. Those points emit a queued signal and then wait on a
// condition variable with a timeout; the GUI answers through respondHostKey()
// or respondCredential(). stop() releases those waits rather than only setting
// a flag — a tab closed during a host-key prompt must not leave a thread parked
// until the timeout expires.
class SshBackend : public IBackend {
    Q_OBJECT

  public:
    // `vault` is borrowed and outlives this object (main() owns it). It may be
    // null, in which case every credential is asked for interactively.
    SshBackend(SshConfig config, Vault* vault, QObject* parent = nullptr);  // owned by parent
    ~SshBackend() override;

    bool start(int cols, int rows) override;
    void writeInput(const QByteArray& bytes) override;
    void resize(int cols, int rows) override;
    void stop() override;

    // True once the shell channel is open. Safe to read from the GUI thread.
    bool isConnected() const { return m_connected.load(); }

  public slots:
    // Answers hostKeyPrompt. `trust` false aborts the connection. A Changed or
    // OtherType key is refused whatever this says — the prompt for those is
    // informational, and there is no setting anywhere that turns it back into a
    // question (rules/net.md).
    void respondHostKey(bool trust);

    // Answers credentialPrompt. `remember` stores it in the vault under
    // vaultKey. Cancel by passing an empty string.
    //
    // ponytail: the text arrives as a QString because that is what a QML
    // TextField holds, and a QString may have reallocated before we ever see
    // it. It is copied into a Secret and the QString overwritten here; the
    // ceiling is the copies QML already made. Closing that needs a text input
    // backed by our own buffer, which is a T46 problem.
    void respondCredential(const QString& text, bool remember);

  signals:
    // Queued to the GUI thread. `detail` is already human-readable — the
    // fingerprint and randomart block for a new key, old-vs-new for a changed
    // one (T40 fills these in). `state` is a HostKeyState.
    void hostKeyPrompt(int state, const QString& detail);
    // `echo` false means a password field. `prompt` is SERVER-CONTROLLED text
    // for keyboard-interactive, so the UI must render it as plain text.
    void credentialPrompt(const QString& prompt, bool echo);
    void connected();

  private:
    struct Impl;  // libssh handles; defined in the .cpp so libssh.h stays there

    void run(int cols, int rows);
    // Each returns false with errorOccurred already emitted.
    bool connectSession();
    bool verifyHostKey();
    bool authenticate();
    bool openShell(int cols, int rows);
    void pump();

    void fail(ErrorCode code, const QString& message);
    // Blocks the worker until the GUI answers, `timeoutMs` passes, or stop() is
    // called. False means "no answer" for any of those reasons.
    bool waitForAnswer(int timeoutMs);

    SshConfig m_config;
    Vault* m_vault = nullptr;  // borrowed; owned by main()
    std::unique_ptr<Impl> m_impl;

    std::thread m_worker;
    std::atomic<bool> m_shutdown{false};
    std::atomic<bool> m_connected{false};
    bool m_started = false;

    // Guards the write queue, the pending grid size, and the answer handshake.
    // One mutex: these are touched from the same two threads at the same
    // moments, and two would only be two things to take in the right order.
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<QByteArray> m_writeQueue;
    int m_pendingCols = 0;
    int m_pendingRows = 0;
    bool m_resizePending = false;

    // The answer handshake. `m_answered` is the predicate the worker waits on,
    // so a spurious wake cannot look like a yes.
    bool m_answered = false;
    bool m_hostKeyTrusted = false;
    Secret m_credential;
    bool m_rememberCredential = false;
};

}  // namespace krait::net

#pragma once

#include "../error.h"
#include "../ibackend.h"
#include "../vault/vault.h"
#include "forward_manager.h"
#include "forwards.h"

#include <QByteArray>
#include <QVariantList>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// At GLOBAL scope, and deliberately: `struct ssh_session_struct*` written
// inside the namespace below declares a NEW type called
// krait::net::ssh_session_struct, which then does not match libssh's callback
// signatures. Forward-declaring it here is what keeps libssh.h out of this
// header without lying about the type.
struct ssh_session_struct;

namespace krait::net {

// Which method to use. Mirrors session::SshAuth deliberately rather than
// sharing it: src/net must not depend on the app layer, and a five-value enum
// is a cheaper price than that dependency.
enum class SshAuthPreference { Auto, Agent, Password, PublicKey, KeyboardInteractive };

// What the profile hands the backend. No secrets: a password or passphrase is
// fetched from the Vault by `vaultKey`, or asked for interactively.
struct SshConfig {
    std::string host;
    int port = 22;
    std::string user;
    // "" = libssh's default (~/.ssh/known_hosts). Set in tests so a run cannot
    // touch the developer's real file.
    std::string knownHostsPath;
    SshAuthPreference auth = SshAuthPreference::Auto;
    std::string keyPath;
    // Comma-separated hops, OpenSSH's ProxyJump spelling:
    // "bastion", "user@bastion:2222,inner". Empty means a direct connection.
    //
    // ADR-0012: libssh 0.11 gained native in-process ProxyJump, so these are
    // its hops rather than direct-tcpip channels we chain ourselves. Krait's
    // host-key and auth UX runs for EVERY hop through the per-hop callbacks —
    // a bastion whose key changed matters exactly as much as the target's.
    std::string proxyJump;
    // Port forwards to open once the session is up. Failures are per-tunnel:
    // the session is what the user asked for, and a port already in use must
    // not cost them the shell.
    std::vector<Forward> forwards;
    // Vault key prefix — the profile id. ":password" and ":passphrase" are
    // appended (rules/net.md: nothing plaintext leaves the vault).
    std::string vaultKey;
    // TERM. xterm-256color rather than "krait": a name no termcap database has
    // heard of makes remote curses apps fall back to dumb, and the first thing
    // anyone would then do is set TERM by hand.
    std::string termType = "xterm-256color";
    int connectTimeoutSeconds = 15;
    // Idle seconds before a keepalive goes out. An idle SSH session behind NAT
    // dies silently otherwise, and the user finds out by typing into a
    // connection that has been dead for twenty minutes. 0 disables it.
    int keepaliveSeconds = 30;
    // 0 disables reconnecting. Only failures isRetryable() allows are retried
    // — never a changed host key, a rejected key, or a bad password.
    int maxReconnectAttempts = 5;
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

// Hops in an OpenSSH ProxyJump string: "a,b@c:22,d" is three.
//
// Exposed rather than kept private because getting it wrong is a SECURITY bug
// with no visible symptom: libssh takes one callbacks struct per hop, so a
// count that is short by one leaves that hop using libssh's DEFAULT host-key
// check instead of Krait's — the connection still works, and a changed key on
// the bastion goes unremarked.
std::size_t countProxyJumpHops(const std::string& spec);

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
    // The tunnel pane's model. Emitted from the worker thread, so the
    // connection to it is queued like every other.
    void forwardsChanged(const QVariantList& tunnels);
    // A retryable failure, and what happens next. The banner says "reconnecting
    // in 4 s (2 of 5)" rather than leaving a dead terminal that looks alive.
    void reconnecting(int attempt, int ofAttempts, int delayMs);

  private:
    struct Impl;  // libssh handles; defined in the .cpp so libssh.h stays there

    // How a connection ENDED, which is what decides whether to try again.
    enum class Outcome {
        CleanExit,  // the remote shell exited: never reconnect
        Failed,     // something the caller already reported; may be retryable
        Stopped,    // stop() was called
    };

    void run(int cols, int rows);
    // One connect-to-disconnect cycle.
    Outcome runOnce(int cols, int rows);
    // Sleeps `ms`, waking early if stop() is called. False means stopped.
    bool sleepInterruptible(int ms);

    // Each returns false with errorOccurred already emitted.
    bool connectSession();
    // `session` is an ssh_session, passed as void* so libssh.h stays in the
    // .cpp with the rest of the pimpl. It is NOT always m_impl->session: a
    // ProxyJump hop hands its own session to these through the callbacks, and
    // that is the whole reason they take one.
    //
    // `label` is what the banner calls this hop — "bastion.example.com (jump
    // host)" rather than the target's name, because a user asked to trust a
    // key needs to know which machine is asking.
    bool verifyHostKey(void* session, const QString& label);
    // libssh's per-hop callbacks. Static with `this` in userdata, because a C
    // API cannot call a member function. Declared here rather than as file
    // statics so they can reach the private methods above.
    //
    // The parameter is an ssh_session; spelled `struct ssh_session_struct*`
    // rather than the typedef so libssh.h still does not have to be included
    // by anything that includes this header.
    static int jumpVerifyHostKey(::ssh_session_struct* session, void* userdata);
    static int jumpAuthenticate(::ssh_session_struct* session, void* userdata);
    bool authenticate(void* session);
    // Each returns an SSH_AUTH_* code. Split out because "which methods, in
    // which order" is a policy question and the policy is easier to read when
    // it is not tangled with libssh's calling conventions.
    int tryAgent();
    int tryPublicKey();
    int tryKeyboardInteractive();
    int tryPassword();
    // Prompts for one secret and returns it. Empty means cancelled or timed
    // out. `remember` says whether the user asked to store it.
    Secret askForSecret(const QString& prompt, bool echo, bool* remember);

    bool openShell(int cols, int rows);
    Outcome pump();

    void fail(ErrorCode code, const QString& message);
    // Clears the answer slot. MUST be called before emitting the prompt, not
    // inside waitForAnswer: the answer can arrive before the worker reaches the
    // wait — trivially so if the receiver is connected directly, and as a race
    // otherwise — and clearing it afterwards throws that answer away and then
    // waits for it.
    void armAnswer();
    // Blocks the worker until the GUI answers, `timeoutMs` passes, or stop() is
    // called. False means "no answer" for any of those reasons.
    bool waitForAnswer(int timeoutMs);

    SshConfig m_config;
    Vault* m_vault = nullptr;  // borrowed; owned by main()
    std::unique_ptr<Impl> m_impl;
    // Tunnels. Lives here rather than in Impl because it holds no libssh type
    // in its header, and it is touched only from the worker thread.
    ForwardManager m_forwards;

    std::thread m_worker;
    // What fail() last reported. Written and read on the worker thread only —
    // it exists so run() can ask isRetryable() about the actual failure rather
    // than guessing from a bool.
    ErrorCode m_lastError = ErrorCode::IoFailed;
    std::atomic<bool> m_shutdown{false};
    std::atomic<bool> m_connected{false};
    // Whether the CURRENT cycle ever reached the connected state, so run() can
    // reset the retry counter after a reconnect that worked.
    bool m_everConnected = false;
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

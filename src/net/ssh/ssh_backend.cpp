// These two must precede EVERY include, which is what the position here buys —
// the Qt headers reached through ssh_backend.h pull in <windows.h> themselves,
// so a define placed after them is dead and the min/max macros land in scope
// for the whole translation unit. std::min in tryKeyboardInteractive is where
// that showed up. Same lesson as src/app/main.cpp.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ssh_backend.h"

#include "../reconnect.h"
#include "../remote_text.h"
#include "agent_bridge.h"
#include "algorithms.h"
#include "hostkey_art.h"
#include <windows.h>

#include <libssh/callbacks.h>
#include <libssh/libssh.h>
// ssh_send_keepalive is declared in server.h but works for a CLIENT session:
// it sends the keepalive@openssh.com global request. libssh puts it there
// because servers use it too, not because it is server-only.
#include <libssh/server.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace krait::net {

namespace {

// How long a blocking channel read waits before the loop gets a turn. It is
// also the worst-case latency between a keystroke arriving on the GUI thread
// and reaching the wire, because a libssh session is single-threaded and the
// write can only happen between reads.
//
// ponytail: 20 ms poll. One frame at 50 fps, and ~50 wakeups a second doing
// nothing measurable. The alternative is an ssh_event loop woken by a socket
// pair — real code, for latency nobody can feel.
constexpr int kPollMs = 20;
constexpr std::size_t kReadChunk = std::size_t{16} * 1024;

// A human has this long to answer a host-key or password prompt before the
// connection gives up. rules/cpp.md: every wait has a timeout.
constexpr int kPromptTimeoutMs = 5 * 60 * 1000;

// Several libssh getters return NULL rather than "" when a field is absent,
// and std::string_view has no NULL. One place to get that wrong is enough.
std::string_view nullSafe(const char* text) {
    return text != nullptr ? std::string_view{text} : std::string_view{};
}

}  // namespace

// libssh handles live here so libssh.h never reaches a header of ours.

// Empty segments are not counted: libssh ignores them, and appending a
// callbacks struct for one would put the list out of step with the chain.
std::size_t countProxyJumpHops(const std::string& spec) {
    std::size_t hops = 0;
    std::size_t start = 0;
    while (start <= spec.size()) {
        const std::size_t comma = spec.find(',', start);
        const std::size_t end = comma == std::string::npos ? spec.size() : comma;
        if (end > start) {
            ++hops;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return hops;
}

namespace {

// What to call a hop in a banner. libssh knows the host it is connecting to,
// and asking it beats re-parsing our own spec — which hop is being verified
// right now is not something the spec string can tell us.
QString hopLabel(ssh_session session) {
    char* host = nullptr;
    if (ssh_options_get(session, SSH_OPTIONS_HOST, &host) == SSH_OK && host != nullptr) {
        const QString label = QString::fromUtf8(host);
        ssh_string_free_char(host);
        return label;
    }
    return QStringLiteral("the jump host");
}

}  // namespace

struct SshBackend::Impl {
    ssh_session session = nullptr;
    ssh_channel channel = nullptr;
    // libssh keeps the POINTER to each callbacks struct, so these must outlive
    // ssh_connect — a vector member rather than anything on a stack frame.
    std::vector<ssh_jump_callbacks_struct> jumpCallbacks;
    // T60. Reaches the Windows agent, which libssh cannot. Lives for the whole
    // backend rather than one cycle so a reconnect gets a fresh one through
    // stop() in teardown, without the auth ladder having to own its lifetime.
    AgentBridge agent;
};

SshBackend::SshBackend(SshConfig config, Vault* vault, QObject* parent)
    : IBackend(parent), m_config(std::move(config)), m_vault(vault),
      m_impl(std::make_unique<Impl>()) {}

SshBackend::~SshBackend() {
    stop();
}

void SshBackend::fail(ErrorCode code, const QString& message) {
    m_lastError = code;
    emit errorOccurred(errorCodeName(code), message);
}

bool SshBackend::start(int cols, int rows) {
    if (m_started) {
        return true;
    }
    if (m_config.host.empty()) {
        fail(ErrorCode::ConnectFailed, tr("This session has no host to connect to."));
        return false;
    }
    m_started = true;
    m_shutdown = false;
    // The connection itself is asynchronous: start() returning true means the
    // worker launched, not that the session is up. Everything after this point
    // reaches the user as a signal, which is what lets a host-key prompt be a
    // banner instead of a modal dialog.
    m_worker = std::thread([this, cols, rows] { run(cols, rows); });
    return true;
}

void SshBackend::run(int cols, int rows) {
    int attempt = 0;
    for (;;) {
        const Outcome outcome = runOnce(cols, rows);
        if (outcome == Outcome::CleanExit || outcome == Outcome::Stopped) {
            return;
        }
        // A cycle that actually connected earns a clean slate. Without this,
        // `attempt` only ever counts up, and an overnight session on flaky wifi
        // that reconnects SUCCESSFULLY once an hour still gives up on the sixth
        // drop — the limit is meant to bound consecutive failures, not a
        // session's lifetime.
        if (m_everConnected) {
            attempt = 0;
            m_everConnected = false;
        }
        // Whether this failure is worth another go is a policy question, and
        // reconnect.h answers it: never a changed host key, a rejected key or a
        // bad password, however many attempts are configured.
        if (!isRetryable(m_lastError) || m_config.maxReconnectAttempts <= 0) {
            return;
        }
        if (++attempt > m_config.maxReconnectAttempts) {
            fail(ErrorCode::ConnectFailed, tr("Gave up reconnecting to %1 after %2 attempts.")
                                               .arg(QString::fromStdString(m_config.host))
                                               .arg(m_config.maxReconnectAttempts));
            return;
        }
        const int delay = backoffDelayMs(attempt);
        // Said out loud, with the numbers. A terminal that silently retries
        // looks identical to one that has hung.
        emit reconnecting(attempt, m_config.maxReconnectAttempts, delay);
        if (!sleepInterruptible(delay)) {
            return;
        }
    }
}

bool SshBackend::sleepInterruptible(int ms) {
    std::unique_lock lock(m_mutex);
    // Waits on the same condition variable stop() notifies, so closing a tab
    // during a 30-second backoff does not hold the join for 30 seconds.
    m_cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return m_shutdown.load(); });
    return !m_shutdown.load();
}

SshBackend::Outcome SshBackend::runOnce(int cols, int rows) {
    const auto teardown = [this] {
        if (m_impl->channel != nullptr) {
            ssh_channel_free(m_impl->channel);
            m_impl->channel = nullptr;
        }
        if (m_impl->session != nullptr) {
            ssh_disconnect(m_impl->session);
            ssh_free(m_impl->session);
            m_impl->session = nullptr;
        }
        // After ssh_free, deliberately: freeing the session closes the socket
        // the bridge handed it, which is what ends the relay by itself. This
        // then only joins the thread and closes the pipe, and a reconnect opens
        // a fresh one.
        m_impl->agent.stop();
    };

    if (!connectSession() ||
        !verifyHostKey(m_impl->session, QString::fromStdString(m_config.host)) ||
        !authenticate(m_impl->session) || !openShell(cols, rows)) {
        // Every failure path has already emitted its own error, with the code
        // that names what went wrong; m_lastError carries it to run().
        teardown();
        return m_shutdown.load() ? Outcome::Stopped : Outcome::Failed;
    }

    m_connected = true;
    m_everConnected = true;
    emit connected();

    // AFTER the shell, so a tunnel that cannot bind does not delay the thing
    // the user actually opened. The callback runs on this thread and only
    // converts to a QVariantList; the emit is queued like every other.
    m_forwards.setStatusCallback([this](const std::vector<TunnelStatus>& tunnels) {
        QVariantList rows;
        for (const TunnelStatus& tunnel : tunnels) {
            QVariantMap row;
            row["label"] = QString::fromStdString(tunnel.forward.describe());
            row["state"] = static_cast<int>(tunnel.state);
            row["connections"] = tunnel.connections;
            row["total"] = tunnel.totalConnections;
            row["detail"] = QString::fromStdString(tunnel.detail);
            rows.append(row);
        }
        emit forwardsChanged(rows);
    });
    m_forwards.start(m_impl->session, m_config.forwards);

    const Outcome outcome = pump();
    m_connected = false;
    // Before the channel and session go: every tunnel holds a channel on this
    // session, and freeing the session under them is a use-after-free on the
    // next poll.
    m_forwards.stop();
    emit forwardsChanged({});

    if (m_impl->channel != nullptr) {
        ssh_channel_send_eof(m_impl->channel);
        ssh_channel_close(m_impl->channel);
    }
    teardown();
    return outcome;
}

bool SshBackend::connectSession() {
    m_impl->session = ssh_new();
    if (m_impl->session == nullptr) {
        fail(ErrorCode::ConnectFailed, tr("Could not create an SSH session."));
        return false;
    }
    ssh_session session = m_impl->session;

    // The algorithm policy, from the one file that holds it (net.md). Applied
    // before anything else so nothing can negotiate outside it.
    ssh_options_set(session, SSH_OPTIONS_KEY_EXCHANGE, kKeyExchange);
    ssh_options_set(session, SSH_OPTIONS_CIPHERS_C_S, kCiphers);
    ssh_options_set(session, SSH_OPTIONS_CIPHERS_S_C, kCiphers);
    ssh_options_set(session, SSH_OPTIONS_HMAC_C_S, kMacs);
    ssh_options_set(session, SSH_OPTIONS_HMAC_S_C, kMacs);
    ssh_options_set(session, SSH_OPTIONS_HOSTKEYS, kHostKeys);
    ssh_options_set(session, SSH_OPTIONS_PUBLICKEY_ACCEPTED_TYPES, kPublicKeyTypes);

    ssh_options_set(session, SSH_OPTIONS_HOST, m_config.host.c_str());
    const int port = m_config.port;
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    if (!m_config.user.empty()) {
        ssh_options_set(session, SSH_OPTIONS_USER, m_config.user.c_str());
    }
    if (!m_config.knownHostsPath.empty()) {
        ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, m_config.knownHostsPath.c_str());
    }
    // T61. A user certificate that does not sit beside its key.
    //
    // This MUST be set before ssh_connect, and not because of the network.
    // SSH_OPTIONS_CERTIFICATE only appends to libssh's UNEXPANDED list
    // (options.c: opts.certificate_non_exp), while every auth path reads
    // opts.certificate; the only thing that moves one to the other is
    // ssh_options_apply(), which is internal and which ssh_connect calls on its
    // own (client.c). Set after ssh_connect this option is still accepted,
    // still returns SSH_OK, and is never read — a silent no-op, which is the
    // worst shape a security setting can have.
    if (!m_config.certPath.empty()) {
        ssh_options_set(session, SSH_OPTIONS_CERTIFICATE, m_config.certPath.c_str());
    }

    // Without this a dead host holds the worker until the OS gives up, which on
    // Windows is long past the point where the user closed the tab.
    const long timeout = m_config.connectTimeoutSeconds;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);

    // KRAIT_SSH_DEBUG=1..4 turns on libssh's own protocol log to stderr. An SSH
    // client without a way to see the handshake is a client where every
    // connection problem is a guess — and libssh's error strings alone
    // routinely name a symptom several layers below the cause.
    if (const QByteArray level = qgetenv("KRAIT_SSH_DEBUG"); !level.isEmpty()) {
        const int verbosity = level.toInt();
        ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    }

    // ADR-0012: native in-process ProxyJump, with OUR host-key and auth UX on
    // every hop. Set before ssh_connect, because ssh_connect is what walks the
    // chain — by the time it returns, every hop has already been contacted.
    if (!m_config.proxyJump.empty()) {
        ssh_options_set(session, SSH_OPTIONS_PROXYJUMP, m_config.proxyJump.c_str());
        // One callbacks struct per hop, appended in order. They are members
        // rather than locals because libssh keeps the POINTER: a struct on this
        // stack frame would be read long after ssh_connect returned.
        //
        // NOTE the struct has no `size` member, unlike libssh's other callback
        // structs — so there is no ssh_callbacks_init() to forget here.
        // Confirmed against callbacks.h in the pinned 0.12.0, which is the
        // check ADR-0012 left open.
        m_impl->jumpCallbacks.clear();
        m_impl->jumpCallbacks.resize(countProxyJumpHops(m_config.proxyJump));
        for (std::size_t hop = 0; hop < m_impl->jumpCallbacks.size(); ++hop) {
            auto& callbacks = m_impl->jumpCallbacks[hop];
            callbacks.userdata = this;
            callbacks.before_connection = nullptr;
            callbacks.verify_knownhost = &SshBackend::jumpVerifyHostKey;
            callbacks.authenticate = &SshBackend::jumpAuthenticate;
            ssh_options_set(session, SSH_OPTIONS_PROXYJUMP_CB_LIST_APPEND, &callbacks);
        }
    }

    if (ssh_connect(session) != SSH_OK) {
        // ssh_get_error is the server's or the resolver's text. It names a host
        // and a reason, never a credential, so it is safe in a banner.
        fail(ErrorCode::ConnectFailed, sanitizeRemoteText(nullSafe(ssh_get_error(session))));
        return false;
    }
    return true;
}

// libssh calls these on ITS thread, which is our worker thread — ssh_connect
// runs there and the whole chain is walked inside it. So the same rules apply
// as everywhere else in this file: prompts are queued to the GUI and the worker
// waits on the condition variable.
//
// Returning < 0 aborts the chain, which is what a refused key must do: a
// bastion whose key changed is exactly as fatal as the target's, and continuing
// past it would put the session's credentials through a machine nobody vouched
// for.
int SshBackend::jumpVerifyHostKey(ssh_session session, void* userdata) {
    auto* self = static_cast<SshBackend*>(userdata);
    const QString label = tr("%1 (jump host)").arg(hopLabel(session));
    return self->verifyHostKey(session, label) ? 0 : -1;
}

int SshBackend::jumpAuthenticate(ssh_session session, void* userdata) {
    auto* self = static_cast<SshBackend*>(userdata);
    return self->authenticate(session) ? 0 : -1;
}

bool SshBackend::verifyHostKey(void* sessionHandle, const QString& label) {
    auto session = static_cast<ssh_session>(sessionHandle);

    ssh_key serverKey = nullptr;
    if (ssh_get_server_publickey(session, &serverKey) != SSH_OK) {
        fail(ErrorCode::HostKeyRejected, tr("The server did not present a host key."));
        return false;
    }
    unsigned char* hash = nullptr;
    std::size_t hashLen = 0;
    const int hashRc =
        ssh_get_publickey_hash(serverKey, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLen);
    // Read BEFORE the key is freed. libssh 0.12 exposes no key-size accessor
    // (checked against the installed libssh.h — only ssh_key_type and
    // ssh_key_type_to_char), so the title carries the type without the bit
    // count that ssh-keygen -lv shows. The label differs; the ART does not,
    // and the art is what anyone actually compares.
    const char* keyType = ssh_key_type_to_char(ssh_key_type(serverKey));
    const QString title = QString::fromUtf8(keyType != nullptr ? keyType : "unknown");
    ssh_key_free(serverKey);
    if (hashRc != SSH_OK) {
        fail(ErrorCode::HostKeyRejected, tr("The server's host key could not be read."));
        return false;
    }

    QString fingerprint;
    if (char* text = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLen);
        text != nullptr) {
        fingerprint = QString::fromUtf8(text);
        ssh_string_free_char(text);
    }
    // Fingerprint AND picture. Nobody compares 43 base64 characters; people do
    // compare pictures, and this is the same picture ssh-keygen -lv draws, so
    // it can be checked against a value the user got somewhere else.
    const QString detail =
        fingerprint + QStringLiteral("\n") +
        QString::fromStdString(randomart(hash, hashLen, title.toStdString(), "SHA256"));
    ssh_clean_pubkey_hash(&hash);

    HostKeyState state = HostKeyState::Error;
    switch (ssh_session_is_known_server(session)) {
    case SSH_KNOWN_HOSTS_OK:
        return true;  // known and unchanged: no prompt, no ceremony
    case SSH_KNOWN_HOSTS_UNKNOWN:
        state = HostKeyState::Unknown;
        break;
    case SSH_KNOWN_HOSTS_NOT_FOUND:
        state = HostKeyState::NoFile;
        break;
    case SSH_KNOWN_HOSTS_CHANGED:
        state = HostKeyState::Changed;
        break;
    case SSH_KNOWN_HOSTS_OTHER:
        state = HostKeyState::OtherType;
        break;
    case SSH_KNOWN_HOSTS_ERROR:
    default:
        fail(ErrorCode::HostKeyRejected, sanitizeRemoteText(nullSafe(ssh_get_error(session))));
        return false;
    }

    if (state == HostKeyState::Changed || state == HostKeyState::OtherType) {
        // rules/net.md: a changed key is a BLOCKING banner, never a question.
        // The prompt is emitted so the UI can show what changed, but the
        // connection is already over — there is no answer that continues it,
        // and no setting anywhere that turns this into a yes/no.
        emit hostKeyPrompt(static_cast<int>(state), detail);
        fail(state == HostKeyState::Changed ? ErrorCode::HostKeyChanged
                                            : ErrorCode::HostKeyRejected,
             state == HostKeyState::Changed
                 ? tr("The host key for %1 has CHANGED. The connection was stopped.").arg(label)
                 : tr("%1 offered a host key of a different type than the one on record.")
                       .arg(label));
        return false;
    }

    // First contact: trust on first use, and only a human decides.
    //
    // Armed BEFORE the emit, and this is the site where it matters most. The
    // object survives the reconnect loop, and m_answered/m_hostKeyTrusted are
    // only ever cleared here — so without this, a cycle that already answered
    // ANY prompt (a password, or an earlier TOFU) leaves the flag set, the next
    // cycle's waitForAnswer returns instantly on the stale value, and
    // known_hosts gets written with no human in the loop.
    armAnswer();
    // `detail`, not `fingerprint`: this is the ONE prompt where the randomart
    // is the point. Nobody compares 43 base64 characters.
    emit hostKeyPrompt(static_cast<int>(state), detail);
    if (!waitForAnswer(kPromptTimeoutMs)) {
        fail(ErrorCode::HostKeyRejected, tr("No answer about the host key; connection stopped."));
        return false;
    }
    bool trusted = false;
    {
        const std::lock_guard lock(m_mutex);
        trusted = m_hostKeyTrusted;
    }
    if (!trusted) {
        fail(ErrorCode::HostKeyRejected, tr("The host key was not accepted."));
        return false;
    }
    if (ssh_session_update_known_hosts(session) != SSH_OK) {
        fail(ErrorCode::HostKeyRejected, tr("The host key could not be saved to known_hosts."));
        return false;
    }
    return true;
}

Secret SshBackend::askForSecret(const QString& prompt, bool echo, bool* remember) {
    armAnswer();  // before the emit; see armAnswer's note
    emit credentialPrompt(prompt, echo);
    if (!waitForAnswer(kPromptTimeoutMs)) {
        return {};
    }
    const std::lock_guard lock(m_mutex);
    if (remember != nullptr) {
        *remember = m_rememberCredential;
    }
    return std::move(m_credential);
}

int SshBackend::tryAgent() {
    // T60. libssh's agent client only ever connects an AF_UNIX socket, and the
    // agent that ships with Windows only ever listens on a named pipe, so on a
    // stock Windows box this used to reach nothing at all — and said so as
    // SSH_AUTH_DENIED, which is also what libssh returns when the server
    // refused every key. That is the whole reason it looked like it worked.
    //
    // SSH_AUTH_SOCK wins when it is set. Setting it is a deliberate act — a
    // developer pointing at a WSL or Git-Bash agent — while the named pipe is
    // simply there on every Windows machine, so overriding an explicit choice
    // with a default is the wrong way round.
    //
    // getenv_s and NOT GetEnvironmentVariableA, which reads a different table.
    // libssh's agent_connect uses getenv, and on MSVC that is the CRT's copy of
    // the environment; the Win32 call reads the live process block. Anything
    // doing SetEnvironmentVariable at runtime would desynchronise the two, and
    // the failure would be silent — we would skip the bridge because the
    // variable looks set while libssh sees nothing and finds no agent at all.
    // getenv_s answers with the size the value needs INCLUDING its terminator,
    // so 0 is unset and 1 is set-but-empty, and both mean the same thing here.
    std::size_t authSockLength = 0;
    ::getenv_s(&authSockLength, nullptr, 0, "SSH_AUTH_SOCK");
    const bool authSockSet = authSockLength > 1;
    if (!authSockSet && m_impl->agent.start()) {
        // ssh_set_agent_socket() only puts the fd in the session's agent
        // socket; libssh then closes it in ssh_free(), which is what
        // releaseSocket() is recording. Its one failure mode is a session with
        // no agent struct, and ssh_new() allocates that unconditionally.
        ssh_set_agent_socket(m_impl->session, static_cast<socket_t>(m_impl->agent.socket()));
        m_impl->agent.releaseSocket();
    }
    return ssh_userauth_agent(m_impl->session, nullptr);
}

int SshBackend::tryPublicKey() {
    ssh_session session = m_impl->session;

    if (m_config.keyPath.empty()) {
        // No key named: let libssh walk the agent and the default identities.
        // Passing a passphrase here would apply it to every key it tries, which
        // is how an account gets locked out on the third identity.
        return ssh_userauth_publickey_auto(session, nullptr, nullptr);
    }

    // A named key. Try it with no passphrase first — most keys on a Windows box
    // are unencrypted or already held by the agent, and asking for a passphrase
    // that is not needed teaches people to type it reflexively.
    ssh_key key = nullptr;
    int rc = ssh_pki_import_privkey_file(m_config.keyPath.c_str(), nullptr, nullptr, nullptr, &key);

    if (rc == SSH_ERROR) {
        // Encrypted, or unreadable. The vault first, then a prompt.
        Secret passphrase;
        bool remember = false;
        bool fromVault = false;
        if (m_vault != nullptr && !m_config.vaultKey.empty()) {
            fromVault = m_vault->retrieve(m_config.vaultKey + ":passphrase", &passphrase);
        }
        if (!fromVault) {
            passphrase =
                askForSecret(tr("Passphrase for %1").arg(QString::fromStdString(m_config.keyPath)),
                             false, &remember);
        }
        if (passphrase.empty()) {
            return SSH_AUTH_DENIED;
        }
        std::vector<char> nulTerminated(passphrase.size() + 1, '\0');
        std::memcpy(nulTerminated.data(), passphrase.data(), passphrase.size());
        rc = ssh_pki_import_privkey_file(m_config.keyPath.c_str(), nulTerminated.data(), nullptr,
                                         nullptr, &key);
        SecureZeroMemory(nulTerminated.data(), nulTerminated.size());

        if (rc == SSH_OK && remember && m_vault != nullptr && !m_config.vaultKey.empty()) {
            // Only once the key actually decrypted. A vault full of wrong
            // passphrases is worse than an empty one.
            m_vault->store(m_config.vaultKey + ":passphrase", passphrase);
            m_vault->save();
        }
    }
    if (rc != SSH_OK || key == nullptr) {
        // Say which file, because the alternative is silence. A path that
        // cannot be opened at all returns SSH_EOF rather than SSH_ERROR, so it
        // never reached the passphrase branch above — it simply falls out of
        // the ladder, and the user watches a profile with a key configured ask
        // for a password with no explanation.
        m_authHint =
            tr("The key file %1 could not be read.").arg(QString::fromStdString(m_config.keyPath));
        return SSH_AUTH_DENIED;
    }

    // T61. libssh 0.12 DECLARES the sk-* key types unconditionally but only
    // implements them when it was built WITH_FIDO2, and vcpkg's port is not
    // (verified in this tree: config.h leaves both WITH_FIDO2 and
    // HAVE_LIBFIDO2 undefined, and the portfile passes no such flag). So a
    // security key parses here and then fails to sign several layers down,
    // with a message about nothing the user wrote. ADR-0014 has the spike and
    // the way out; until then, say the true thing, which is that the agent
    // route works — the agent does the signing, so libssh never needs libfido2
    // for it.
    switch (ssh_key_type(key)) {
    case SSH_KEYTYPE_SK_ECDSA:
    case SSH_KEYTYPE_SK_ECDSA_CERT01:
    case SSH_KEYTYPE_SK_ED25519:
    case SSH_KEYTYPE_SK_ED25519_CERT01:
        m_authHint = tr("%1 is a FIDO2 security key. This build cannot use one from a file, but "
                        "it can use one held by the OpenSSH agent — run `ssh-add %1` and try "
                        "again.")
                         .arg(QString::fromStdString(m_config.keyPath));
        ssh_key_free(key);
        return SSH_AUTH_DENIED;
    default:
        break;
    }

    int auth = ssh_userauth_publickey(session, nullptr, key);

    // T61. A certificate, tried only after the plain key was refused. That is
    // libssh's own order in ssh_userauth_publickey_auto — the branch above —
    // and matching it keeps a named key behaving like a default one. It also
    // means a stale certificate beside a key that still works plainly costs
    // nothing, where offering the certificate first would have made it a
    // lockout.
    //
    // The configured path first, then the `<key>-cert.pub` sibling, which is
    // where ssh-keygen writes it and therefore where most people's is. An
    // absent or unreadable certificate is not an error worth reporting: the
    // key alone is still a credential.
    if (auth == SSH_AUTH_DENIED) {
        const std::string certPath =
            m_config.certPath.empty() ? m_config.keyPath + "-cert.pub" : m_config.certPath;
        ssh_key cert = nullptr;
        if (ssh_pki_import_cert_file(certPath.c_str(), &cert) == SSH_OK) {
            if (ssh_pki_copy_cert_to_privkey(cert, key) == SSH_OK) {
                auth = ssh_userauth_publickey(session, nullptr, key);
            }
            ssh_key_free(cert);
        }
    }

    ssh_key_free(key);
    return auth;
}

int SshBackend::tryKeyboardInteractive() {
    ssh_session session = m_impl->session;
    int rc = ssh_userauth_kbdint(session, nullptr, nullptr);

    // Bounded, because the loop is driven by the SERVER: a hostile one can keep
    // answering SSH_AUTH_INFO forever and turn the prompt into a denial of
    // service against the person sitting in front of it.
    constexpr int kMaxRounds = 8;
    constexpr unsigned int kMaxPromptsPerRound = 8;

    for (int round = 0; rc == SSH_AUTH_INFO && round < kMaxRounds; ++round) {
        const QString name = sanitizeRemoteText(nullSafe(ssh_userauth_kbdint_getname(session)));
        const QString instruction =
            sanitizeRemoteText(nullSafe(ssh_userauth_kbdint_getinstruction(session)));

        const int prompts = ssh_userauth_kbdint_getnprompts(session);
        if (prompts < 0) {
            return SSH_AUTH_ERROR;
        }
        const unsigned int count =
            std::min(static_cast<unsigned int>(prompts), kMaxPromptsPerRound);
        for (unsigned int i = 0; i < count; ++i) {
            char echo = 0;
            const QString prompt =
                sanitizeRemoteText(nullSafe(ssh_userauth_kbdint_getprompt(session, i, &echo)));

            // All three of these are SERVER text, already stripped of controls
            // and bounded. The UI shows them as plain text — Banner.qml learned
            // that in M1, when a pasted tag could restyle the warning about it.
            QString shown = prompt;
            if (!instruction.isEmpty()) {
                shown = instruction + QStringLiteral("\n") + shown;
            }
            if (!name.isEmpty()) {
                shown = name + QStringLiteral("\n") + shown;
            }

            Secret answer = askForSecret(shown, echo != 0, nullptr);
            if (answer.empty()) {
                return SSH_AUTH_DENIED;
            }
            std::vector<char> nulTerminated(answer.size() + 1, '\0');
            std::memcpy(nulTerminated.data(), answer.data(), answer.size());
            const int set = ssh_userauth_kbdint_setanswer(session, i, nulTerminated.data());
            SecureZeroMemory(nulTerminated.data(), nulTerminated.size());
            if (set < 0) {
                return SSH_AUTH_ERROR;
            }
        }
        rc = ssh_userauth_kbdint(session, nullptr, nullptr);
    }
    return rc == SSH_AUTH_INFO ? SSH_AUTH_DENIED : rc;
}

int SshBackend::tryPassword() {
    ssh_session session = m_impl->session;

    Secret password;
    bool remember = false;
    bool fromVault = false;
    if (m_vault != nullptr && !m_config.vaultKey.empty()) {
        fromVault = m_vault->retrieve(m_config.vaultKey + ":password", &password);
    }
    if (!fromVault) {
        password = askForSecret(
            tr("Password for %1@%2")
                .arg(QString::fromStdString(m_config.user), QString::fromStdString(m_config.host)),
            false, &remember);
    }
    if (password.empty()) {
        return SSH_AUTH_DENIED;
    }

    // libssh wants a NUL-terminated string; a Secret is sized bytes. The copy is
    // zeroed the moment the call returns.
    std::vector<char> nulTerminated(password.size() + 1, '\0');
    std::memcpy(nulTerminated.data(), password.data(), password.size());
    const int rc = ssh_userauth_password(session, nullptr, nulTerminated.data());
    SecureZeroMemory(nulTerminated.data(), nulTerminated.size());

    if (rc == SSH_AUTH_SUCCESS && remember && m_vault != nullptr && !m_config.vaultKey.empty()) {
        // Only after it WORKED. Storing a password the server rejected is how a
        // vault fills up with typos.
        m_vault->store(m_config.vaultKey + ":password", password);
        m_vault->save();
    }
    return rc;
}

bool SshBackend::authenticate(void* sessionHandle) {
    auto session = static_cast<ssh_session>(sessionHandle);

    // "none" first, because it is what makes ssh_userauth_list meaningful — and
    // occasionally it simply succeeds.
    const int none = ssh_userauth_none(session, nullptr);
    if (none == SSH_AUTH_SUCCESS) {
        return true;
    }
    if (none == SSH_AUTH_ERROR) {
        fail(ErrorCode::AuthFailed, sanitizeRemoteText(nullSafe(ssh_get_error(session))));
        return false;
    }
    const int offered = ssh_userauth_list(session, nullptr);
    // Cleared per attempt: a reconnect after the user moved the key must not
    // still be explaining the last cycle's problem.
    m_authHint.clear();

    // The ladder. Auto is agent, then keys, then the two interactive methods —
    // cheapest and least annoying first, so the common case (an agent that
    // already holds the key) never prompts at all. A profile that NAMES a
    // method gets only that method: "use the agent" has to mean it, or a broken
    // agent silently degrades into typing a password every day and nobody
    // notices it broke.
    struct Step {
        int method;  // the SSH_AUTH_METHOD_* bit the server must offer
        int (SshBackend::*run)();
    };

    std::vector<Step> ladder;
    switch (m_config.auth) {
    case SshAuthPreference::Agent:
        ladder = {{SSH_AUTH_METHOD_PUBLICKEY, &SshBackend::tryAgent}};
        break;
    case SshAuthPreference::PublicKey:
        ladder = {{SSH_AUTH_METHOD_PUBLICKEY, &SshBackend::tryPublicKey}};
        break;
    case SshAuthPreference::Password:
        ladder = {{SSH_AUTH_METHOD_PASSWORD, &SshBackend::tryPassword}};
        break;
    case SshAuthPreference::KeyboardInteractive:
        ladder = {{SSH_AUTH_METHOD_INTERACTIVE, &SshBackend::tryKeyboardInteractive}};
        break;
    case SshAuthPreference::Auto:
        ladder = {{SSH_AUTH_METHOD_PUBLICKEY, &SshBackend::tryAgent},
                  {SSH_AUTH_METHOD_PUBLICKEY, &SshBackend::tryPublicKey},
                  {SSH_AUTH_METHOD_INTERACTIVE, &SshBackend::tryKeyboardInteractive},
                  {SSH_AUTH_METHOD_PASSWORD, &SshBackend::tryPassword}};
        break;
    }

    for (const Step& step : ladder) {
        if ((offered & step.method) == 0) {
            continue;  // the server will not take it; do not waste the attempt
        }
        if (m_shutdown.load()) {
            return false;  // the tab closed mid-prompt
        }
        const int rc = (this->*step.run)();
        if (rc == SSH_AUTH_SUCCESS) {
            return true;
        }
        if (rc == SSH_AUTH_ERROR) {
            fail(ErrorCode::AuthFailed, sanitizeRemoteText(nullSafe(ssh_get_error(session))));
            return false;
        }
        // SSH_AUTH_PARTIAL means the server wants ANOTHER factor, which is
        // exactly what the next rung is for, so it falls through like a denial.
    }

    // A rung that knows WHY it could not work says so here. Without the hint
    // this line is true and useless: it names the host and tells the user
    // nothing they can act on.
    QString message = tr("%1 refused every authentication method we could offer.")
                          .arg(QString::fromStdString(m_config.host));
    if (!m_authHint.isEmpty()) {
        message += QLatin1Char(' ') + m_authHint;
    }
    fail(ErrorCode::AuthFailed, message);
    return false;
}

bool SshBackend::openShell(int cols, int rows) {
    ssh_session session = m_impl->session;
    m_impl->channel = ssh_channel_new(session);
    if (m_impl->channel == nullptr) {
        fail(ErrorCode::ConnectFailed, tr("Could not open a channel."));
        return false;
    }
    ssh_channel channel = m_impl->channel;

    if (ssh_channel_open_session(channel) != SSH_OK) {
        fail(ErrorCode::ConnectFailed, sanitizeRemoteText(nullSafe(ssh_get_error(session))));
        return false;
    }
    // A zero-sized pty is what an item asks for before its geometry is applied
    // (the ItemSceneChange lesson from T26). Clamp rather than pass it on: some
    // servers accept it and then wrap every line at column 0.
    const int safeCols = cols > 0 ? cols : 80;
    const int safeRows = rows > 0 ? rows : 24;
    if (ssh_channel_request_pty_size(channel, m_config.termType.c_str(), safeCols, safeRows) !=
        SSH_OK) {
        fail(ErrorCode::ConnectFailed, sanitizeRemoteText(nullSafe(ssh_get_error(session))));
        return false;
    }
    if (ssh_channel_request_shell(channel) != SSH_OK) {
        fail(ErrorCode::ConnectFailed, sanitizeRemoteText(nullSafe(ssh_get_error(session))));
        return false;
    }
    return true;
}

SshBackend::Outcome SshBackend::pump() {
    ssh_channel channel = m_impl->channel;
    std::vector<char> buffer(kReadChunk);
    // Idle detection. An SSH session behind NAT dies silently otherwise, and
    // the user finds out by typing into a connection that has been dead for
    // twenty minutes.
    auto lastTraffic = std::chrono::steady_clock::now();

    while (!m_shutdown.load()) {
        // Writes and resizes first: they are only allowed to touch the session
        // from THIS thread, so the queue is the only way in.
        std::deque<QByteArray> pending;
        bool resize = false;
        int cols = 0;
        int rows = 0;
        {
            const std::lock_guard lock(m_mutex);
            pending.swap(m_writeQueue);
            resize = m_resizePending;
            cols = m_pendingCols;
            rows = m_pendingRows;
            m_resizePending = false;
        }
        for (const QByteArray& bytes : pending) {
            qsizetype written = 0;
            while (written < bytes.size() && !m_shutdown.load()) {
                const int n = ssh_channel_write(channel, bytes.constData() + written,
                                                static_cast<std::uint32_t>(bytes.size() - written));
                // n < 0, not n == SSH_ERROR: SSH_AGAIN is -2, and `written += n`
                // would then walk BACKWARDS through the buffer with a length
                // that grows. Unreachable while the session stays blocking, but
                // that is an invariant of our own code rather than a promise
                // libssh makes.
                if (n < 0) {
                    fail(ErrorCode::IoFailed,
                         sanitizeRemoteText(nullSafe(ssh_get_error(m_impl->session))));
                    return Outcome::Failed;
                }
                written += n;
            }
            // Our own traffic counts: a session someone is typing into does not
            // need a keepalive on top of it.
            lastTraffic = std::chrono::steady_clock::now();
        }
        if (resize) {
            ssh_channel_change_pty_size(channel, cols, rows);
        }

        const int n = ssh_channel_read_timeout(
            channel, buffer.data(), static_cast<std::uint32_t>(buffer.size()), 0, kPollMs);
        if (n == SSH_ERROR) {
            fail(ErrorCode::IoFailed, sanitizeRemoteText(nullSafe(ssh_get_error(m_impl->session))));
            return Outcome::Failed;
        }
        if (n > 0) {
            lastTraffic = std::chrono::steady_clock::now();
            emit outputReceived(QByteArray(buffer.data(), n));
        }

        // stderr, non-blocking. With a pty the server normally merges it, but
        // "normally" is not "always", and an unread stream fills its window and
        // stalls the whole channel.
        const int errBytes = ssh_channel_read_nonblocking(
            channel, buffer.data(), static_cast<std::uint32_t>(buffer.size()), 1);
        if (errBytes > 0) {
            emit outputReceived(QByteArray(buffer.data(), errBytes));
        }

        // Tunnels, on the SAME thread as everything else that touches the
        // session. Serviced after the shell read so the terminal keeps
        // priority; the 20 ms poll bounds how long a tunnel waits.
        m_forwards.service(m_impl->session);

        if (ssh_channel_is_eof(channel) != 0) {
            // The remote shell ended. That is an exit, not an error — telling
            // someone their connection "failed" when they typed exit is how a
            // banner trains people to ignore banners (error.h).
            emit exited(ssh_channel_get_exit_status(channel));
            return Outcome::CleanExit;
        }

        if (m_config.keepaliveSeconds > 0) {
            const auto idle = std::chrono::steady_clock::now() - lastTraffic;
            if (idle >= std::chrono::seconds(m_config.keepaliveSeconds)) {
                // A failure here is the peer being gone, which is exactly what
                // the keepalive is for: report it and let run() decide whether
                // to reconnect.
                if (ssh_send_keepalive(m_impl->session) != SSH_OK) {
                    fail(ErrorCode::PeerClosed,
                         tr("%1 stopped responding.").arg(QString::fromStdString(m_config.host)));
                    return Outcome::Failed;
                }
                lastTraffic = std::chrono::steady_clock::now();
            }
        }
    }
    return Outcome::Stopped;
}

void SshBackend::writeInput(const QByteArray& bytes) {
    if (bytes.isEmpty()) {
        return;
    }
    {
        const std::lock_guard lock(m_mutex);
        m_writeQueue.push_back(bytes);
    }
    m_cv.notify_all();
}

void SshBackend::resize(int cols, int rows) {
    if (cols <= 0 || rows <= 0) {
        return;
    }
    const std::lock_guard lock(m_mutex);
    m_pendingCols = cols;
    m_pendingRows = rows;
    m_resizePending = true;
}

void SshBackend::stop() {
    if (!m_started) {
        return;  // idempotent (rules/net.md)
    }
    m_started = false;
    m_shutdown = true;
    {
        // A password typed after the prompt timed out has nothing left to
        // consume it. Wipe it rather than carry it to the destructor.
        const std::lock_guard lock(m_mutex);
        m_credential.clear();
    }
    // Releases a worker parked in waitForAnswer. Without this, closing a tab
    // during a host-key prompt leaves that thread sitting there for five
    // minutes and the join below waits with it.
    m_cv.notify_all();
    // T60, and the same shape one rung along. The worker can also be parked
    // inside ssh_userauth_agent, which blocks in recv() on the socket the
    // bridge handed libssh, for as long as the AGENT takes to answer — for a
    // FIDO2 key that is "until somebody touches it", and for a wedged agent
    // service it is forever. m_shutdown is only read between rungs of the auth
    // ladder, so it does not reach that wait; ending the relay does, because
    // the relay shutting its side of the pair makes libssh's recv() return 0.
    //
    // cancel() and not stop(): stop() would join the relay from here, and this
    // is the GUI thread.
    m_impl->agent.cancel();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_connected = false;
}

void SshBackend::armAnswer() {
    const std::lock_guard lock(m_mutex);
    m_answered = false;
    m_hostKeyTrusted = false;
    // Any answer still sitting here belongs to a prompt that is over. Keeping
    // it would leave a password in memory for the object's lifetime, which is
    // exactly what net.md's "zero after use" forbids — and would let a stale
    // answer satisfy the NEXT prompt.
    m_credential.clear();
}

bool SshBackend::waitForAnswer(int timeoutMs) {
    std::unique_lock lock(m_mutex);
    const bool woke = m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                    [this] { return m_answered || m_shutdown.load(); });
    return woke && m_answered;
}

void SshBackend::respondHostKey(bool trust) {
    {
        const std::lock_guard lock(m_mutex);
        m_hostKeyTrusted = trust;
        m_answered = true;
    }
    m_cv.notify_all();
}

void SshBackend::respondCredential(const QString& text, bool remember) {
    {
        const std::lock_guard lock(m_mutex);
        QByteArray utf8 = text.toUtf8();
        m_credential = Secret(std::string_view(utf8.constData(), utf8.size()));
        SecureZeroMemory(utf8.data(), static_cast<std::size_t>(utf8.size()));
        m_rememberCredential = remember;
        m_answered = true;
    }
    m_cv.notify_all();
}

}  // namespace krait::net

#include "ssh_backend.h"

#include "hostkey_art.h"

#include <libssh/libssh.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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
constexpr std::size_t kReadChunk = 16 * 1024;

// A human has this long to answer a host-key or password prompt before the
// connection gives up. rules/cpp.md: every wait has a timeout.
constexpr int kPromptTimeoutMs = 5 * 60 * 1000;

}  // namespace

// libssh handles live here so libssh.h never reaches a header of ours.
struct SshBackend::Impl {
    ssh_session session = nullptr;
    ssh_channel channel = nullptr;
};

SshBackend::SshBackend(SshConfig config, Vault* vault, QObject* parent)
    : IBackend(parent), m_config(std::move(config)), m_vault(vault),
      m_impl(std::make_unique<Impl>()) {}

SshBackend::~SshBackend() {
    stop();
}

void SshBackend::fail(ErrorCode code, const QString& message) {
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
    if (!connectSession() || !verifyHostKey() || !authenticate() || !openShell(cols, rows)) {
        // Every failure path has already emitted its own error, with the code
        // that names what went wrong.
        if (m_impl->channel != nullptr) {
            ssh_channel_free(m_impl->channel);
            m_impl->channel = nullptr;
        }
        if (m_impl->session != nullptr) {
            ssh_disconnect(m_impl->session);
            ssh_free(m_impl->session);
            m_impl->session = nullptr;
        }
        return;
    }

    m_connected = true;
    emit connected();
    pump();

    m_connected = false;
    if (m_impl->channel != nullptr) {
        ssh_channel_send_eof(m_impl->channel);
        ssh_channel_close(m_impl->channel);
        ssh_channel_free(m_impl->channel);
        m_impl->channel = nullptr;
    }
    if (m_impl->session != nullptr) {
        ssh_disconnect(m_impl->session);
        ssh_free(m_impl->session);
        m_impl->session = nullptr;
    }
}

bool SshBackend::connectSession() {
    m_impl->session = ssh_new();
    if (m_impl->session == nullptr) {
        fail(ErrorCode::ConnectFailed, tr("Could not create an SSH session."));
        return false;
    }
    ssh_session session = m_impl->session;

    ssh_options_set(session, SSH_OPTIONS_HOST, m_config.host.c_str());
    const int port = m_config.port;
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    if (!m_config.user.empty()) {
        ssh_options_set(session, SSH_OPTIONS_USER, m_config.user.c_str());
    }
    if (!m_config.knownHostsPath.empty()) {
        ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, m_config.knownHostsPath.c_str());
    }
    // Without this a dead host holds the worker until the OS gives up, which on
    // Windows is long past the point where the user closed the tab.
    const long timeout = m_config.connectTimeoutSeconds;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);

    if (ssh_connect(session) != SSH_OK) {
        // ssh_get_error is the server's or the resolver's text. It names a host
        // and a reason, never a credential, so it is safe in a banner.
        fail(ErrorCode::ConnectFailed, QString::fromUtf8(ssh_get_error(session)));
        return false;
    }
    return true;
}

bool SshBackend::verifyHostKey() {
    ssh_session session = m_impl->session;

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
        fail(ErrorCode::HostKeyRejected, QString::fromUtf8(ssh_get_error(session)));
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
                 ? tr("The host key for %1 has CHANGED. The connection was stopped.")
                       .arg(QString::fromStdString(m_config.host))
                 : tr("%1 offered a host key of a different type than the one on record.")
                       .arg(QString::fromStdString(m_config.host)));
        return false;
    }

    // First contact: trust on first use, and only a human decides.
    emit hostKeyPrompt(static_cast<int>(state), fingerprint);
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

bool SshBackend::authenticate() {
    ssh_session session = m_impl->session;

    // "none" first, because it is what makes ssh_userauth_list meaningful — and
    // occasionally it simply succeeds.
    const int none = ssh_userauth_none(session, nullptr);
    if (none == SSH_AUTH_SUCCESS) {
        return true;
    }
    if (none == SSH_AUTH_ERROR) {
        fail(ErrorCode::AuthFailed, QString::fromUtf8(ssh_get_error(session)));
        return false;
    }
    const int methods = ssh_userauth_list(session, nullptr);

    // Public key, which is also how the agent gets its turn: publickey_auto
    // walks the agent first and then the default identities.
    if ((methods & SSH_AUTH_METHOD_PUBLICKEY) != 0) {
        const int rc = ssh_userauth_publickey_auto(session, nullptr, nullptr);
        if (rc == SSH_AUTH_SUCCESS) {
            return true;
        }
        if (rc == SSH_AUTH_ERROR) {
            fail(ErrorCode::AuthFailed, QString::fromUtf8(ssh_get_error(session)));
            return false;
        }
    }

    if ((methods & SSH_AUTH_METHOD_PASSWORD) != 0) {
        Secret password;
        bool remember = false;
        bool fromVault = false;
        if (m_vault != nullptr && !m_config.vaultKey.empty()) {
            fromVault = m_vault->retrieve(m_config.vaultKey + ":password", &password);
        }
        if (!fromVault) {
            emit credentialPrompt(tr("Password for %1@%2")
                                      .arg(QString::fromStdString(m_config.user),
                                           QString::fromStdString(m_config.host)),
                                  false);
            if (!waitForAnswer(kPromptTimeoutMs)) {
                fail(ErrorCode::AuthFailed, tr("No password was given."));
                return false;
            }
            const std::lock_guard lock(m_mutex);
            password = std::move(m_credential);
            remember = m_rememberCredential;
        }
        if (password.empty()) {
            fail(ErrorCode::AuthFailed, tr("Authentication was cancelled."));
            return false;
        }

        // libssh wants a NUL-terminated string; a Secret is sized bytes. The
        // copy is zeroed the moment the call returns.
        std::vector<char> nulTerminated(password.size() + 1, '\0');
        std::memcpy(nulTerminated.data(), password.data(), password.size());
        const int rc = ssh_userauth_password(session, nullptr, nulTerminated.data());
        SecureZeroMemory(nulTerminated.data(), nulTerminated.size());

        if (rc == SSH_AUTH_SUCCESS) {
            if (remember && m_vault != nullptr && !m_config.vaultKey.empty()) {
                // Only after it WORKED. Storing a password the server rejected
                // is how a vault fills up with typos.
                m_vault->store(m_config.vaultKey + ":password", password);
                m_vault->save();
            }
            return true;
        }
    }

    fail(ErrorCode::AuthFailed, tr("%1 refused every authentication method we could offer.")
                                    .arg(QString::fromStdString(m_config.host)));
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
        fail(ErrorCode::ConnectFailed, QString::fromUtf8(ssh_get_error(session)));
        return false;
    }
    // A zero-sized pty is what an item asks for before its geometry is applied
    // (the ItemSceneChange lesson from T26). Clamp rather than pass it on: some
    // servers accept it and then wrap every line at column 0.
    const int safeCols = cols > 0 ? cols : 80;
    const int safeRows = rows > 0 ? rows : 24;
    if (ssh_channel_request_pty_size(channel, m_config.termType.c_str(), safeCols, safeRows) !=
        SSH_OK) {
        fail(ErrorCode::ConnectFailed, QString::fromUtf8(ssh_get_error(session)));
        return false;
    }
    if (ssh_channel_request_shell(channel) != SSH_OK) {
        fail(ErrorCode::ConnectFailed, QString::fromUtf8(ssh_get_error(session)));
        return false;
    }
    return true;
}

void SshBackend::pump() {
    ssh_channel channel = m_impl->channel;
    std::vector<char> buffer(kReadChunk);

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
                if (n == SSH_ERROR) {
                    fail(ErrorCode::IoFailed, QString::fromUtf8(ssh_get_error(m_impl->session)));
                    return;
                }
                written += n;
            }
        }
        if (resize) {
            ssh_channel_change_pty_size(channel, cols, rows);
        }

        const int n = ssh_channel_read_timeout(
            channel, buffer.data(), static_cast<std::uint32_t>(buffer.size()), 0, kPollMs);
        if (n == SSH_ERROR) {
            fail(ErrorCode::IoFailed, QString::fromUtf8(ssh_get_error(m_impl->session)));
            return;
        }
        if (n > 0) {
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

        if (ssh_channel_is_eof(channel) != 0) {
            // The remote shell ended. That is an exit, not an error — telling
            // someone their connection "failed" when they typed exit is how a
            // banner trains people to ignore banners (error.h).
            emit exited(ssh_channel_get_exit_status(channel));
            return;
        }
    }
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
    // Releases a worker parked in waitForAnswer. Without this, closing a tab
    // during a host-key prompt leaves that thread sitting there for five
    // minutes and the join below waits with it.
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_connected = false;
}

bool SshBackend::waitForAnswer(int timeoutMs) {
    std::unique_lock lock(m_mutex);
    m_answered = false;
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

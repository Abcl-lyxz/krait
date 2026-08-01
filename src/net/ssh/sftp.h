#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// At GLOBAL scope for the same reason ssh_backend.h forward-declares
// ssh_session_struct there: written inside the namespace this would declare a
// NEW type called krait::net::sftp_session_struct, which then does not match
// libssh's. Forward-declaring it keeps libssh/sftp.h in the .cpp without lying
// about the type.
struct sftp_session_struct;

namespace krait::net {

// One entry of a remote directory, already stripped of everything libssh's
// sftp_attributes carries that the app layer has no business seeing — and of
// everything a hostile server might have put in it (see Sftp::listDir).
struct SftpEntry {
    std::string name;
    std::uint64_t size = 0;
    // POSIX mode bits as the server reported them, 0 when it did not. SFTP v3
    // (what libssh negotiates) is allowed to leave this out.
    std::uint32_t permissions = 0;
    // Seconds since the epoch; 0 when the server did not say.
    std::int64_t mtime = 0;
    bool isDir = false;
    bool isLink = false;
};

// The SFTP subsystem over an already-authenticated ssh_session.
//
// THREAD AFFINITY: every method must run on the thread that owns the
// ssh_session. libssh is explicit that a session and its channels cannot be
// touched from two threads at once — the failure mode is internal state
// corruption, not an error return. In Krait that thread is the one SshBackend
// starts, which is also why this class does no locking of its own: there is
// nothing to lock against.
//
// The session must be in BLOCKING mode. libssh 0.12's sftp.h says so outright
// ("most sftp_* functions do not support the non-blocking API"), and Krait's
// session is blocking already.
class Sftp {
  public:
    Sftp() = default;
    ~Sftp();
    Sftp(const Sftp&) = delete;
    Sftp& operator=(const Sftp&) = delete;

    // Called after every chunk of a transfer with (bytes so far, total; total
    // is 0 when unknown). Returning false CANCELS the transfer.
    //
    // It is also the interleave hook, and that is not incidental. sftp_read and
    // sftp_write block, and they share a session with the interactive shell, so
    // a 200 MB download would otherwise freeze the terminal for its whole
    // duration. SshBackend passes a callback that drains the shell channel
    // between chunks; the chunk size is what bounds how long the terminal
    // waits.
    using Progress = std::function<bool(std::uint64_t done, std::uint64_t total)>;

    // Opens the subsystem on `session` (an ssh_session; void* so libssh stays
    // out of this header). False leaves lastError() set.
    bool open(void* session);
    // Idempotent.
    void close();

    bool isOpen() const { return m_sftp != nullptr; }

    // Resolves `path` against the login directory — "." is how you ask where
    // that is, which is the first thing a file panel needs.
    bool realpath(const std::string& path, std::string* out);

    // Directory listing, sorted directories-first then by name. `.` and `..`
    // are dropped: the panel navigates with its own model of where it is, and
    // a server that says `..` is a directory named `..` is not telling us
    // anything we did not already know.
    bool listDir(const std::string& path, std::vector<SftpEntry>* out);

    // stat, following symlinks. `out->name` is left empty — the server only
    // fills a name in for directory reads, and inventing one from the path
    // would make an empty answer look like a real one.
    bool stat(const std::string& path, SftpEntry* out);

    // Downloads `remote` to `localPath`, truncating it. On failure or cancel
    // the partial local file is REMOVED: a half-written file with the right
    // name is worse than no file, because the next thing to read it cannot
    // tell.
    bool get(const std::string& remote, const std::string& localPath, const Progress& progress);

    // Uploads `localPath` to `remote`, truncating it. The remote partial is
    // left in place — deleting a file on someone else's machine because our
    // transfer failed is a bigger decision than this class gets to make, and
    // the caller knows whether it was overwriting something.
    bool put(const std::string& localPath, const std::string& remote, const Progress& progress);

    // Human-readable, never carrying a secret (rules/net.md). Remote-supplied
    // text in here has already been through sanitizeRemoteText.
    const std::string& lastError() const { return m_error; }

    // True when the last false return was the Progress callback saying stop,
    // rather than anything going wrong. The caller needs the difference: a
    // cancelled transfer is not an error banner, and showing one for a button
    // the user just pressed is how banners get ignored.
    bool cancelled() const { return m_cancelled; }

    // A directory bigger than this is refused rather than truncated. Remote
    // input is hostile (rules/net.md) and a listing is the cheapest way for a
    // server to make a client allocate without bound. Refusing beats silently
    // truncating: a short listing that looks complete is how someone concludes
    // a file is not there.
    static constexpr std::size_t kMaxEntries = 65536;

  private:
    // Sets m_error from libssh's own text for `context`. Split out because
    // getting it wrong is easy: sftp_get_error() explains an SFTP-level refusal
    // (no such file) and ssh_get_error() explains a transport-level one, and
    // only one of them is meaningful at a time.
    void failFromLibssh(const char* context);

    sftp_session_struct* m_sftp = nullptr;
    // Borrowed: the ssh_session outlives this object, and freeing it here would
    // pull the shell channel down with it.
    void* m_session = nullptr;
    std::string m_error;
    bool m_cancelled = false;
    // What sftp_limits() reported, clamped. Queried once at open(): libssh 0.12
    // caps every read and write at the server's stated maximum, so a bigger
    // buffer than this is memory that can never be filled.
    std::size_t m_chunk = 0;
};

}  // namespace krait::net

#include "sftp.h"

#include "../remote_text.h"

// Before libssh, which pulls in windows.h: without this the min/max MACROS
// eat std::min<uint64_t>( and the error names a token, not the cause. Same
// guard as ssh_backend.cpp and every other Windows-touching file here.
#define NOMINMAX

#include <fcntl.h>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace krait::net {

namespace {

// Chunk size for transfers, clamped from what the server says it will accept.
//
// The ceiling is not about memory. sftp_read blocks, and it shares a session
// with the interactive shell, so the chunk size IS the worst-case latency the
// terminal sees during a transfer — one 4 MB read on a slow link is a terminal
// that ignores keystrokes for several seconds. 64 KiB is a round trip the user
// cannot feel and still fills a fast link.
constexpr std::size_t kMinChunk = 16 * 1024;
constexpr std::size_t kMaxChunk = 64 * 1024;

// What an SFTP status code means, in words a person can act on. libssh's own
// ssh_get_error() for these says "sftp server: no such file", which reads like
// an internal log line.
const char* sftpErrorText(int code) {
    switch (code) {
    case SSH_FX_NO_SUCH_FILE:
    case SSH_FX_NO_SUCH_PATH:
        return "No such file or directory on the server.";
    case SSH_FX_PERMISSION_DENIED:
        return "The server refused: permission denied.";
    case SSH_FX_OP_UNSUPPORTED:
        return "The server does not support that operation.";
    case SSH_FX_FILE_ALREADY_EXISTS:
        return "That name already exists on the server.";
    case SSH_FX_WRITE_PROTECT:
        return "The remote file system is read-only.";
    case SSH_FX_NO_CONNECTION:
    case SSH_FX_CONNECTION_LOST:
        return "The SFTP connection was lost.";
    case SSH_FX_BAD_MESSAGE:
        return "The server sent a malformed SFTP message.";
    case SSH_FX_NO_MEDIA:
        return "The remote drive has no media.";
    default:
        // SSH_FX_FAILURE and anything newer: libssh's own text is more specific
        // than a sentence we could write here.
        return nullptr;
    }
}

// Whether a name the SERVER chose is safe to use as one path component.
//
// rules/net.md: all remote input is hostile. A directory entry named
// "../../.ssh/authorized_keys" is not a filename, it is a download destination
// chosen by the server — and the panel composes local paths from these. Names
// with a separator or an embedded control character are dropped rather than
// escaped: there is nothing useful to show a user for either, and an escaped
// version invites someone to "fix" it later by unescaping.
bool isSafeName(const char* name) {
    if (name == nullptr || *name == '\0') {
        return false;
    }
    for (const char* p = name; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

// Directories first, then by name. Case-insensitive over ASCII only: real
// collation belongs to the panel's locale, not to the byte layer, and folding
// just the ASCII range inside a UTF-8 string is safe because no continuation
// byte ever falls in it.
bool entryLess(const SftpEntry& a, const SftpEntry& b) {
    if (a.isDir != b.isDir) {
        return a.isDir;
    }
    const auto fold = [](char raw) {
        const unsigned char c = static_cast<unsigned char>(raw);
        return static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    };
    return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                                        [&fold](char l, char r) { return fold(l) < fold(r); });
}

void fillEntry(sftp_attributes attr, SftpEntry* out) {
    out->size = attr->size;
    out->permissions = attr->permissions;
    // mtime64 is the v4+ field and mtime the v3 one; libssh negotiates v3, so
    // mtime is normally the one that is set. Preferring whichever is non-zero
    // costs nothing and stops a v4 server from showing 1970.
    out->mtime = attr->mtime64 != 0 ? static_cast<std::int64_t>(attr->mtime64)
                                    : static_cast<std::int64_t>(attr->mtime);
    out->isDir = attr->type == SSH_FILEXFER_TYPE_DIRECTORY;
    out->isLink = attr->type == SSH_FILEXFER_TYPE_SYMLINK;
}

}  // namespace

Sftp::~Sftp() {
    close();
}

void Sftp::failFromLibssh(const char* context) {
    m_error = context;
    m_error += ' ';
    // sftp_get_error() explains an SFTP-level refusal and ssh_get_error() a
    // transport-level one, and only one of them is meaningful at a time. The
    // SFTP code wins when it has something specific to say; otherwise libssh's
    // own string still beats a generic sentence.
    const int code = m_sftp != nullptr ? sftp_get_error(m_sftp) : SSH_FX_FAILURE;
    if (const char* text = sftpErrorText(code); text != nullptr) {
        m_error += text;
        return;
    }
    const char* raw =
        m_session != nullptr ? ssh_get_error(static_cast<ssh_session>(m_session)) : nullptr;
    if (raw != nullptr && *raw != '\0') {
        m_error += sanitizeRemoteText(raw).toStdString();
    } else {
        m_error += "The server refused the request.";
    }
}

bool Sftp::open(void* session) {
    close();
    m_cancelled = false;
    m_session = session;
    if (session == nullptr) {
        m_error = "No SSH session to open SFTP on.";
        return false;
    }
    m_sftp = sftp_new(static_cast<ssh_session>(session));
    if (m_sftp == nullptr) {
        failFromLibssh("Could not open the SFTP subsystem.");
        return false;
    }
    if (sftp_init(m_sftp) != SSH_OK) {
        // A server with SFTP switched off fails HERE rather than at sftp_new:
        // the channel opens and the subsystem request is what gets refused.
        // Saying so is the difference between a user enabling sftp on their
        // server and a user filing a bug against Krait.
        failFromLibssh("The server did not start an SFTP session.");
        // Deliberately NOT close(): m_session has to survive for the caller's
        // error path, and sftp_free alone is what needs undoing.
        sftp_free(m_sftp);
        m_sftp = nullptr;
        return false;
    }

    m_chunk = kMaxChunk;
    if (sftp_limits_t limits = sftp_limits(m_sftp); limits != nullptr) {
        const std::uint64_t smallest = std::min(limits->max_read_length, limits->max_write_length);
        if (smallest > 0) {
            const std::uint64_t capped = std::min<std::uint64_t>(smallest, kMaxChunk);
            m_chunk = std::max(static_cast<std::size_t>(capped), kMinChunk);
        }
        sftp_limits_free(limits);
    }
    return true;
}

void Sftp::close() {
    if (m_sftp != nullptr) {
        sftp_free(m_sftp);
        m_sftp = nullptr;
    }
    m_session = nullptr;
}

bool Sftp::realpath(const std::string& path, std::string* out) {
    if (m_sftp == nullptr) {
        m_error = "SFTP is not open.";
        return false;
    }
    // Cleared even though nothing here can set it: cancelled() is what the
    // caller uses to decide between a banner and silence, and a leftover true
    // from the transfer before this one would swallow a real error.
    m_cancelled = false;
    char* resolved = sftp_canonicalize_path(m_sftp, path.c_str());
    if (resolved == nullptr) {
        failFromLibssh("Could not resolve that path.");
        return false;
    }
    // The server chose this string. It is a path rather than a name, so
    // separators are legal and isSafeName does not apply — but control
    // characters in it would still land in a UI label.
    *out = sanitizeRemoteText(resolved, 4096, 1).toStdString();
    ssh_string_free_char(resolved);
    return true;
}

bool Sftp::listDir(const std::string& path, std::vector<SftpEntry>* out, const Progress& progress) {
    if (m_sftp == nullptr) {
        m_error = "SFTP is not open.";
        return false;
    }
    m_cancelled = false;
    sftp_dir dir = sftp_opendir(m_sftp, path.c_str());
    if (dir == nullptr) {
        failFromLibssh("Could not open that directory.");
        return false;
    }

    std::vector<SftpEntry> entries;
    bool overflowed = false;
    // Counts ITERATIONS, not stored entries. Bounding only what we keep bounds
    // nothing: a server that streams names we reject — a separator, a control
    // byte, "." forever — leaves entries.size() at zero and this loop never
    // ends. It runs on the one ssh worker thread and stop() joins that thread,
    // so that is not a slow listing, it is the whole app hung by a peer.
    std::size_t seen = 0;
    while (sftp_attributes attr = sftp_readdir(m_sftp, dir)) {
        if (++seen > kMaxEntries) {
            sftp_attributes_free(attr);
            overflowed = true;
            break;
        }
        const char* name = attr->name;
        const bool dot =
            name != nullptr && (std::string_view(name) == "." || std::string_view(name) == "..");
        if (!dot && isSafeName(name)) {
            SftpEntry entry;
            entry.name = name;
            fillEntry(attr, &entry);
            entries.push_back(std::move(entry));
        }
        sftp_attributes_free(attr);
        // After the free, so a cancel cannot leak the attributes it stopped on.
        // This is the ONLY place a listing can be interrupted: sftp_readdir
        // blocks, exactly like sftp_read does in get().
        if (progress && !progress(seen, 0)) {
            m_cancelled = true;
            m_error = "Cancelled.";
            sftp_closedir(dir);
            return false;
        }
    }
    // sftp_readdir returning NULL is both "done" and "failed", and the two have
    // to reach the user differently: a listing that stopped halfway because the
    // connection died must not look like a directory with fewer files in it.
    const bool complete = sftp_dir_eof(dir) != 0;
    sftp_closedir(dir);

    if (overflowed) {
        m_error = "That directory holds more than 65536 entries; Krait will not list it.";
        return false;
    }
    if (!complete) {
        failFromLibssh("The directory listing stopped early.");
        return false;
    }
    std::sort(entries.begin(), entries.end(), entryLess);
    *out = std::move(entries);
    return true;
}

bool Sftp::stat(const std::string& path, SftpEntry* out) {
    if (m_sftp == nullptr) {
        m_error = "SFTP is not open.";
        return false;
    }
    m_cancelled = false;  // see realpath
    sftp_attributes attr = sftp_stat(m_sftp, path.c_str());
    if (attr == nullptr) {
        failFromLibssh("Could not read that file's details.");
        return false;
    }
    *out = SftpEntry{};
    fillEntry(attr, out);
    sftp_attributes_free(attr);
    return true;
}

bool Sftp::get(const std::string& remote, const std::string& localPath, const Progress& progress) {
    if (m_sftp == nullptr) {
        m_error = "SFTP is not open.";
        return false;
    }
    m_cancelled = false;

    sftp_file file = sftp_open(m_sftp, remote.c_str(), O_RDONLY, 0);
    if (file == nullptr) {
        failFromLibssh("Could not open the remote file.");
        return false;
    }
    // fstat on the open handle rather than a second stat() by path: the total
    // only drives a progress bar, and asking twice invites the two answers to
    // disagree.
    std::uint64_t total = 0;
    if (sftp_attributes attr = sftp_fstat(file); attr != nullptr) {
        total = attr->size;
        sftp_attributes_free(attr);
    }

    const std::filesystem::path local(localPath);
    std::ofstream out_file(local, std::ios::binary | std::ios::trunc);
    if (!out_file) {
        m_error = "Could not create the local file.";
        sftp_close(file);
        return false;
    }

    // Removes the partial download. A half-written file with the right name is
    // worse than no file at all, because whatever reads it next cannot tell.
    const auto discard = [&out_file, &local] {
        out_file.close();
        std::error_code ignored;
        std::filesystem::remove(local, ignored);
    };

    std::vector<char> buffer(m_chunk);
    std::uint64_t done = 0;
    if (progress && !progress(0, total)) {
        m_cancelled = true;
        m_error = "Cancelled.";
        sftp_close(file);
        discard();
        return false;
    }
    for (;;) {
        // 0 is END OF FILE here and negative is the error, which is the
        // opposite of ssh_channel_read's convention. Treating 0 as "nothing
        // yet" would spin forever at the end of every download.
        const ssize_t n = sftp_read(file, buffer.data(), buffer.size());
        if (n < 0) {
            failFromLibssh("The download failed.");
            sftp_close(file);
            discard();
            return false;
        }
        if (n == 0) {
            break;
        }
        out_file.write(buffer.data(), n);
        if (!out_file) {
            m_error = "Could not write to the local file — the disk may be full.";
            sftp_close(file);
            discard();
            return false;
        }
        done += static_cast<std::uint64_t>(n);
        if (progress && !progress(done, total)) {
            m_cancelled = true;
            m_error = "Cancelled.";
            sftp_close(file);
            discard();
            return false;
        }
    }
    sftp_close(file);
    out_file.close();
    if (!out_file) {
        m_error = "Could not finish writing the local file — the disk may be full.";
        std::error_code ignored;
        std::filesystem::remove(local, ignored);
        return false;
    }
    // The read loop's only terminator is the SERVER saying end-of-file, so a
    // server that promises 8 MB and stops at 1 MB otherwise produces a short
    // file, no error, and a true return — the exact "half-written file with the
    // right name" this function deletes on every other failure path. Only
    // checked when the server gave a size at all.
    if (total > 0 && done < total) {
        m_error = "The server ended the download early; the file is incomplete.";
        std::error_code ignored;
        std::filesystem::remove(local, ignored);
        return false;
    }
    return true;
}

bool Sftp::put(const std::string& localPath, const std::string& remote, const Progress& progress) {
    if (m_sftp == nullptr) {
        m_error = "SFTP is not open.";
        return false;
    }
    m_cancelled = false;

    const std::filesystem::path local(localPath);
    std::ifstream in_file(local, std::ios::binary);
    if (!in_file) {
        m_error = "Could not open the local file.";
        return false;
    }
    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(local, sizeError);
    const std::uint64_t total = sizeError ? 0 : static_cast<std::uint64_t>(size);

    // 0644, not the local file's mode: Windows has no POSIX permission bits to
    // copy, and inventing 0777 out of FILE_ATTRIBUTE_NORMAL would hand out
    // execute on a file that never had it.
    sftp_file file = sftp_open(m_sftp, remote.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file == nullptr) {
        failFromLibssh("Could not create the remote file.");
        return false;
    }

    std::vector<char> buffer(m_chunk);
    std::uint64_t done = 0;
    if (progress && !progress(0, total)) {
        m_cancelled = true;
        m_error = "Cancelled.";
        sftp_close(file);
        return false;
    }
    while (in_file) {
        in_file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in_file.gcount();
        if (got <= 0) {
            break;
        }
        // Short writes are legal: libssh caps a write at the server's stated
        // maximum, so a loop that assumed one call moved everything would
        // silently drop the tail of every chunk against a server with a small
        // limit.
        std::streamsize written = 0;
        while (written < got) {
            const ssize_t n =
                sftp_write(file, buffer.data() + written, static_cast<std::size_t>(got - written));
            if (n <= 0) {
                failFromLibssh("The upload failed.");
                sftp_close(file);
                return false;
            }
            written += n;
        }
        done += static_cast<std::uint64_t>(got);
        if (progress && !progress(done, total)) {
            m_cancelled = true;
            m_error = "Cancelled.";
            sftp_close(file);
            return false;
        }
    }
    if (in_file.bad()) {
        m_error = "Could not read the local file.";
        sftp_close(file);
        return false;
    }
    if (sftp_close(file) != SSH_OK) {
        // The server can still refuse at close — a quota is the usual reason,
        // and it is the last moment anything can tell us.
        failFromLibssh("The server could not finish writing the file.");
        return false;
    }
    return true;
}

}  // namespace krait::net

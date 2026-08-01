#include "ssh_test_server.h"

// libssh declares the SFTP SERVER entry points — sftp_server_new,
// sftp_server_init, sftp_server_free — inside `#ifdef WITH_SERVER`, and nothing
// in its installed CMake config defines that for consumers. The symbols ARE
// exported from ssh.dll in this vcpkg build (confirmed with dumpbin /EXPORTS);
// only the declarations are hidden. Defining it here is what makes them
// visible, and it has to come before the first libssh include.
#define WITH_SERVER
// Before the Windows headers below: without it the min/max MACROS eat
// std::min<...>( and the error names a token rather than the cause. Same guard
// as every other Windows-touching file in the tree.
#define NOMINMAX

#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/sftp.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace krait::test {

namespace {

// Stage tracing for the server thread, on when KRAIT_TEST_SERVER_TRACE is set.
// A hung contract test is otherwise a black box: the client log says the client
// finished, and nothing says which of the server's blocking calls it is parked
// in.
void trace(const char* stage) {
    // _dupenv_s rather than getenv: MSVC deprecates the latter and warnings are
    // errors here, same as everywhere else in the tree.
    static const bool on = [] {
        char* value = nullptr;
        std::size_t size = 0;
        const bool present =
            _dupenv_s(&value, &size, "KRAIT_TEST_SERVER_TRACE") == 0 && value != nullptr;
        std::free(value);
        return present;
    }();
    if (on) {
        std::fprintf(stderr, "[test-server] %s\n", stage);
        std::fflush(stderr);
    }
}

// Wakes a thread parked in ssh_bind_accept by connecting to the port and
// hanging up. ssh_bind_set_blocking(bind, 0) does not make accept non-blocking
// on Windows — verified: the FIRST accept returns because a client is already
// arriving, and the second parks forever, so stop() joins a thread that never
// comes back. A self-connect is the portable way out: accept returns, the
// handshake fails, and the loop re-checks the shutdown flag.
//
// WSAStartup has already been done by libssh's own init by the time any of this
// runs; if it somehow has not, socket() fails and stop() is no worse off than
// it was.
void wakeAccept(int port) {
    const SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(static_cast<u_short>(port));
    ::InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr);
    // This connect exists only to make a parked accept() return, so being
    // refused is a success as far as the caller is concerned. The result is
    // still looked at rather than discarded: a failure for some OTHER reason
    // means the wake did not happen and the test is about to hang, and that
    // deserves a line in the output instead of silence.
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::fprintf(stderr, "ssh_test_server: wake connect failed (%d)\n", ::WSAGetLastError());
    }
    ::closesocket(sock);
}

// Fixed rather than ephemeral. libssh's ssh_bind gives no way to read back the
// port the OS chose, and a contract test has to know where to connect. The
// number is high and odd enough to be free on a developer box and a runner.
constexpr int kPort = 47222;

// How long a wait runs before the loop re-checks the shutdown flag. Every wait
// in this file is bounded, same rule as the production code.
constexpr int kAcceptTimeoutMs = 200;

// --- SFTP subsystem -------------------------------------------------------
//
// Hand-rolled rather than libssh's own handler suite: the default one
// (sftp_channel_default_subsystem_request / sftp_channel_default_data_callback)
// is compiled under `#ifndef _WIN32` and is a stub on this platform, so it
// answers nothing.

// SFTP v3 has NO type field on the wire. The client derives file-vs-directory
// from the mode bits — libssh's sftp_parse_attr_3 switches on
// `permissions & SSH_S_IFMT` — so the format bits here are load-bearing, not
// decoration: send a bare 0644 and every file comes back as type UNKNOWN.
constexpr std::uint32_t kFileMode = 0100644;  // SSH_S_IFREG | rw-r--r--
constexpr std::uint32_t kDirMode = 0040755;   // SSH_S_IFDIR | rwxr-xr-x

// Ceiling on one SSH_FXP_READ reply. The length comes off the wire, so it sizes
// an allocation a client chooses — the production rule (cap every remotely
// influenced allocation) applies to a fixture too, and this one lives in the
// repo where someone will copy it.
constexpr std::uint32_t kMaxReadLength = 256 * 1024;

// Flood names per SSH_FXP_NAME reply (see Options::floodRejectedNames). One
// reply carrying all 65536+ of them would be megabytes and the client caps the
// size of a packet it will read, so the flood has to arrive spread over many
// replies — which is also how a real directory that big would arrive.
constexpr int kFloodBatch = 512;

// One handle the client is holding: a directory it is listing or a file it is
// transferring. SFTP handles are opaque 4-byte tokens to the client, so this is
// where the server keeps everything the NEXT message about that handle needs.
struct SftpHandle {
    std::filesystem::path path;
    std::fstream file;
    std::vector<std::filesystem::path> entries;
    bool isDir = false;
    // A listing terminates when the server answers a SECOND SSH_FXP_READDIR for
    // the same handle with SSH_FX_EOF. Without this flag the client re-reads the
    // same names forever.
    bool listed = false;
    // Synthetic flood names already sent on this handle, so the flood can span
    // replies instead of being one impossible packet.
    int flooded = 0;
    // SSH_FXP_READs already answered on this handle, which is what
    // Options::truncateReads counts before it starts saying end-of-file.
    int reads = 0;
};

// Attributes for an entry with no file behind it. The injected and flooded
// names are strings on the wire and nothing more, so statPath has nothing to
// stat — but SFTP v3 carries no type field, and a client derives file-vs-dir
// from the format bits, so those still have to be right.
sftp_attributes_struct syntheticAttr() {
    sftp_attributes_struct attr{};
    attr.flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS;
    attr.type = SSH_FILEXFER_TYPE_REGULAR;
    attr.permissions = kFileMode;
    return attr;
}

// Maps a client-supplied path onto a real one under `root`, or returns an empty
// path when it escapes.
//
// This is a fixture, but a fixture that can be walked out of with
// "../../../.ssh/id_ed25519" is a footgun sitting in the repo — and the thing
// sending the paths is precisely the code under test, which is the last thing
// that should be trusted to be well-behaved.
std::filesystem::path resolveUnderRoot(const std::filesystem::path& root, const char* clientPath) {
    if (clientPath == nullptr) {
        return {};
    }
    // The fixture presents `root` as "/", so a leading separator means "the
    // served root" rather than the drive root. Stripping it also stops
    // operator/ from throwing the left-hand side away for an absolute path.
    std::string_view relative(clientPath);
    while (!relative.empty() && (relative.front() == '/' || relative.front() == '\\')) {
        relative.remove_prefix(1);
    }
    if (relative == ".") {
        relative = {};
    }
    std::error_code failed;
    std::filesystem::path resolved =
        std::filesystem::weakly_canonical(root / std::filesystem::path(relative), failed);
    if (failed) {
        return {};
    }
    // Component-wise, not a string prefix: "C:/tmp/root-evil" starts with
    // "C:/tmp/root" as text and is a different directory.
    const auto [rootEnd, unused] =
        std::mismatch(root.begin(), root.end(), resolved.begin(), resolved.end());
    if (rootEnd != root.end()) {
        return {};
    }
    return resolved;
}

// What the client is TOLD a path is. Answering with the real Windows path would
// leak the developer's directory layout into the client and force every test to
// spell out a temp directory; "/" plus POSIX separators is what a client
// expects from an SFTP server anyway.
std::string virtualPath(const std::filesystem::path& root, const std::filesystem::path& resolved) {
    const std::filesystem::path relative = resolved.lexically_relative(root);
    std::string shown = "/";
    if (relative != ".") {
        shown += relative.generic_string();
    }
    return shown;
}

// Fills `attr` from a real path. False means it is not there, which is the
// caller's cue to answer SSH_FX_NO_SUCH_FILE.
bool statPath(const std::filesystem::path& path, sftp_attributes_struct* attr) {
    *attr = {};
    std::error_code failed;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, failed);
    if (failed || !std::filesystem::exists(status)) {
        return false;
    }
    const bool isDir = std::filesystem::is_directory(status);
    attr->flags =
        SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
    attr->type = isDir ? SSH_FILEXFER_TYPE_DIRECTORY : SSH_FILEXFER_TYPE_REGULAR;
    attr->permissions = isDir ? kDirMode : kFileMode;
    if (!isDir) {
        const std::uintmax_t size = std::filesystem::file_size(path, failed);
        attr->size = failed ? 0 : static_cast<std::uint64_t>(size);
    }
    if (const auto written = std::filesystem::last_write_time(path, failed); !failed) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::clock_cast<std::chrono::system_clock>(written).time_since_epoch());
        attr->mtime = static_cast<std::uint32_t>(seconds.count());
        attr->atime = attr->mtime;
    }
    return true;
}

// The `ls -l` line every SSH_FXP_NAME entry carries. Krait's client ignores it,
// but the field is not optional in the protocol and clients that DO parse it
// (psftp reads the owner out of here) choke on an empty one.
std::string longName(const std::string& name, const sftp_attributes_struct& attr) {
    char line[512];
    std::snprintf(line, sizeof(line), "%s 1 krait krait %8llu Jan  1 00:00 %s",
                  attr.type == SSH_FILEXFER_TYPE_DIRECTORY ? "drwxr-xr-x" : "-rw-r--r--",
                  static_cast<unsigned long long>(attr.size), name.c_str());
    return line;
}

// The SftpHandle behind a client's handle string, or nullptr if it is not one
// of ours. Every handle-taking message goes through here: a client is free to
// invent a handle, and dereferencing whatever comes back would be the fixture
// crashing on hostile input.
SftpHandle* handleFor(sftp_session sftp, ssh_string handle) {
    if (handle == nullptr) {
        return nullptr;
    }
    return static_cast<SftpHandle*>(sftp_handle(sftp, handle));
}

// Answers one client message against real files under `root`, misbehaving in
// whatever way `options` asks for.
void handleSftpMessage(sftp_session sftp, sftp_client_message msg,
                       const std::filesystem::path& root, const SshTestServer::Options& options,
                       std::vector<std::unique_ptr<SftpHandle>>* handles) {
    switch (sftp_client_message_get_type(msg)) {
    case SSH_FXP_REALPATH: {
        const std::filesystem::path resolved =
            resolveUnderRoot(root, sftp_client_message_get_filename(msg));
        if (resolved.empty()) {
            sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, "outside the served root");
            break;
        }
        // A path being resolved need not exist, so a failed stat is not an
        // error here — it just means the attributes go out empty.
        sftp_attributes_struct attr{};
        statPath(resolved, &attr);
        const std::string shown = virtualPath(root, resolved);
        sftp_reply_names_add(msg, shown.c_str(), longName(shown, attr).c_str(), &attr);
        sftp_reply_names(msg);
        break;
    }

    case SSH_FXP_OPENDIR: {
        const std::filesystem::path resolved =
            resolveUnderRoot(root, sftp_client_message_get_filename(msg));
        if (resolved.empty()) {
            sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, "outside the served root");
            break;
        }
        std::error_code failed;
        if (!std::filesystem::is_directory(resolved, failed)) {
            sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "not a directory");
            break;
        }
        auto handle = std::make_unique<SftpHandle>();
        handle->isDir = true;
        handle->path = resolved;
        // Snapshotted at open rather than iterated at readdir: a directory
        // iterator that outlives the message it was created for is a lifetime
        // question this fixture has no reason to answer.
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(resolved, failed)) {
            handle->entries.push_back(entry.path());
        }
        ssh_string token = sftp_handle_alloc(sftp, handle.get());
        if (token == nullptr) {
            sftp_reply_status(msg, SSH_FX_FAILURE, "out of handles");
            break;
        }
        handles->push_back(std::move(handle));
        sftp_reply_handle(msg, token);
        // sftp_reply_handle copies the bytes into the packet; the string itself
        // is ours to free.
        ssh_string_free(token);
        break;
    }

    case SSH_FXP_READDIR: {
        SftpHandle* handle = handleFor(sftp, msg->handle);
        if (handle == nullptr || !handle->isDir) {
            sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "not an open directory");
            break;
        }
        // The flood comes first and never sets `listed`, so it keeps answering
        // until its budget runs out — by which point a correct client has
        // already given up at its own iteration cap.
        if (handle->flooded < options.floodRejectedNames) {
            sftp_attributes_struct attr = syntheticAttr();
            const int end = std::min(handle->flooded + kFloodBatch, options.floodRejectedNames);
            for (; handle->flooded < end; ++handle->flooded) {
                // A separator is the cheapest thing a client must refuse, and
                // refusing it is what keeps these out of the kept-entry count.
                const std::string name = "x/" + std::to_string(handle->flooded);
                sftp_reply_names_add(msg, name.c_str(), longName(name, attr).c_str(), &attr);
            }
            sftp_reply_names(msg);
            break;
        }
        if (handle->listed) {
            if (options.readdirFailsEarly) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "the listing broke");
                break;
            }
            sftp_reply_status(msg, SSH_FX_EOF, nullptr);
            break;
        }
        int added = 0;
        // "." and ".." on purpose. A real server sends them, and Krait's
        // listDir is supposed to drop them — a fixture that omitted them would
        // leave that filter untested and passing by accident.
        for (const char* dot : {".", ".."}) {
            sftp_attributes_struct attr{};
            if (statPath(handle->path, &attr)) {
                sftp_reply_names_add(msg, dot, longName(dot, attr).c_str(), &attr);
                ++added;
            }
        }
        for (const std::filesystem::path& entry : handle->entries) {
            sftp_attributes_struct attr{};
            if (!statPath(entry, &attr)) {
                continue;
            }
            const std::string name = entry.filename().string();
            sftp_reply_names_add(msg, name.c_str(), longName(name, attr).c_str(), &attr);
            ++added;
        }
        // Verbatim, after the real entries and in the same reply: the case
        // these serve is "the hostile name was dropped AND the legitimate one
        // next to it survived", which needs both in one listing.
        for (const std::string& injected : options.injectNames) {
            sftp_attributes_struct attr = syntheticAttr();
            sftp_reply_names_add(msg, injected.c_str(), longName(injected, attr).c_str(), &attr);
            ++added;
        }
        handle->listed = true;
        if (added == 0) {
            // sftp_reply_names dereferences the accumulated buffer, which
            // sftp_reply_names_add is what allocates — replying "no names" with
            // it would be a null dereference inside libssh.
            sftp_reply_status(msg, SSH_FX_EOF, nullptr);
            break;
        }
        sftp_reply_names(msg);
        break;
    }

    case SSH_FXP_OPEN: {
        const std::filesystem::path resolved =
            resolveUnderRoot(root, sftp_client_message_get_filename(msg));
        if (resolved.empty()) {
            sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, "outside the served root");
            break;
        }
        const std::uint32_t flags = sftp_client_message_get_flags(msg);
        const bool writing = (flags & (SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC)) != 0;
        std::error_code failed;
        if (!writing && !std::filesystem::is_regular_file(resolved, failed)) {
            sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "no such file");
            break;
        }
        auto handle = std::make_unique<SftpHandle>();
        handle->path = resolved;
        // std::ios::out already truncates unless in is also set, which matches
        // the only write flags Krait's client sends (O_WRONLY|O_CREAT|O_TRUNC).
        // A fixture that also handled append would be handling a case nothing
        // produces.
        handle->file.open(resolved, std::ios::binary | (writing ? std::ios::out : std::ios::in));
        if (!handle->file) {
            sftp_reply_status(msg, SSH_FX_FAILURE, "could not open the file");
            break;
        }
        ssh_string token = sftp_handle_alloc(sftp, handle.get());
        if (token == nullptr) {
            sftp_reply_status(msg, SSH_FX_FAILURE, "out of handles");
            break;
        }
        handles->push_back(std::move(handle));
        sftp_reply_handle(msg, token);
        ssh_string_free(token);
        break;
    }

    case SSH_FXP_READ: {
        SftpHandle* handle = handleFor(sftp, msg->handle);
        if (handle == nullptr || handle->isDir) {
            sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "not an open file");
            break;
        }
        // One chunk, then end-of-file for the rest of the transfer — after
        // FSTAT has already promised the full size. The client cannot tell this
        // from a file that really ended, which is the whole problem: its only
        // defence is comparing what arrived against what it was promised.
        if (options.truncateReads && handle->reads++ > 0) {
            sftp_reply_status(msg, SSH_FX_EOF, nullptr);
            break;
        }
        std::vector<char> buffer(std::min(msg->len, kMaxReadLength));
        handle->file.seekg(static_cast<std::streamoff>(msg->offset));
        handle->file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = handle->file.gcount();
        // A short read sets eofbit, and a stream in that state ignores the next
        // seekg — so every later read would return nothing and the transfer
        // would stall one chunk before the end.
        handle->file.clear();
        if (got <= 0) {
            sftp_reply_status(msg, SSH_FX_EOF, nullptr);
            break;
        }
        sftp_reply_data(msg, buffer.data(), static_cast<int>(got));
        break;
    }

    case SSH_FXP_WRITE: {
        SftpHandle* handle = handleFor(sftp, msg->handle);
        if (handle == nullptr || handle->isDir || msg->data == nullptr) {
            sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "not an open file");
            break;
        }
        // ssh_string_data/len, not sftp_client_message_get_data: that one hands
        // back a NUL-TERMINATED copy, which silently truncates the first time a
        // file contains a zero byte — exactly what the round-trip test uploads.
        handle->file.seekp(static_cast<std::streamoff>(msg->offset));
        handle->file.write(static_cast<const char*>(ssh_string_data(msg->data)),
                           static_cast<std::streamsize>(ssh_string_len(msg->data)));
        if (!handle->file) {
            sftp_reply_status(msg, SSH_FX_FAILURE, "could not write the file");
            break;
        }
        sftp_reply_status(msg, SSH_FX_OK, nullptr);
        break;
    }

    case SSH_FXP_CLOSE: {
        SftpHandle* handle = handleFor(sftp, msg->handle);
        if (handle == nullptr) {
            sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "not an open handle");
            break;
        }
        // sftp_handle_remove takes the INFO pointer that sftp_handle_alloc
        // registered, NOT the handle string — libssh compares slots by pointer
        // identity. Passing msg->handle here matches nothing, frees no slot,
        // and runs the session out of handles after SFTP_HANDLES transfers.
        sftp_handle_remove(sftp, handle);
        std::erase_if(*handles, [handle](const std::unique_ptr<SftpHandle>& held) {
            return held.get() == handle;
        });
        sftp_reply_status(msg, SSH_FX_OK, nullptr);
        break;
    }

    case SSH_FXP_STAT:
    case SSH_FXP_LSTAT: {
        const std::filesystem::path resolved =
            resolveUnderRoot(root, sftp_client_message_get_filename(msg));
        if (resolved.empty()) {
            sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, "outside the served root");
            break;
        }
        sftp_attributes_struct attr{};
        if (!statPath(resolved, &attr)) {
            sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "no such file");
            break;
        }
        sftp_reply_attr(msg, &attr);
        break;
    }

    case SSH_FXP_FSTAT: {
        // Krait's Sftp::get fstats the OPEN handle to size its progress bar
        // rather than stat-ing the path a second time. Refusing this would
        // leave every download reporting a total of 0.
        SftpHandle* handle = handleFor(sftp, msg->handle);
        sftp_attributes_struct attr{};
        if (handle == nullptr || !statPath(handle->path, &attr)) {
            sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "not an open file");
            break;
        }
        sftp_reply_attr(msg, &attr);
        break;
    }

    default:
        sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED, "this fixture does not do that");
        break;
    }
}

// Pumps client messages until the client goes away or the server is stopped.
void serveSftpSession(sftp_session sftp, ssh_channel channel, const std::filesystem::path& root,
                      const SshTestServer::Options& options, const std::atomic<bool>& shutdown) {
    std::vector<std::unique_ptr<SftpHandle>> handles;
    while (!shutdown.load()) {
        // Poll before reading. sftp_get_client_message goes through
        // sftp_packet_read, which treats a read that TIMES OUT as fatal and
        // returns NULL — so calling it on an idle channel would drop the
        // connection after the session's two-second timeout. Polling keeps the
        // wait bounded, re-checks the shutdown flag between polls so stop() can
        // still join, and only lets the blocking read run when a packet is
        // already sitting there.
        const int ready = ssh_channel_poll_timeout(channel, kAcceptTimeoutMs, 0);
        if (ready == SSH_ERROR || ready == SSH_EOF) {
            break;
        }
        if (ready == 0) {
            continue;
        }
        sftp_client_message msg = sftp_get_client_message(sftp);
        if (msg == nullptr) {
            break;
        }
        handleSftpMessage(sftp, msg, root, options, &handles);
        sftp_client_message_free(msg);
    }
}

// Negotiates the "sftp" subsystem on an already-open session channel and serves
// it until the client leaves. The channel and session stay the caller's to tear
// down, same as the shell path.
void serveSftp(ssh_session session, ssh_channel channel, const SshTestServer::Options& options,
               const std::atomic<bool>& shutdown) {
    std::error_code failed;
    // Canonical once, here: every later path check compares against it, and
    // comparing a canonical path with a non-canonical root is how a directory
    // traversal check ends up passing everything.
    const std::filesystem::path root = std::filesystem::weakly_canonical(options.sftpRoot, failed);
    if (failed) {
        return;
    }

    trace("subsystem-loop");
    bool subsystem = false;
    while (!subsystem && !shutdown.load()) {
        ssh_message message = ssh_message_get(session);
        if (message == nullptr) {
            return;
        }
        const char* name = ssh_message_type(message) == SSH_REQUEST_CHANNEL &&
                                   ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_SUBSYSTEM
                               ? ssh_message_channel_request_subsystem(message)
                               : nullptr;
        if (name != nullptr && std::string_view(name) == "sftp") {
            subsystem = true;
            ssh_message_channel_request_reply_success(message);
        } else {
            ssh_message_reply_default(message);
        }
        ssh_message_free(message);
    }
    if (!subsystem) {
        return;
    }

    trace("sftp-init");
    sftp_session sftp = sftp_server_new(session, channel);
    if (sftp == nullptr) {
        return;
    }
    // sftp_server_init is marked SSH_DEPRECATED, but that macro expands to
    // nothing outside GCC/Clang (libssh.h guards it on __GNUC__), so MSVC emits
    // no C4996 and there is nothing to suppress here. The replacement is the
    // callbacks-based server, which is the one that is a Windows stub — see the
    // note at the top of this section.
    if (sftp_server_init(sftp) != 0) {
        sftp_server_free(sftp);
        return;
    }

    trace("sftp-serve");
    serveSftpSession(sftp, channel, root, options, shutdown);
    trace("sftp-done");
    // sftp_server_free frees the handle table and the read buffer; the channel
    // it was handed is deliberately left alone for the caller.
    sftp_server_free(sftp);
}

}  // namespace

struct SshTestServer::Impl {
    ssh_bind bind = nullptr;
    ssh_session session = nullptr;
    ssh_channel channel = nullptr;
    SshTestServer::Options options;
    std::string hostKeyPath;
    mutable std::mutex mutex;
    std::string received;
};

SshTestServer::SshTestServer() : m_impl(std::make_unique<Impl>()) {}

SshTestServer::~SshTestServer() {
    // stop() joins a thread and touches libssh; nothing in it is declared
    // noexcept, and an exception escaping a destructor terminates the whole
    // run — turning a teardown hiccup into a suite with no results.
    try {
        stop();
    } catch (...) {  // NOLINT(bugprone-empty-catch): see above
    }
}

bool SshTestServer::start(Options options) {
    m_impl->options = std::move(options);
    m_shutdown = false;
    m_error.clear();

    // A throwaway host key per run, in the temp dir. `rotateHostKey` gives it a
    // different name so the key genuinely differs from the previous run's — the
    // whole point of the changed-key test is that the bytes are not the same.
    const std::string suffix = m_impl->options.rotateHostKey ? "-rotated" : "";
    m_impl->hostKeyPath =
        (std::filesystem::temp_directory_path() / ("krait-test-hostkey" + suffix)).string();
    std::filesystem::remove(m_impl->hostKeyPath);

    ssh_key hostKey = nullptr;
    if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &hostKey) != SSH_OK) {
        m_error = "could not generate a host key";
        return false;
    }
    const int exported = ssh_pki_export_privkey_file(hostKey, nullptr, nullptr, nullptr,
                                                     m_impl->hostKeyPath.c_str());
    ssh_key_free(hostKey);
    if (exported != SSH_OK) {
        m_error = "could not write the host key";
        return false;
    }

    m_impl->bind = ssh_bind_new();
    if (m_impl->bind == nullptr) {
        m_error = "ssh_bind_new failed";
        return false;
    }
    const char* address = "127.0.0.1";
    int port = kPort;
    ssh_bind_options_set(m_impl->bind, SSH_BIND_OPTIONS_BINDADDR, address);
    ssh_bind_options_set(m_impl->bind, SSH_BIND_OPTIONS_BINDPORT, &port);
    ssh_bind_options_set(m_impl->bind, SSH_BIND_OPTIONS_HOSTKEY, m_impl->hostKeyPath.c_str());

    if (ssh_bind_listen(m_impl->bind) != SSH_OK) {
        m_error = ssh_get_error(m_impl->bind);
        ssh_bind_free(m_impl->bind);
        m_impl->bind = nullptr;
        return false;
    }
    m_port = kPort;
    m_thread = std::thread([this] { serve(); });
    return true;
}

void SshTestServer::serve() {
    while (!m_shutdown.load()) {
        ssh_session session = ssh_new();
        if (session == nullptr) {
            return;
        }
        // Blocking. stop() wakes it with a self-connect; see wakeAccept.
        if (ssh_bind_accept(m_impl->bind, session) != SSH_OK) {
            ssh_free(session);
            if (m_shutdown.load()) {
                return;
            }
            continue;
        }
        if (m_shutdown.load()) {
            // The wake-up connection, not a client.
            ssh_free(session);
            return;
        }
        // The accepted session inherits the bind's non-blocking mode, and every
        // call below is written as blocking. Put it back before the key
        // exchange, or ssh_handle_key_exchange returns SSH_AGAIN and the
        // connection is dropped as a failure.
        trace("accepted");
        ssh_set_blocking(session, 1);
        // ...but blocking with a BOUND. Without this, the teardown below
        // (ssh_channel_close writes a close packet) blocks forever once the
        // client has already gone, and stop() then joins a thread that will
        // never return. Same rule as the production code: every wait has a
        // timeout.
        long sessionTimeout = 2;
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &sessionTimeout);
        m_impl->session = session;

        trace("kex-start");
        if (ssh_handle_key_exchange(session) != SSH_OK) {
            ssh_disconnect(session);
            ssh_free(session);
            m_impl->session = nullptr;
            continue;
        }

        // Auth. Only "password" is offered, so the client's ladder has exactly
        // one rung it can use and the test knows which path it took.
        trace("auth-loop");
        bool authenticated = false;
        while (!authenticated && !m_shutdown.load()) {
            ssh_message message = ssh_message_get(session);
            if (message == nullptr) {
                break;
            }
            if (ssh_message_type(message) == SSH_REQUEST_AUTH &&
                ssh_message_subtype(message) == SSH_AUTH_METHOD_PASSWORD) {
                const char* user = ssh_message_auth_user(message);
                const char* password = ssh_message_auth_password(message);
                const bool ok = !m_impl->options.refuseAuth && user != nullptr &&
                                password != nullptr && m_impl->options.user == user &&
                                m_impl->options.password == password;
                if (ok) {
                    authenticated = true;
                    ssh_message_auth_reply_success(message, 0);
                } else {
                    ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                }
            } else {
                ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_PASSWORD);
                ssh_message_reply_default(message);
            }
            ssh_message_free(message);
        }
        if (!authenticated) {
            ssh_disconnect(session);
            ssh_free(session);
            m_impl->session = nullptr;
            continue;
        }

        // Channel, then pty, then shell — the same three the client asks for.
        trace("channel-loop");
        ssh_channel channel = nullptr;
        while (channel == nullptr && !m_shutdown.load()) {
            ssh_message message = ssh_message_get(session);
            if (message == nullptr) {
                break;
            }
            if (ssh_message_type(message) == SSH_REQUEST_CHANNEL_OPEN &&
                ssh_message_subtype(message) == SSH_CHANNEL_SESSION) {
                channel = ssh_message_channel_request_open_reply_accept(message);
            } else {
                ssh_message_reply_default(message);
            }
            ssh_message_free(message);
        }
        if (channel == nullptr) {
            ssh_disconnect(session);
            ssh_free(session);
            m_impl->session = nullptr;
            continue;
        }
        m_impl->channel = channel;

        // SFTP instead of a shell. The shell path below is left exactly as it
        // was — the existing contract tests are written against it.
        if (!m_impl->options.sftpRoot.empty()) {
            serveSftp(session, channel, m_impl->options, m_shutdown);
            trace("close-channel");
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            m_impl->channel = nullptr;
            ssh_disconnect(session);
            ssh_free(session);
            m_impl->session = nullptr;
            continue;
        }

        trace("shell-loop");
        bool shell = false;
        while (!shell && !m_shutdown.load()) {
            ssh_message message = ssh_message_get(session);
            if (message == nullptr) {
                break;
            }
            if (ssh_message_type(message) == SSH_REQUEST_CHANNEL &&
                (ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_PTY ||
                 ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_SHELL)) {
                shell = ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_SHELL;
                ssh_message_channel_request_reply_success(message);
            } else {
                ssh_message_reply_default(message);
            }
            ssh_message_free(message);
        }

        if (m_impl->options.dropAfterShell) {
            // No EOF, no exit status, no disconnect message: the socket just
            // goes. That is what a peer vanishing looks like, and it has to
            // reach the user as something other than "the shell exited".
            ssh_silent_disconnect(session);
            ssh_free(session);
            m_impl->session = nullptr;
            m_impl->channel = nullptr;
            continue;
        }

        // A banner, so the client has something to render, then echo whatever
        // arrives. The echo is what proves the write path reached the wire.
        trace("echo-start");
        const std::string greeting = "krait-test-server ready\r\n";
        ssh_channel_write(channel, greeting.data(), static_cast<std::uint32_t>(greeting.size()));

        char buffer[1024];
        while (!m_shutdown.load() && ssh_channel_is_open(channel) != 0 &&
               ssh_channel_is_eof(channel) == 0) {
            const int n =
                ssh_channel_read_timeout(channel, buffer, sizeof(buffer), 0, kAcceptTimeoutMs);
            if (n == SSH_ERROR) {
                break;
            }
            if (n > 0) {
                {
                    const std::lock_guard lock(m_impl->mutex);
                    m_impl->received.append(buffer, static_cast<std::size_t>(n));
                }
                ssh_channel_write(channel, buffer, static_cast<std::uint32_t>(n));
            }
        }

        trace("close-channel");
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        m_impl->channel = nullptr;
        trace("disconnect");
        ssh_disconnect(session);
        trace("disconnected");
        ssh_free(session);
        trace("session-freed");
        m_impl->session = nullptr;
    }
    trace("serve-exit");
}

void SshTestServer::stop() {
    trace("stop-enter");
    m_shutdown = true;
    if (m_thread.joinable()) {
        wakeAccept(m_port);
        m_thread.join();
    }
    trace("stop-joined");
    if (m_impl->bind != nullptr) {
        ssh_bind_free(m_impl->bind);
        m_impl->bind = nullptr;
    }
    if (!m_impl->hostKeyPath.empty()) {
        std::filesystem::remove(m_impl->hostKeyPath);
        m_impl->hostKeyPath.clear();
    }
}

std::string SshTestServer::received() const {
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->received;
}

}  // namespace krait::test

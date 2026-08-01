#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace krait::test {

// A real SSH server, in this process, on a loopback port.
//
// The milestone asks for contract tests against a dockerized sshd. This is
// better for what the tests actually need: no Docker on a Windows CI runner, no
// container start-up in the inner loop, and — the part a production sshd cannot
// give you — the ability to make the server misbehave ON PURPOSE. Half of these
// tests are about what Krait does when the server changes its host key, vanishes
// mid-session, or refuses every credential, and asking a real sshd to do that on
// cue is harder than writing this.
//
// One connection at a time, which is all a contract test needs.
class SshTestServer {
  public:
    struct Options {
        // The password that works. Anything else is refused, which is how the
        // auth-failed path gets exercised without inventing a real account.
        std::string password = "correct-horse";
        std::string user = "tester";
        // Regenerate the host key before accepting. This is how the changed-key
        // case is produced: connect once to establish trust, restart with this
        // set, and reconnect.
        bool rotateHostKey = false;
        // Close the transport the moment the shell opens, without a clean exit.
        // A peer that vanishes is not a peer that said goodbye, and the two have
        // to reach the user differently.
        bool dropAfterShell = false;
        // Refuse every password, to drive the auth-failed banner.
        bool refuseAuth = false;
        // When set, the session serves the SFTP subsystem rooted at this
        // directory instead of a shell. Everything the client asks for is
        // resolved under it and anything that escapes is refused — see
        // resolveUnderRoot. Empty (the default) keeps the shell behaviour the
        // contract tests are written against.
        std::string sftpRoot;

        // --- SFTP misbehaviour ------------------------------------------
        //
        // The knobs below exist for the same reason this class does: a real
        // sshd will not answer with a filename it knows is illegal, stop a
        // download halfway, or fail a listing on cue, and those are precisely
        // the answers Sftp has to survive (rules/net.md: all remote input is
        // hostile). They only apply when sftpRoot is set.

        // Extra SSH_FXP_READDIR entries, sent verbatim and backed by no file
        // on disk. This is how a name the client must REFUSE gets onto the
        // wire — "../escape", a name holding a control byte, a name with a
        // separator in it. The panel composes local paths out of these, so a
        // name the server chose is a destination the server chose.
        std::vector<std::string> injectNames;

        // Emit this many synthetic entries that Sftp::listDir is guaranteed to
        // reject, spread over as many READDIR replies as it takes. Set above
        // Sftp::kMaxEntries this proves the iteration cap terminates: the cap
        // counts ITERATIONS, and a version that counted kept entries instead
        // would sit at zero here and never leave the loop. Generated on the
        // fly — nothing this large goes near the disk.
        int floodRejectedNames = 0;

        // Answer the second and every later SSH_FXP_READ with SSH_FX_EOF,
        // whatever size SSH_FXP_FSTAT already promised. A server that stops
        // short has to reach the user as a failed download, not as a complete
        // file that is quietly missing its tail.
        bool truncateReads = false;

        // End the listing with an error status instead of SSH_FX_EOF. Both
        // reach the client as a NULL from sftp_readdir, and sftp_dir_eof is
        // the only thing that tells "finished" from "stopped" — a listing that
        // stopped must not come back looking like a smaller directory.
        bool readdirFailsEarly = false;
    };

    SshTestServer();
    ~SshTestServer();
    SshTestServer(const SshTestServer&) = delete;
    SshTestServer& operator=(const SshTestServer&) = delete;

    // Binds 127.0.0.1 on a fixed loopback port and starts accepting. False on
    // failure, with error() saying why.
    bool start(Options options);
    void stop();

    int port() const { return m_port; }

    const std::string& error() const { return m_error; }

    // Everything the shell channel received. The contract test writes bytes and
    // checks they arrived, which is the only way to prove the write path from
    // the GUI thread actually reaches the wire.
    std::string received() const;

  private:
    struct Impl;
    void serve();

    std::unique_ptr<Impl> m_impl;
    std::thread m_thread;
    std::atomic<bool> m_shutdown{false};
    int m_port = 0;
    std::string m_error;
};

}  // namespace krait::test

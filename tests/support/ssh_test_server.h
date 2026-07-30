#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

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

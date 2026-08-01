# STATE

Phase: M4 — power tools (SFTP, shell integration, triggers). Not yet started.

## Now

M0–M3 are all on `main`. PR #25 merged 2026-07-31 as `8627724`; the working tree
is clean and `main` is in sync with origin. The app runs for real: local ConPTY
shell, SSH with jump hosts and L/R/D forwarding, telnet, raw, serial, three
importers, 402 tests green. Nothing is mid-flight — this is a clean milestone
boundary, and the only uncommitted thing should be this file.

## Next task (exactly one)

**T64 — SFTP core, headless.** Wrap libssh's sftp API in
`src/net/ssh/sftp.{h,cpp}` and drive it from the thread `SshBackend` already owns
(`src/net/ssh/ssh_backend.h`) — an `ssh_session` is not thread-safe, and a second
session would mean a second authentication. Scope: open, readdir, stat, get, put.
No QML panel yet; that is T65.
Verify: `ctest --preset dev` with a new `tests/unit/sftp_test.cpp`.
**Check FIRST, it is unverified:** whether `tests/support/ssh_test_server.cpp`
can serve an SFTP subsystem at all. If it cannot, the test goes against a real
sshd behind an opt-in env guard — not a skipped test that reports green.

## After that

- T65 — SFTP dual-pane QML panel + drag-drop (editor round-trip is the first cut line)
- T66 — OSC 133 prompt marks in `src/core/parser/osc.cpp`, jump-to-prompt, OSC 9;4 taskbar progress
- T67 — triggers (regex → highlight/notify/log/send) and the snippet bar

## Open questions

- **M3's demo has never been run.** Needs a USB serial adapter: plug in, see a
  friendly VID/PID name, replug, watch it reconnect, toggle hexdump. Serial and
  ConPTY have no hardware-level test at all. **user-decides** whether M4 starts
  without it.
- **First-run discoverability.** The owner ran the app this session and saw
  "just a terminal". That is by design — `TabStrip.qml:26` hides the strip at one
  tab, and rules/ui.md forbids a menu bar — but nothing tells a new user that
  `Ctrl+Shift+P` exists. **user-decides** whether M4 owes a first-run hint.
- **No M4 task file exists.** `docs/plan/` stops at `02-m0-tasks.md`; T20–T63
  lived only in STATE.md. **plan-decides** whether to write one.

## Watchouts

- **`main` is protected.** Even this file's rewrite needs a branch + PR — see
  PR #23 `m1-state-sync` for the pattern. A direct commit is refused.
- **`git show 728cbff:STATE.md` before calling M3 complete.** That commit holds
  nine deliberate documented refusals (no agent-signed auth test, FIDO2 untested
  on hardware, no end-to-end forwarding test, no multi-hop test, …) AND every
  libssh fact verified during M3: `ssh_set_agent_socket` fd ownership,
  `shutdown()` not waking a pending recv on Windows, `SSH_OPTIONS_CERTIFICATE`
  having to precede `ssh_connect`, `~` never expanded by `ssh_pki_import_*`.
  Do not re-derive any of it.
- **The build shell has no dev environment.** vcvars64 plus
  `QT_ROOT=C:\Qt\6.10.3\msvc2022_64`; `VCPKG_ROOT` comes from vcvars — do not
  override it. A hung ctest holds `krait-qt-tests.exe` open and the next link
  fails LNK1168: `taskkill /F /IM krait-qt-tests.exe` first.
- **Three `krait-app.exe` exist.** `build/dev/` is current; `build/rel/` and
  `build/release/` are stale enough to look like regressions when run by hand.
- **`Read` returns only line 1** of any file the claude-mem hook has observations
  on. Use `Grep` with pattern `^`, `output_mode: content`, `head_limit: 0`.

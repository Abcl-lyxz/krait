# STATE

Phase: **M3 code-complete — T52-T63 done**, on branch `t52-backend-factory`.
**M2 IS MERGED** (PR #24, commit 2794cc4 on main). The M3 branch is pushed; no
PR opened yet.

**Next: open the M3 PR and let CI run it.** Then M4. Read "What is NOT done"
before telling anyone M3 is finished — several milestone bullets shipped as a
deliberate, documented refusal rather than as code.

## What landed

| Task | What |
|---|---|
| T52 | Backend factory: a saved session finally opens a connection |
| T52b | The translation gate reads the SOURCES, not just the two .ts files |
| T53 | Tabs and splits, with a scripted UI self-test |
| T54 | Telnet: RFC 1143 negotiation, contract tests, fuzz target |
| T55 | Raw sockets, and TcpBackend extracted from telnet's socket half |
| T56 | Serial: VID/PID identity, replug reconnect, DTR/RTS/break |
| T57 | Hexdump view and timestamped session logging |
| T58 | Jump hosts via native ProxyJump, our host-key UX on every hop |
| T59 | Port forwarding L/R/D, SOCKS5, live tunnel pane |
| T60 | The agent bridge — libssh cannot reach the Windows agent, and did not |
| T61 | User certificates; FIDO2 answered through the agent (ADR-0014) |
| T62 | ssh_config and mRemoteNG importers, and all three wired to the palette |

402 tests pass; clang-tidy clean on every file touched; the app self-test
exercises tabs, splits, divider drag and close.

## T60 was fixing a lie, not adding a feature

`tryAgent()` carried a comment saying it reached the Windows agent "through
libssh's own transport". It did not, and the shape of the failure is worth
keeping:

- libssh's agent client has ONE transport — read `SSH_AUTH_SOCK` (or
  `SSH_OPTIONS_IDENTITY_AGENT`), then `ssh_socket_unix()`, which is
  `socket(AF_UNIX)` + `connect`. There is no named-pipe path in `src/agent.c` on
  any platform. The agent that ships with Windows listens on
  `\\.\pipe\openssh-ssh-agent` and nothing else.
- So it reached nothing, and reported that as **`SSH_AUTH_DENIED` — the same
  code libssh returns when the server refused every key**. The ladder fell
  through to a password prompt and nothing looked broken. That is why it
  survived two milestones.

Do not "simplify" the bridge away on the grounds that libssh has agent support.
It has agent support for a transport Windows does not use.

## Verified facts — do NOT re-derive

Everything below was read out of libssh 0.12's own source in
`build/fuzz-msvc/vcpkg_installed/vcpkg/blds/libssh/src/libssh-0-*.clean/`, or
measured here. Documentation was wrong or silent on several of them.

- **`ssh_set_agent_socket(session, fd)` is the whole seam.** It calls
  `ssh_socket_set_fd(session->agent->sock, fd)`; `session->agent` is allocated
  unconditionally in `ssh_new()`; `ssh_agent_is_running()` returns true iff that
  socket's fd is not INVALID_SOCKET. So handing libssh a connected socket is
  enough to make it speak the agent protocol down it. **libssh then OWNS that
  fd** — `ssh_free` closes it.
- **`shutdown()` does NOT reliably wake a recv already pending** on Windows.
  Documented behaviour covers recv calls made AFTER the shutdown; one already
  inside the kernel usually returns and sometimes does not. It cost an hour: the
  round-trip tests passed standalone and hung under ctest, with the relay
  provably finished. The relay socket now carries `SO_RCVTIMEO` (200 ms) purely
  so the stop flag gets looked at. Do not remove it in favour of shutdown alone.
- **`CancelIoEx` cancels only I/O that is ALREADY OUTSTANDING.** A relay caught
  between "wrote the request" and "about to issue the read" would issue an
  uncancellable read a moment later. That is why the pipe is opened
  `FILE_FLAG_OVERLAPPED` and every operation waits on `{io event, stop event}`,
  with an atomic checked before each one. The flag alone does not close it.
- **`lpNumberOfBytesTransferred` is documented as possibly erroneous when
  `lpOverlapped` is non-null.** `GetOverlappedResult` is the only source used.
- **`ssh_connect()` calls `ssh_options_apply()` itself** (client.c). That
  matters because `SSH_OPTIONS_CERTIFICATE` only appends to the UNEXPANDED list
  (`opts.certificate_non_exp`) while auth reads `opts.certificate`, and
  `ssh_options_apply` is the only thing that moves one to the other — and it is
  **not in the installed headers**, so it cannot be called directly. Set the
  option before `ssh_connect` or it is accepted, returns SSH_OK, and is never
  read.
- **`ssh_connect()` also calls `ssh_options_parse_config(session, NULL)`** when
  the config has not been processed. Krait therefore already honours
  `~/.ssh/config` on every connection — which is why the T62 importer is about
  making hosts VISIBLE, not about making them work.
- **The vcpkg libssh is built `WITH_FIDO2=OFF`.** Both `WITH_FIDO2` and
  `HAVE_LIBFIDO2` are undefined in this tree's generated `config.h`, and the
  port's configure log prints `With FIDO2/U2F support: OFF`. The sk API is
  declared in the header regardless, which is what makes this expensive to get
  wrong. ADR-0014.
- **libssh does NOT expand `~` for a path handed to `ssh_pki_import_privkey_file`
  or `ssh_pki_import_cert_file`.** It expands only while applying its OPTIONS —
  `ssh_options_apply` runs `ssh_path_expand_escape` over the identity and
  certificate lists — so a key this backend opens itself never gets it. That
  matters because `~/.ssh/id_ed25519` is the canonical ssh_config spelling, so
  every key T62 imports arrives written that way. `expandHome` in
  `backend_factory.cpp` is the one funnel that fixes it, for imported and
  hand-typed profiles alike.
- **`ssh_pki_import_privkey_file` returns `SSH_EOF` (-2), not `SSH_ERROR` (-1),
  for a file it cannot open at all.** The passphrase branch keys off
  `SSH_ERROR`, so an unreadable path used to fall out of the ladder in silence
  and degrade to a password prompt. It now sets `m_authHint`.
- **libssh does not verify host certificates.** `knownhosts.c` skips
  `@cert-authority` and `@revoked` lines at parse time and says so in a comment;
  `ssh_session_is_known_server` is not CA-aware.
- **ssh_config(5), all three counter-intuitive**: keyword and value may be
  separated by whitespace OR exactly one `=`; the FIRST value of a repeated
  keyword wins, not the last; `LocalForward`/`RemoteForward` take TWO
  whitespace-separated arguments, which OpenSSH joins with a colon before
  parsing. A parser that guesses gets all three backwards.
- **mRemoteNG's confCons.xml**: `Node` is in the EMPTY namespace while the root
  is in `mrng:`, so an XPath of `//mrng:Node` finds nothing; containers carry
  `Hostname`, `Protocol` and `Port` too, so `Type` is the only way to tell them
  apart; a missing `Type` means Connection.
- **A self-closing `<Node/>` emits an EndElement** from QXmlStreamReader. The
  folder stack must be popped by what was PUSHED, not by seeing a Node end — the
  first version flattened every sibling after a leaf, and a test caught it.

### Still true, from earlier milestones

- **`SetSearchPathMode` does NOT harden `CreateProcessW`.** Resolve the path
  yourself and pass it as `lpApplicationName`, which `resolveShellCommand` does.
- **Qt reports `RemoteHostClosedError` for a graceful FIN and a reset alike.**
  Measured both ways. A clean telnet logout and a dropped connection are
  genuinely indistinguishable at that layer.
- **`ssh_jump_callbacks_struct` has no `size` member** — no `ssh_callbacks_init`.
  libssh keeps the pointer, so the structs must outlive `ssh_connect`.
- **`QTimer::stop()` from another thread is refused silently.**
- **Qt caps socket READS and has no write-buffer cap at all.** Telnet's
  subnegotiation answers amplify 6 bytes to 20; capped at 1 MB pending.
- **A QML `Timer` never fires in a window that is never composited.** The UI
  self-test is driven from `main()` by `QMetaObject::invokeMethod`.
- **A `Repeater` delegate must be an Item**, so `Shortcut` cannot be one.
- **RFC 1928 corrections** are in `socks5.cpp` and its tests; the MUST-close on
  "no acceptable methods" is on the CLIENT, and offering only "no
  authentication" is a deliberate deviation from section 3.
- **`GUID_DEVINTERFACE_COMPORT` is in ntddser.h**, needs `DIGCF_DEVICEINTERFACE`;
  `SetupDiOpenDevRegKey` fails with `INVALID_HANDLE_VALUE`, not null.
- **`mlkem768x25519-sha256` does not work in this build** — a POSTPONEMENT,
  re-test on the next libssh bump.
- **`tr(runtimeString)` is invisible to lupdate** — the action registry repeats
  its labels as `QT_TR_NOOP` literals and a test compares the two lists.

## What is NOT done

Some of these are refusals, and are the right answer. They are listed so nobody
mistakes them for oversights.

- **No test drives libssh through an agent-SIGNED authentication.** The bridge
  is tested end to end against a real named pipe, and libssh is tested to accept
  the socket, but nothing joins the two: a fake agent that could really sign
  needs `ssh_pki_export_pubkey_blob` and friends, which live in `pki.h` — a
  header libssh does not install.
- **FIDO2 is untested against hardware.** No authenticator was available. An
  `sk-*` key named directly by a profile is deliberately REFUSED with a message
  pointing at `ssh-add`, because this libssh cannot sign with one (ADR-0014).
- **User certificates have no server-side test.** The plumbing is tested; making
  the in-process libssh server accept a CA-signed user certificate is real work
  and was not done.
- **The pipe server's identity is not checked.** If the agent service is not
  running, a local process can create that pipe name first and answer as the
  agent. `SECURITY_IDENTIFICATION` stops it impersonating us, and a squatter can
  only OFFER keys the server still has to have authorised — so it is a nuisance,
  not a credential leak. OpenSSH's own Windows client carries the same exposure.
  Closing it means a `GetNamedPipeServerProcessId` owner-SID check, which would
  break every setup where the agent runs as something we did not guess.
- **`Include` in an ssh_config is reported, not followed**, and ssh_config's
  first-obtained-value rule ACROSS blocks is not modelled — a `Host *` placed
  BEFORE the specific blocks would win in real ssh and does not here. Files are
  written the other way round; the manual page says to write them that way.
- **The importers read fixed locations**, with no file picker: `~/.ssh/config`,
  `%APPDATA%\mRemoteNG\confCons.xml`, and PuTTY's registry key.
- **No forwarding runs end to end in a test.** The SOCKS5 and spec parsers are
  covered; the socket-and-channel plumbing is not. Tunnel latency is bounded by
  the 20 ms shell poll, marked in `forward_manager.h` with its upgrade path.
- **No multi-hop contract test.** ADR-0002 asks for a two-hop chain including
  failure mid-chain; the in-process test server takes one connection.
- **`ConptyBackend::resolveShellCommand` and the serial backend have no
  hardware-level test**, and **the M3 demo has not been run** — plug a USB
  serial adapter, watch it appear with a friendly name, replug it, toggle the
  hexdump. Needs hardware this session did not have.
- **Splits are a flat list with one orientation per tab**, not a tree. Marked in
  `SessionPane.qml`. **Serial replug detection polls** once a second rather than
  using `CM_Register_Notification`. Both marked with their upgrade paths.

## Two gates that were lying, now fixed (do not re-introduce)

- **The fast gate had NEVER completed on a milestone-sized branch.** clang-tidy
  ran serially over every changed file and blew the 30-minute job timeout;
  GitHub reports a timeout kill as "cancelled", which STATE.md previously
  recorded as the concurrency group doing it. Fanned out four ways.
- **The [i18n] tests compared the two .ts files with each other and nothing
  else.** A `tr()` string missing from both passed green. lupdate now runs in CI
  with `-locations none`, and the build fails if it changes anything.

## Build environment

The shell has no dev environment. Every build needs vcvars64 plus `QT_ROOT`
(`C:\Qt\6.10.3\msvc2022_64`); `VCPKG_ROOT` comes from vcvars itself and is the
VS-bundled vcpkg, which is what built this tree — do not override it with
`C:\vcpkg`, which does not exist here. Invoke a self-contained `.cmd` via
`MSYS_NO_PATHCONV=1 cmd.exe /c 'C:\abs\path.cmd'` with single quotes.

**Do not write `\r\n` inside a python or bash heredoc** — it collapses to a raw
newline, MSVC then reports "newline in string literal", and it has cost two
repairs.

**A hung ctest holds `krait-qt-tests.exe` open** and the next link fails with
LNK1168. `taskkill /F /IM krait-qt-tests.exe` before rebuilding.

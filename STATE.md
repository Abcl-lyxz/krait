# STATE

Phase: **M3 in progress — T52-T58 done**, on branch `t52-backend-factory`.
**M2 IS MERGED** (PR #24, commit 2794cc4 on main) — the first time that gate
has ever gone green. Branch is pushed; no PR opened for M3 yet.

**Next task: T59, port forwarding (L/R/D) with a live pane.** Then T60 (agent
named-pipe bridge), T61 (FIDO2 + certs), T62 (ssh_config/mRemoteNG importers),
T63 (milestone wrap). Read "What is NOT done" before claiming M3 is finished.

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

336 tests pass; clang-tidy clean on every file touched; fuzz smoke green on
both targets (parser 23k runs, telnet 76k runs); the app self-test exercises
tabs, splits, divider drag and close.

## Two gates that were lying, now fixed

Both were found by making CI actually run, and both had been green while
checking nothing. Do not re-introduce either shape.

- **The fast gate had NEVER completed on a milestone-sized branch.** clang-tidy
  ran serially over every changed file — about a minute per TU with Qt headers
  — and blew the 30-minute job timeout. GitHub reports a timeout kill as
  "cancelled", which is what STATE.md previously recorded as the `concurrency:
  cancel-in-progress` setting doing it. It was never the concurrency group.
  Fanned out four ways; the gate then immediately found six real `bugprone-*`
  errors that had been in the tree since M2 was written.
- **The [i18n] tests compare the two .ts files with each other and nothing
  else.** A `tr()` string missing from both passes green — the file's own
  header comment claimed the opposite. lupdate now runs in CI with
  `-locations none` (so the catalogues stop churning on every line shift) and
  the build fails if it changes anything. Proved it fires by injecting a probe
  string before trusting it.

## Verified facts — do NOT re-derive

- **`SetSearchPathMode` does NOT harden `CreateProcessW`.** Its documented
  scope is `SearchPath` only. CreateProcessW's own search (used when
  `lpApplicationName` is null) looks in the calling process's directory and the
  CURRENT directory before System32. The documented mitigation is to resolve
  the path yourself and pass it as `lpApplicationName`, quoted in the command
  line — which is what `resolveShellCommand` does.
- **Qt reports `RemoteHostClosedError` for a graceful FIN and for a reset
  alike.** Measured, both paths, against the test server. Telnet carries no
  logout message either, so a clean logout and a dropped connection are
  genuinely indistinguishable at that layer. Treated as a clean exit; reconnect
  therefore covers connect failures only.
- **`ssh_jump_callbacks_struct` has no `size` member**, unlike libssh's other
  callback structs — no `ssh_callbacks_init()`. libssh keeps the pointer, so
  the structs must outlive `ssh_connect`. ADR-0012's open item is now closed.
- **`QTimer::stop()` from another thread is refused silently.** The app tears
  backends down on a thread pool (correct for SshBackend, which blocks in
  libssh), so `TcpBackend::stop()` hops to its own thread first — queued, never
  blocking, because `~QThreadPool` waits on the GUI thread and a blocking hop
  deadlocks.
- **Qt caps socket READS and has no write-buffer cap at all.** Telnet's
  subnegotiation answers amplify 6 bytes to 20, so a peer that floods and stops
  reading grew the queue unboundedly. Capped at 1 MB pending.
- **A QML `Timer` reports `running == true` and never fires in a window that is
  never composited.** The UI self-test is driven from `main()` by
  `QMetaObject::invokeMethod`, the same way the screenshot hook is.
- **A `Repeater` delegate must be an Item**, so `Shortcut` cannot be one.
- **`GUID_DEVINTERFACE_COMPORT` is in ntddser.h**, not setupapi.h, and needs
  `DIGCF_DEVICEINTERFACE` or the GUID is read as a setup class.
  `SetupDiOpenDevRegKey` fails with `INVALID_HANDLE_VALUE`, not null.
  `SPDRP_HARDWAREID` is REG_MULTI_SZ and composite devices append `&MI_zz`.

## What is NOT done

- **T59-T62 have not started.** Port forwarding, the agent bridge, FIDO2/certs
  and the ssh_config/mRemoteNG importers are all M3 scope and all absent.
- **No multi-hop contract test.** ADR-0002 asks for a two-hop chain including
  failure mid-chain. The in-process libssh test server takes one connection, so
  a two-hop fixture needs it to listen twice. T58 shipped without it and said
  so.
- **`ConptyBackend::resolveShellCommand` and the serial backend have no
  hardware-level test.** The pure parts are covered; opening a real COM port
  and replugging it needs a person and a USB adapter.
- **The M3 demo has not been run**: plug a USB serial adapter, watch it appear
  with a friendly name, unplug and replug it, toggle the hexdump. Needs
  hardware this session did not have.
- **Splits are a flat list with one orientation per tab**, not a tree. Marked
  in `SessionPane.qml` with its upgrade path.
- **Serial replug detection polls** once a second while disconnected, rather
  than using `CM_Register_Notification`. Marked with its upgrade path.

## From M2, still true

- **`mlkem768x25519-sha256` does not work in this build.** libssh 0.12
  advertises it, OpenSSH 10 prefers it, the two ends negotiate it, and the
  client then fails with "Failed to construct client init buffer" before
  sending its KEX init. This is why `src/net/ssh/algorithms.h` omits PQ key
  exchange. A POSTPONEMENT: re-test on the next libssh bump.
- **libssh 0.12 has no key-size accessor**, so randomart titles read
  `[ED25519]` where ssh-keygen writes `[ED25519 256]`. The art is identical.
- **`ssh_bind_set_blocking(bind, 0)` does not make accept non-blocking on
  Windows.** The test server wakes it with a self-connect.
- **`ssh_send_keepalive` is declared in `server.h`** but works for a client.
- **NOMINMAX must precede every include in a file that reaches Qt headers.**
- **`tr(runtimeString)` is invisible to lupdate** — the action registry repeats
  its labels as `QT_TR_NOOP` literals and a test compares the two lists.
- **The corpus `reports/` directory asserts REPLIES**, `csi/` asserts cursor
  state, `parser/` asserts tokens. Each case starts from a FRESH terminal.

## Build environment

The shell has no dev environment. Every build needs vcvars64 plus `QT_ROOT`
(`C:\Qt\6.10.3\msvc2022_64`); `VCPKG_ROOT` comes from vcvars itself and is the
VS-bundled vcpkg, which is what built this tree — do not override it with
`C:\vcpkg`, which does not exist here. Invoke a self-contained `.cmd` via
`MSYS_NO_PATHCONV=1 cmd.exe /c 'C:\abs\path.cmd'` with single quotes.

**Do not write `\r\n` inside a python or bash heredoc** — it collapses to a raw
newline, MSVC then reports "newline in string literal", and it has now cost two
repairs this session. Use `chr(92)` or edit the file directly.

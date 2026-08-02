# STATE

Phase: M6 — platform & 1.0. **PR #28 is open, fast-gate GREEN, MERGEABLE —
it needs a human to merge it.** Seven commits on `t84-m6-platform`.

## Now

M5 merged (`d31d644`). This session closed M5's one real gap and landed four
of M6's seven tasks. **619 tests green**, up from 591. Build clean under
`/W4 /WX`; clang-format clean under **20.1.8** (the version CI runs — local is
22.1.2 and they disagree); clang-tidy clean on every changed source; fuzz smoke
clean (3702 parser runs, 38370 telnet, zero crashes); `cpack -G ZIP` produces a
populated package.

| Task | What landed |
|---|---|
| T84 | The renderer paints sixel, kitty graphics and OSC 66 sized text. M5's gap |
| T87 | `packaging/` — portable ZIP, CPack NSIS config, winget manifests |
| T88 | `docs/configuration.md` — syncing config, and what must never travel |
| T86 | `src/app/crash/` — local minidumps, no Breakpad, no submission |
| T90 | `web/index.html` — one static page, first on M6's cut line |

T84 also fixed a **core** bug M5 recorded and deferred. A placement's anchor was
`linesEverStarted() + grid.row`, which mixes logical-line space with visual-row
space: it over-counts by one per wrapped row above the cursor and the error
ACCUMULATES as those rows retire. M5 guessed it was a constant off-by-one at
placement time; it is neither. `Grid::stableLineOfScreenRow`,
`Grid::viewportTopStable` and `Scrollback::stableAtVisualFromEnd` are now the
only conversion between the two spaces.

## Next task (exactly one)

**Fix the three BLOCKING findings from T89's security review of `src/net/`.**
They are all PRE-EXISTING — none came from this session's changes — and the
first one is bad enough that it outranks finishing M6.

### 1. ProxyJump silently connects DIRECTLY to the target

`src/net/ssh/ssh_backend.cpp:327`. **Verified against the pinned libssh source**,
not taken on trust: `build/fuzz-msvc/vcpkg_installed/vcpkg/blds/libssh/src/
libssh-0-e5f4972781.clean/src/client.c:621-630` wraps the whole ProxyJump
connect branch in `#ifndef _WIN32 / #ifdef HAVE_PTHREAD`. MSVC defines `_WIN32`
and not `HAVE_PTHREAD`, so `ssh_socket_connect_proxyjump` is not compiled in and
`ssh_connect` falls through to the final `else` — a plain `ssh_socket_connect`
to the target host.

What makes it dangerous rather than merely missing: `ssh_options_set(
SSH_OPTIONS_PROXYJUMP, …)` still SUCCEEDS and still fills `opts.proxy_jumps`,
and `SSH_OPTIONS_PROXYJUMP_CB_LIST_APPEND` still stores our callbacks. Nothing
reads either. The session comes up looking normal wherever the target is
reachable without the bastion — every deployment where a jump host is an audit
or policy control rather than a network necessity — the bastion is never
contacted, its host key is never checked, and every guarantee ADR-0012 records
is vacuous on the only platform Krait ships on. `ssh_config_import.cpp:323`
imports `ProxyJump` straight out of `~/.ssh/config`, so this is a live path.

**The fix was written this session and could not be landed** — see "Watchouts".
Fail closed: replace the whole `if (!m_config.proxyJump.empty()) { … }` block
with a `fail(ErrorCode::ConnectFailed, …)` and `return`, saying the build has no
ProxyJump support and that connecting would bypass the jump host. Keep
`countProxyJumpHops` and the hop callbacks — they are what the real chained
`direct-tcpip` implementation will need. Add a backend-contract test that a
config with `proxyJump` set never reaches `ssh_connect`.

Secondary, same area: if `OPENSSH_PROXYJUMP=1` is in the environment, libssh
builds an `ssh -W` **ProxyCommand** subprocess instead (`misc.c:2431`,
`config.c:602`) — banned by ADR-0002. Krait neither sets nor clears it.

### 2. Remote forwards are unbounded and block the SSH worker

`src/net/ssh/forward_manager.cpp:218-270`. Two defects, one trigger — the server
choosing how many `forwarded-tcpip` channels to open. `m_impl->tunnels` has no
cap, and each tunnel holds a socket plus two 1 MiB buffers. And `getaddrinfo` +
`connect` are synchronous with no timeout on the SSH worker thread —
`setNonBlocking` runs at `:259`, AFTER the connect — so while that sits there the
terminal receives nothing, no other tunnel is serviced, and `stop()`'s join waits
with it, so closing the tab does not help. Needs the user to have configured a
`-R` forward; the server picks when it fires.

### 3. SerialBackend closes the handle under in-flight I/O

`src/net/serial/serial_backend.cpp:355-361`. `stop()` runs on a `QThreadPool`
thread and calls `closePort()` — `CloseHandle` then `m_handle = nullptr` —
**before** joining the workers. `ConptyBackend` already fixed exactly this
(`conpty_backend.cpp:383-398`, "closed handle values recycle"); serial did not
copy it. Separately the device-removal path posts a lambda that joins and
reassigns `m_reconnect` on the GUI thread with no `m_shutdown` check, racing
`stop()`'s own join of the same `std::thread` — and if it lands after `stop()`
completes it spawns a thread nothing will join, so `~std::thread` calls
`std::terminate`. Trigger: unplug a USB serial adapter and close the tab.

Five non-blocking findings are in the same review — vault `path()`/`error()`
returning references without the lock, duplicate vault keys defeating "forget
this password", every `ssh_options_set` return discarded (fail-open on the
algorithm policy), no bidi/format-control stripping in `remote_text.cpp:41`, and
remote-forward port mismatch routing to `statuses[0]`.

## After that, to finish M6

- **T85 — the Lua API (sol2).** NOT STARTED, and the largest thing left. It adds
  a vcpkg dependency and a scripting sandbox, which is a security surface;
  `docs/plan/01-milestones.md` cuts UI extensions before the event API, so the
  event API is the part that must land. Deliberately not begun at the end of a
  long session — a hastily sandboxed script engine is how you ship a hole.
- **T89 remainder.** The audits RAN and are recorded above; the fixes are not
  applied. Fuzz smoke and the perf A/B are done and clean.

## Open questions

- **Nothing in M5 or M6 has been run by a human.** The theme gallery, the
  background image, the graphics renderer, the crash handler and the package all
  pass tests and reviews and have never been looked at. **user-decides.**
- **The WARP perf baselines are stale and now fail on this machine — for
  everyone.** `bench/baselines/m1-wrap.json` and `m2-wrap.json` were captured
  vsync-bound (`cpu_avg_ms` is exactly `1000/fps` in every row), so they measure
  frame pacing rather than renderer cost. Unmodified `main` measured today is
  36% off them. T84 itself is clean: an interleaved 11-run A/B against `main`
  put every warm figure inside ±2.5%. **Re-record them, or every future audit
  reads as a failure.** No baseline file was modified.
- **The hardware D3D11 leg still cannot be measured.** `KRAIT_GPU=hardware`
  exits 2 on the 60 s watchdog on both branches — and now does so WITH a 179 Hz
  display attached, which was m1/m2's recorded explanation for the same failure.
  That explanation is therefore wrong and the cause is unknown. Last real
  hardware number is T25's 140.7 fps, four milestones stale.
- **M3's serial demo has still never been run** — carried over, and finding 3
  above is in that same file.
- **First-run discoverability** — carried from M3/M4/M5. **user-decides.**

## Watchouts

- **`main` is protected.** Branch + PR, always.
- **The build shell has no dev environment.** vcvars64 (VS **18** Community)
  plus `QT_ROOT=C:\Qt\6.10.3\msvc2022_64`; `VCPKG_ROOT` comes from vcvars. A
  plain `cmake --build` fails with `Cannot open include file: 'type_traits'`,
  which is that and nothing subtler.
- **A build permission block ended this session's code work.** After the
  security review was read, the harness's auto-mode classifier began refusing
  the build wrapper — "blocking for safety because of earlier conversation
  content". The ProxyJump fix above was written, then REVERTED unbuilt rather
  than committed unverified onto a green PR. Whoever picks this up may need to
  allow the build command explicitly, or start a fresh session.
- **clang-format 20.1.8 vs local 22.1.2.** CI runs 20.1.8 and they disagree.
  `pip install --target <dir> clang-format==20.1.8` and use
  `<dir>/clang_format/data/bin/clang-format.exe` — NOT `<dir>/bin/`, which is a
  shim that reports the local version.
- **CI tidies CHANGED files.** Touching a file with pre-existing clang-tidy
  errors makes them yours. Two were fixed this session for that reason
  (`microsoft-exception-spec` on the DirectWrite overrides, a missing
  designated-field initializer in `scrollback_test.cpp`).
- **A failed build leaves the OLD test exe in place, and ctest then reports
  "All tests passed" from it.** Never read a ctest result without confirming the
  build before it exited 0.
- **A hung ctest holds `krait-qt-tests.exe` open and the next link fails
  LNK1168:** `taskkill /F /IM krait-qt-tests.exe` first.
- **`cpack -G NSIS` is configured but has never run** — NSIS is not installed
  here. `packaging/README.md` lists that and four other release blockers,
  including that CPack's stock NSIS template hardcodes
  `RequestExecutionLevel admin`, so the winget manifest's `Scope: user` is
  currently a claim the installer does not honour.
- **A package built without `windeployqt` contains no Qt and will not start.**
  `qt_generate_deploy_qml_app_script()` does not work in this tree; the reason
  and the replacement command are in `packaging/README.md`.

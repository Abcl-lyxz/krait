# STATE

Phase: M4 — power tools. Feature-complete on `t64-m4-power-tools`, in review.

## Now

M4 is built and gated. Nine commits on `t64-m4-power-tools` (`3f4252a`..`385dbaa`),
branched from the M3 merge `8627724`. **529 tests green**, up from 402. Build clean
under `/W4 /WX`, clang-format clean, clang-tidy clean on the gated
`bugprone-*`/`concurrency-*` checks. The working tree is clean and the branch is
pushed; the PR is open and needs a human to merge it.

Every feature `docs/plan/01-milestones.md` lists for M4 shipped, including all
three cut-line items — editor round-trip, broadcast, quake mode. Nothing was cut.

| Task | What landed |
|---|---|
| T64 | `src/net/ssh/sftp.{h,cpp}` — SFTP on the SSH worker thread, no second session |
| T65 | `src/app/sftp_model.*` + `qml/FilePanel.qml` — dual-pane panel, drag-drop |
| T66 | OSC 133 A/P/B/C/D marks on grid lines, surviving scrollback and reflow |
| T67 | OSC 9;4 → `ITaskbarList3`, `Shell_NotifyIconW` notification, jump-to-prompt |
| T68 | `src/app/session/triggers.*` — regex over output: highlight/notify/log/send |
| T69 | Snippet bar |
| T70 | Logging UX — path template, three formats, honest secret position |
| T71 | `src/app/input/copy_mode.*` — vim-keys, cluster- and width-correct |
| T73 | `assets/shell-integration/*`, rc-file auto-install over SFTP, editor round-trip |
| T74 | Broadcast-with-interlock, quake mode + global hotkey |

## Next task (exactly one)

**Merge the PR, then start M5.** Watch CI settle and merge. If CI fails it will
almost certainly be clang-format on a file a hook missed — that is what happened
to PR #25, and the fix is to run clang-format in place and push, not to argue
with the gate.

M5 is theme gallery + live editor, background image/blur/acrylic, sixel + kitty
graphics, OSC 66, mode 2048, mode 2031 + OSC 10/11, CRT shader. Read
`docs/plan/01-milestones.md`. **Before starting it**, note that M4 left 47 hex
colour literals across four new QML files with `TODO(theme)` markers, and
`theme.name` is a dead setting — the theme system does not exist at all. M5's
first task is that system, and those literals are its first customer.

## Open questions

- **Nothing in M4 has been run by a human.** The whole milestone is verified by
  tests and by reading. **user-decides** whether to do a manual pass before merge;
  the list of what only a human can check is under "Not covered by any test".
- **M3's serial demo still has never been run** — carried over, unchanged. Needs a
  USB serial adapter.
- **First-run discoverability** — carried over from M3 and now worse: M4 added
  five palette commands and five shortcuts, and still nothing tells a new user
  that `Ctrl+Shift+P` exists. **user-decides**.

## Watchouts

- **`main` is protected.** Branch + PR, always. A direct commit is refused.
- **The build shell has no dev environment.** vcvars64 (VS **18** Community, not
  2022) plus `QT_ROOT=C:\Qt\6.10.3\msvc2022_64`; `VCPKG_ROOT` comes from vcvars —
  do not override it. A hung ctest holds `krait-qt-tests.exe` open and the next
  link fails LNK1168: `taskkill /F /IM krait-qt-tests.exe` first.
- **A failed build leaves the OLD test exe in place, and ctest then reports
  "All tests passed" from it.** This bit the M4 mutation testing: a mutation that
  failed to compile came back green, which reads as "the test is not load-bearing"
  when the truth is "the test never saw the change". Never read a ctest result
  without confirming the build before it exited 0.
- **Three `krait-app.exe` exist.** `build/dev/` is current; `build/rel/` and
  `build/release/` are stale enough to look like regressions when run by hand.
- **`Read` returns only line 1** of any file the claude-mem hook has observations
  on. Use `Grep` with pattern `^`, `output_mode: content`, `head_limit: 0`.

## Not covered by any automated test — M4's honest list

Do not read 529 green as "M4 is verified". These are the holes, and they are the
first place to look when something is wrong:

- **`interleaveShell()` is entirely uncovered.** Making it a complete no-op still
  passes all 529 tests — not the stderr drain, not the tunnels, not the resize,
  not the keystroke path that predates M4. The blocker is structural:
  `SshTestServer` serves *either* a shell *or* SFTP on one session, and a
  starvation test needs both channels live at once. Fixing it means restructuring
  the fixture's linear `auth → one channel → serve` flow into a multi-channel
  poll loop, in a fixture whose own comments record that libssh's callbacks-based
  server path is a Windows stub. This is the single biggest test gap in M4.
- **No SFTP fuzz harness.** `rules/net.md` wants fuzz seeds with new message
  handling; `tests/fuzz/parser_fuzz.cpp` fuzzes VT sequences, not SFTP wire
  messages, so a seed there would be theatre.
- **Every QML surface** — FilePanel, SnippetBar, BroadcastBar, the copy-mode and
  logging strips. There are no QML tests in this repo at all. Drag-and-drop and
  keyboard reachability are both untested.
- **The live Win32 calls**: `RegisterHotKey`, `ITaskbarList3`, `Shell_NotifyIconW`.
  Compile-checked only; they need a desktop session with Explorer running.
- **The editor launch path** — needs a real editor process.
- **The zsh `precmd_functions` reordering** in the shell integration script is
  reasoned from the documentation, not run against a real zsh with oh-my-zsh
  loaded. Same class of gap as the serial demo.
- **Docs↔schema agreement has no gate test**, which is exactly why the entire
  `[logging]` block drifted out of `docs/configuration.md` undetected.
- **The shortcut table vs `actions.cpp`** — 24 hardcoded duplicates in `Main.qml`,
  no gate test.

## Known defects, deliberately not fixed in M4

- **A `D` can still land on an older prompt** if a marked line is destroyed
  without its mark being cleared — scroll-region scrolling discards `Line`s when
  `scrollTop != 0`. Found during the gate audit, distinct from the anchor bug that
  WAS fixed, and it needs its own corpus case.
- **Quake mode has no palette entry and no registered Action**, and
  `quake.hotkey` defaults to empty — out of the box it is reachable by neither
  keyboard, mouse, nor palette.
- **The shell-integration confirmation flow is mouse-only.** `BannerButton.qml`
  has no focus or `Keys` handling and the install sheet has no Esc path, so
  confirming a write to someone else's machine requires a mouse.
- **Telnet, raw and serial declare `reconnecting`** but nothing in the app listens
  to any of them, so broadcast cannot see their reconnects the way it now sees
  SSH's. Pre-existing; flagged in the code where the next person will find it.
- **`~TerminalItem` joins the backend on the GUI thread** while `resetSession()`
  correctly offloads it to the pool. Pre-existing.
- **Trigger highlights re-derive per output chunk**, not per frame, with a
  `ponytail:` comment naming the ceiling and the upgrade path.

## Facts verified during M4 — do not re-derive

- **libssh 0.12** (vcpkg, features `core;pcap;server`). `sftp_read` returns 0 for
  EOF and negative for error — the OPPOSITE of `ssh_channel_read`. No fixed max
  chunk: query `sftp_limits()`. The session must be in blocking mode.
  `sftp_handle_remove` takes the *info* pointer, not `msg->handle`.
  `sftp_client_message_get_data` NUL-terminates and truncates binary at the first
  zero byte — use `ssh_string_data`/`ssh_string_len`. SFTP v3 has no wire type
  field, so `permissions` must carry the format bits (`0100644`, `0040755`).
  `sftp_get_client_message` treats a timed-out read as fatal, so it cannot be
  called on an idle channel. Server-side SFTP needs `#define WITH_SERVER`; libssh's
  default handler suite is `#ifndef _WIN32` and is a stub here.
- **Qt cannot bound PCRE2.** `QRegularExpression::MatchOption` has three values and
  nothing in the class reaches `match_limit` or `depth_limit`. That is why triggers
  use `std::regex`, which throws `regex_error(error_complexity)` while matching.
- **`LoadIconW` returns a SHARED icon** — never `DestroyIcon` it. Microsoft's own
  taskbar sample does, which is only valid for its non-shared resource icon.
- **`ITaskbarList3`**: `HrInit()` before every other method, and
  `TaskbarButtonCreated` must arrive first. Qt already `OleInitialize`s the GUI
  thread, so no `CoInitializeEx`.
- **`QFileSystemWatcher` stops watching a file the instant it is renamed** — which
  is what write-then-rename editors do, and why the round-trip watches the
  directory too.
- **Microsoft and ConEmu disagree about OSC 9;4 state 4** (Warning vs Paused).
  Both map to `TBPF_PAUSED`, so the enum is named after the flag.
- **zsh, wezterm and DomTerm emit `OSC 133;P`, never `133;A`.** kitty sends
  `A;k=s` before PS2, so `k=` must be parsed, not ignored.
- M3's libssh facts still hold — see `git show 728cbff:STATE.md`.

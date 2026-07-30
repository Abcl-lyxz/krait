# STATE

Phase: **M2 ENGINE COMPLETE — T36-T51 done**, on branch `t36-backend-seam`,
PR #24. Not yet merged; not yet a product.
**Next task: the backend factory.** Read "The one thing that is missing" below
before anything else — it is small, it is the difference between a demo and a
product, and every remaining M2 acceptance item is blocked behind it.

## The one thing that is missing

**Nothing connects from the UI.** The SSH engine works and is contract-tested;
the palette lists saved sessions; choosing one raises a banner saying the
connection is not wired up. What does not exist is roughly one file:

    Profile -> SshConfig -> new SshBackend(config, vault) -> TerminalItem

`TerminalItem` already holds an `IBackend*` (T36 made that possible) and
hard-codes `new ConptyBackend(this)` in `ensureStarted()`. The factory replaces
that line with a switch on `Profile::backend`, forwards the host-key and
credential prompts to the existing Banner, and wires `SessionModel`'s
`sessionRequested` to opening one.

Do that and the M2 demo runs. Until then `krait ssh user@host` parses its
arguments, logs what it would open, and opens a local shell.

The second missing piece is tabs and splits — still one window, one terminal, as
in M1. The tab strip needs the same factory, so it follows naturally.

## What landed in M2

| Task | What |
|---|---|
| T36 | `IBackend` is the QObject seam; `krait-net` becomes a library |
| T37 | Session profiles, folder inheritance, the palette's fuzzy matcher |
| T38 | DPAPI vault + a `Secret` that is actually wiped |
| T39 | libssh 0.12 SSH backend, one worker thread, host-key gate |
| T40 | Randomart + the four new error banners, EN and TH |
| T41 | Auth ladder: agent, key, keyboard-interactive, password |
| T42 | Keepalive + a reconnect policy that knows what NOT to retry |
| T43 | In-process libssh test server + the contract suite |
| T44 | PuTTY importer, including what it refuses to import |
| T45 | Action registry, palette ranking, derived tree, QML palette |
| T46 | Settings page generated from the schema, searchable in Thai |
| T47 | Scrollback search + smart selection |
| T48 | Kitty keyboard, baseline flag, honest negotiation |
| T49 | OSC 8 + OSC 52 with the read permission gate |
| T50 | `krait ssh user@host` parsing |
| T51 | This wrap |

## Verified facts — do NOT re-derive these

- **`mlkem768x25519-sha256` does not work in this build.** libssh 0.12
  advertises it, OpenSSH 10 prefers it, the two ends negotiate it, and the
  client then fails with "Failed to construct client init buffer" before sending
  its KEX init. Against libssh 0.12.0 + OpenSSL 3.6.3 on MSVC. This is why
  `src/net/ssh/algorithms.h` exists and why it omits PQ key exchange. It is a
  POSTPONEMENT: re-test on the next libssh bump, and the T43 contract tests are
  the check.
- **libssh 0.12 has no key-size accessor.** Only `ssh_key_type` and
  `ssh_key_type_to_char`, so the randomart title reads `[ED25519]` where
  ssh-keygen writes `[ED25519 256]`. The art itself is identical.
- **`ssh_bind_set_blocking(bind, 0)` does not make accept non-blocking on
  Windows.** The FIRST accept returns because a client is already arriving; the
  second parks forever. The test server wakes it with a self-connect.
- **`ssh_send_keepalive` is declared in `server.h`** but works for a client
  session — libssh puts it there because servers use it too.
- **NOMINMAX must precede every include in a file that reaches Qt headers**, not
  just `<windows.h>`: Qt pulls windows.h in itself, so a define after it is
  dead. `std::min` in `ssh_backend.cpp` is where that surfaced.
- **`tr(runtimeString)` is invisible to lupdate.** The action registry is
  Qt-free by design, so all fourteen labels would have shipped untranslated.
  `session_model.cpp` repeats them as `QT_TR_NOOP` literals and
  `action_labels_test.cpp` compares the two lists both ways. Same shape of
  mistake as M1's `translate()` lambda.
- **The corpus `reports/` directory asserts REPLIES; `csi/` asserts cursor
  state; `parser/` asserts tokens.** A reply-shaped case in the wrong directory
  fails confusingly. Also: each case starts from a FRESH terminal, so a
  negotiation has to be one `IN` line.
- **A `type="vanished"` entry in a .ts file fails the i18n gate**, which treats
  any type attribute as untranslated. lupdate keeps them; they have to go.

## Watch out

- **`waitForAnswer` must be ARMED before the prompt is emitted, not inside the
  wait.** A directly-connected receiver answers inside the emit; clearing the
  flag afterwards discards that answer and then waits five minutes for it. The
  contract tests connect directly on purpose so this cannot regress.
- Every wait in `ssh_backend.cpp` is bounded and `stop()` notifies the condition
  variable — closing a tab during a host-key prompt or a 30-second backoff must
  not hold the join.
- clang-tidy rejects PARTIAL designated initializers
  (`missing-designated-field-initializers`). CI catches it; a local check needs
  `clang-tidy -p build/dev` on the changed files, which `/preflight` does not do.
- The palette bench and the flood bench both live in the normal suite. The flood
  is vsync-bound at 180 Hz, so `cpu_avg_ms` is the number that carries
  information, not fps.

## Evidence

| Gate | Result |
|---|---|
| `cmake --build --preset dev` | pass |
| `cmake --build --preset release` | pass |
| `ctest --preset dev` | **278/278** (was 199 at M1) |
| `ctest --preset release` | **278/278** |
| clang-tidy, changed files | clean |
| SSH contract suite vs in-process sshd | 8 cases, under 1 s |
| Palette, 2000 profiles | well under the 100 ms budget |
| Release flood, WARP, 60 fps budget | **PASS** — 177.6 fps, cpu 5.63 ms |
| Release flood vs M1 | no regression (180.0 -> 177.6 fps, 5.555 -> 5.630 ms) |
| Locales | 72 strings, 0 unfinished in EN or TH |
| App starts, QML loads | exit 0 (a failed load exits 1) |

Baseline: `bench/baselines/m2-wrap.json`.

## Open, not blocking

- **The hardware flood leg is STILL unmeasured**, the same as at M1 close and
  for the same reason: no usable attached display, so a hardware D3D11 present
  has nowhere to go. `KRAIT_GPU=hardware` exits non-zero with no frames. The
  last real hardware number is T25's 140.7 fps, now two milestones old. It needs
  one run on a machine with a monitor, and no code at all.
- **`cpp-reviewer` has NOT been run over this branch.** M1 ran it and it found
  three blocking issues, so this is a real gap rather than a formality. It is
  the first thing to do before merging PR #24.
- **The manual gates have still not been run by a human.** Same reason as M1.
  The palette, the settings page and the banners have been verified to LOAD (the
  app exits 0, and a QML failure exits 1), not to look right.
- OSC 8's spoof guard — showing the real target before a click — is a renderer
  obligation and is not implemented. The link is stored; nothing follows it.
- OSC 52 read permission has no UI to grant it. The core gate works and is
  tested; `allowClipboardRead` has no caller yet but the tests.
- The kitty keyboard ships flag 1 only. Flags 2/4/8/16 were the milestone's cut
  line. The negotiation is honest about it — ask for 31, get told 1.
- `.ppk` keys are imported as paths but libssh cannot read them. Conversion is
  unimplemented; auth falls back to the agent.
- Jump hosts (ADR-0012) are M3 and untouched.
- `src/core/grid/scrollback.cpp:47` still calls `shrink_to_fit()` in the
  continuation-append hot path. Pre-existing from T21, still deserves a ticket.
- Curly/dotted/dashed underlines still draw as one line. Pre-existing from T17.
- No settings migrations exist yet; `kSchemaVersion` is still 1.

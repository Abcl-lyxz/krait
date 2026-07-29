# Krait Milestones M0–M6

Refines IDEAS.md §6. Every milestone is shippable + demoable and gated by
`/preflight`. Acceptance = commands or scripted manual steps, never "works
well". Cut lines are ordered: first listed = first dropped under pressure.

## M0 — Skeleton that types (the gate milestone)

Two jobs: prove the toolchain, and prove QRhi can be a terminal renderer
(ADR-0001 2-week spike gate).

| Scope | Detail |
|---|---|
| Scaffold | git repo (public, MIT), CMake presets (dev), vcpkg manifest (pinned baseline), Qt 6.11.1 via aqtinstall, clangd via compile_commands, CI fast gate (ADR-0008) |
| Core subset | UTF-8 decoder; Paul Williams parser tables; C0 controls; CSI cursor family; SGR basic (0–29/30–49/90–107); ED/EL; grid with logical lines + wrap points + damage; capability table + honest DA1/DSR |
| Fuzz | libFuzzer parser target (clang-cl, ADR-0010) + seeds for every implemented family |
| Renderer spike | QQuickRhiItem + glyph-atlas quad grid (240×63), per-cell fg/bg; flood bench; run on dev GPU and D3D11-WARP |
| Backend | ConPTY via bundled OpenConsole (ADR-0011), IBackend v0, wired end-to-end |

**Acceptance (all must pass):**

```
cmake --preset dev && cmake --build --preset dev   # clean configure+build
ctest --preset dev                                  # unit + corpus green
tests\fuzz\run-smoke.cmd                            # 60 s fuzz, zero crashes
bench\spike\flood-report.cmd                        # emits fps + ms/frame table
```

- Spike gate numbers (recorded in `bench/baselines/m0-spike.json`):
  ≥60 fps sustained full-grid-change flood at 4K on dev GPU; ≥30 fps on WARP;
  renderer cost <10 ms/frame; atlas upload path exercised.
- Manual demo: open Krait → a PowerShell prompt appears → type `dir` →
  output renders → `cls` clears. (`vttest` menu renders legibly.)
- Go/no-go recorded as an addendum ADR referencing the numbers. No-go ⇒
  QPainter correctness path becomes M1 scope, renderer optimization forks off.

**Cut lines:** spike visual niceties (cursor styles, selection) → WSL/shell
detection → any QML beyond one window with one item.

## M1 — A local terminal you prefer over conhost

| Scope | Detail |
|---|---|
| VT | full SGR (truecolor, 256, 4:x styled+colored underlines 58/59, colon subparams), DECSTBM/IL/DL/SU/SD, modes 1049/2004/2026 (150 ms guard)/2027, honest DECRQM |
| Unicode | grapheme clusters (utf8proc), generated width tables, VS15/16, EAA setting; width identical in grid and renderer |
| Text | HarfBuzz+FreeType shaping, DirectWrite-enumerated fallback chain, ligatures, Nerd Font + emoji fallback |
| Reflow + scrollback | resize reflow from logical lines; scrollback with per-tab caps (compressed paging may slip) |
| Input | clipboard, paste-guard (multiline/sudo confirm, ESC/C0 strip, bracketed paste), IME (Thai/JP composition + candidate positioning) |
| App | settings registry v1 + TOML + hot reload, EN+TH locales, per-monitor DPI, portable mode, per-tab error banners |

**Acceptance:**

```
ctest --preset dev                                  # incl. reflow + width + corpus
bench\run.cmd --compare bench\baselines\            # flood ≥ M0 baseline, no >5% regression
tools\vttest-check.cmd                              # scripted vttest: implemented screens match golden
```

- Scripted IME demo: focus Krait → switch to Thai IME → type สวัสดี →
  composition draws at the cursor cell, commit inserts, width correct.
  Repeat with Japanese (へんかん → 変換 via candidate window).
- Manual: drag window between 100% and 150% monitors → no blur, metrics snap.
- Daily-drive checklist (10 items: prompt, vim, less, git log --graph, emoji,
  Thai file names, resize during vim, alt-screen restore, paste-guard fires,
  hot-reload a color) — all pass.

**Cut lines:** compressed/spillable scrollback (→M2) → ligatures polish →
portable mode → JP IME script (Thai stays).

## M2 — SSH + sessions (the PuTTY killer)

| Scope | Detail |
|---|---|
| SSH | libssh backend: password/key/agent/kbd-interactive auth, known_hosts TOFU UX (fingerprint + randomart), changed-key blocking banner, keepalive, auto-reconnect w/ backoff |
| Vault | DPAPI vault for passphrases/passwords; nothing plaintext (rules/net.md) |
| Sessions | profile tree (folders/tags/fuzzy search/bulk edit/inheritance), PuTTY registry importer |
| UI | tabs + splits polish, command palette, searchable settings UI, scrollback regex search, smart selection |
| VT | kitty keyboard protocol, OSC 8 (spoof-guarded), OSC 52 (write + gated read + caps) |

**Acceptance:** contract tests vs dockerized sshd fixture (connect, each auth
flow, host-key change, peer-vanish, reconnect); `krait ssh user@host` CLI
works; PuTTY import round-trip test; palette fuzzy-finds a session in <100 ms
(bench). Demo: import PuTTY sessions → connect prod profile → red accent +
changed-key banner scenario → search scrollback.
**Cut lines:** bulk edit → smart selection → kitty keyboard full-flags
(baseline flags stay).

## M3 — Protocol breadth

Telnet (negotiation), raw socket, serial (VID/PID names, auto-reconnect on
replug, DTR/RTS/break, hexdump, timestamped logs), jump hosts via native
ProxyJump + per-hop UX (ADR-0012), port-forward UI (L/R/D live pane), agent
interop (OpenSSH named-pipe bridge; own agent w/ confirm+timeout), FIDO2 keys
(libssh 0.12 sk API + libfido2 — prototype task first), certs, importers
(ssh_config, mRemoteNG, MobaXterm).
**Acceptance:** per-backend contract suites (serial vs com0com loopback;
telnet vs test server); jump-chain test through 2 hops incl. failure-mid-chain;
forward pane opens/closes/auto-reopens tunnels in test harness. Demo: plug USB
serial adapter → auto-appears with friendly name → unplug/replug →
auto-reconnects → hexdump toggles.
**Cut lines:** MobaXterm importer → own-agent confirm UI (interop bridge
stays) → dynamic SOCKS UI (engine stays).

## M4 — Power tools

SFTP dual-pane panel + drag-drop + editor round-trip, triggers
(regex→highlight/notify/log/send), snippet bar, broadcast-with-interlock,
quake mode + global hotkey, OSC 133 + bundled shell scripts + SSH
auto-install, OSC 9;4 taskbar progress, notifications, logging UX, vim-keys
copy mode.
**Acceptance:** SFTP round-trip test (edit remote file, save, re-download,
diff clean); trigger regression suite; OSC 133 marks navigate in scripted
scrollback. Demo: long build over SSH → progress in taskbar → notification on
finish → jump-to-prompt.
**Cut lines:** editor round-trip → broadcast → quake mode.

## M5 — Beauty + protocol completeness

Theme gallery (iTerm2/WT/base16 import) + live editor + per-profile accents,
background image/blur/acrylic, sixel + kitty graphics, OSC 66 (Thai marquee),
mode 2048, mode 2031 + OSC 10/11, CRT shader.
**Acceptance:** graphics corpus (kitty icat + img2sixel goldens); theme
import round-trip; OSC 66 renders the Thai demo doc at 2× sizes. Demo: notcurses
demo runs; theme editor live-preview.
**Cut lines:** CRT shader → acrylic → iTerm2 image protocol (already v2).

## M6 — Platform & 1.0

Lua API (sol2; events, UI extension points), config sync docs, NSIS installer
+ code signing + winget manifest (ADR-0007), portable zip polish, website,
crash reporting (local dump + opt-in submit), 1.0 hardening pass (fuzz budget
soak, security review of net/, perf soak).
**Acceptance:** `winget install krait` from a test manifest; signed installer
passes SmartScreen on a clean VM; Lua smoke suite; 72 h soak (10 sessions,
no leak growth >1%/h).
**Cut lines:** website polish → Lua UI extensions (event API stays) → crash
submit (local dumps stay).

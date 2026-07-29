# Krait — Product Brain

Vision, differentiators, feature bank, and roadmap. This document is opinionated
and evidence-backed; the raw evidence with sources is in
`docs/research/landscape-2026.md`. The plan session refines §6 into
`docs/plan/`; it does not relitigate §3/§4 without new evidence.

## 1. Vision

**Everything PuTTY should have become.** A single, fast, beautiful Windows
program that covers the whole "connect to things" job: SSH, local shells,
telnet, raw sockets, and serial — with a session manager you'd show off,
settings you can search, config you can put in git, rendering that handles
Thai/emoji/ligatures perfectly, and safety features that prevent the classic
"pasted `rm -rf` into prod" story. Free, no login, no telemetry, no
subscription.

One sentence per audience:

- *PuTTY user:* everything you know, plus tabs, search, sane paste, and a
  session tree — imports your saved sessions on first run.
- *MobaXterm/Xshell user:* the power features without the bloat or the
  license nag.
- *Termius user:* the sync and polish without the subscription.
- *WindTerm user:* the same speed, actually open and actively maintained.
- *Thai/SEA developer:* the first terminal where ภาษาไทย renders correctly
  everywhere — cells, tabs, prompts, editors.

## 2. Name

Working codename: **Krait** (งูสามเหลี่ยม — fast, precise, found in Thailand).
Binary `krait`. Alternatives kept warm: **Naga/Naka** (พญานาค — mythic, Thai
identity, but collides with the gfx-rs "naga" shader compiler), **Payanak**,
**Vanta**. Criteria: ≤2 syllables, typeable as a command, unique on GitHub +
winget. Renaming is a one-day sed until M2 branding lands; do not bikeshed
before then.

## 3. Top 10 differentiators (pain → feature → why we win)

1. **Session manager as a product** — PuTTY's flat list is its #1 complaint
   after tabs. Krait: tree with folders/tags, fuzzy search, bulk edit,
   profile **inheritance** (base → env → host overrides; a PuTTY wishlist
   item unshipped for ~20 years), one-click importers (PuTTY registry,
   `~/.ssh/config`, mRemoteNG XML, MobaXterm) — mRemoteNG is abandoned, its
   users are migration-hungry.
2. **Config as files, not registry** — human-readable TOML on disk,
   git/Dropbox-syncable, hot-reload, with a **searchable settings UI** that
   writes those files (VS Code model; Windows Terminal only got settings
   search in 1.25 — validated demand).
3. **Safety cues nobody ships on Windows** — per-profile accent: prod = red
   border/tab/watermark, host-pattern → profile auto-switch, and
   **paste-guard**: confirm multiline/`sudo`-ish pastes, strip ESC/C0,
   bracketed paste always. Kills PuTTY's most infamous failure mode
   (right-click = instant paste).
4. **Thai/CJK/emoji rendering as a marquee feature** — HarfBuzz shaping +
   real grapheme clustering (mode 2027) + width tables + polished Windows
   IME, and **early adoption of OSC 66 text-sizing** (only kitty + foot ship
   it; first Windows client to do so owns the complex-script niche).
5. **Serial/embedded excellence** — COM auto-detect with VID/PID friendly
   names, **auto-reconnect on unplug/replug** (top embedded ask for a
   decade), baud presets, DTR/RTS/break buttons, hexdump view, timestamped
   logging. No modern GPU terminal does serial well.
6. **Security UX that guides** — host-key TOFU with fingerprint + randomart +
   plain-language "key changed" diff, FIDO2/sk-key enrollment wizard,
   built-in agent with per-use confirmation + timeout (both PuTTY wishlist
   items), DPAPI/Windows-Hello vault, OpenSSH-agent + 1Password interop.
7. **Visual tunnel + jump-host manager** — live port-forward pane with
   health/toggle/auto-reopen and a jump-chain diagram. State of the art is
   Xshell's (dated, paid). We implement jumps natively via direct-tcpip.
8. **Triggers, highlights, snippets — SecureCRT parity, free** — regex
   highlight sets, triggers (pattern → notify/log/send), snippet bar,
   broadcast-input to panes with safety interlock, expect-style login
   scripting, later full Lua.
9. **Searchable everything** — command palette (sessions, actions, settings,
   snippets), regex scrollback search, OSC 133 jump-to-prompt +
   command-status marks in scrollbar, scrollback export. Each individually
   validated by competitors; nobody combines them in an SSH-manager client.
10. **Modern VT completeness on Windows** — kitty keyboard protocol, kitty
    graphics + sixel, styled/colored underlines, sync output 2026, hyperlinks
    OSC 8, gated OSC 52, mode 2048 resize, OSC 9;4 progress — plus
    auto-installed shell integration over SSH. MobaXterm/Xshell/SecureCRT are
    all VT-poor; that's the flank.

## 4. Feature bank

Tags: [M0–M6] target milestone, [v2] post-1.0, [moon] moonshot.

### Terminal core
- Own VT parser (Paul Williams state machine), table-driven, fuzzed [M0]
- Grid + scrollback as logical lines with wrap points; reflow on resize [M0]
- Compressed, disk-spillable scrollback; per-tab caps; O(pages) memory [M1]
- Truecolor, 256, styled+colored underlines (4:x, SGR 58/59) [M1]
- Grapheme clustering (utf8proc), width tables, VS15/16, EAA setting, mode 2027 [M1]
- Sync output mode 2026 (with timeout guard) [M1]; kitty keyboard protocol [M2]
- OSC 8 hyperlinks (spoof-guarded) [M2]; OSC 52 write + permission-gated read [M2]
- OSC 133 shell integration + bundled scripts (bash/zsh/fish/pwsh, auto-install over SSH) [M4]
- Graphics: sixel [M5], kitty graphics [M5], iTerm2 images [v2]
- OSC 66 text sizing (Thai marquee) [M5]; mode 2048 resize notify [M5]
- OSC 9;4 progress → taskbar; OSC 9/99 notifications [M4]; OSC 7 cwd [M4]
- Honest DA/DECRQM/XTGETTCAP capability reporting [M1, forever]
- BiDi: explicitly out of scope; leave hooks [never in v1]

### Protocols / backends
- Local shell via bundled OpenConsole/ConPTY (PowerShell, cmd, WSL detect) [M1]
- SSH via libssh: auth (password/key/agent/interactive), known_hosts UI,
  keepalive, auto-reconnect with backoff [M2]
- ssh_config import; jump hosts (native direct-tcpip chains) [M3]
- Port forwarding L/R/D with live UI [M3]; X11 forwarding [v2]
- Agent: OpenSSH agent + Pageant interop; own agent with confirm/timeout [M3]
- FIDO2 sk-ed25519/sk-ecdsa + certificates [M3]
- Telnet (with negotiation) + raw socket [M3]
- Serial: enumerate w/ friendly names, auto-reconnect, DTR/RTS/break,
  hexdump, logging [M3]
- SFTP: dual-pane browser panel + drag-drop + editor round-trip [M4]; zmodem [v2]
- Mosh [moon]; tmux control mode à la iTerm2 [moon]

### Sessions & config
- Profile tree: folders, tags, fuzzy search, bulk edit, inheritance [M2]
- Importers: PuTTY registry, ssh_config, mRemoteNG, MobaXterm, Xshell [M3]
- TOML config dir, hot reload, portable mode (config next to exe) [M1]
- DPAPI/Hello vault; master-password option; nothing plaintext ever [M2]
- Config sync via any file-sync/git (docs + conflict-safe format) [M2];
  optional E2E sync service [moon]
- Per-profile: colors/font/keybinds/env/startup-commands/logging [M2]

### UI / UX
- Tabs + split panes (tiling, drag to re-dock, detach to window) [M1–M2]
- Command palette Ctrl+Shift+P: actions, sessions, settings, snippets [M2]
- Searchable settings UI (VS Code model) writing TOML; every setting has
  doc tooltip + reset-to-default + "copy as TOML" [M2]
- Scrollback: regex search + highlight-all [M2], select/copy modes with
  vim keys [M4], smart selection (URL/path/IP) [M2], export [M2]
- Paste-guard + bracketed paste + control-char stripping [M1]
- Quake-mode dropdown; global hotkey [M4]; broadcast input w/ interlock [M4]
- Theme gallery: import iTerm2/Windows Terminal/base16 schemes; live editor
  with preview; per-profile accents; background image/blur/acrylic [M5]
- Ligatures + Nerd Font + emoji fallback chain; per-pane zoom [M1–M2]
- Latency/connection indicator; bell/activity per tab [M2]
- Keyboard-first: every action bindable; keybinding editor UI [M2]
- Localized UI: English + Thai at launch [M1]

### Automation & power
- Triggers: regex → highlight/notify/sound/log/send-text [M4]
- Snippet/quick-command bar with folders + variables [M4]
- Session logging: raw + timestamped, rotation [M2]
- Expect-style login scripting [M4]; macro record/replay [v2]
- Lua scripting API (sol2): events, UI extensions, custom commands [M6]
- CLI: `krait ssh user@host`, `krait --profile prod-db` [M2]
- "Command done" notifications via OSC 133 timing [M4]

### Quality bars (cross-cutting, every milestone)
- 60 fps under `cat` flood; input-latency budget; no per-byte rendering
- Fuzz-clean parser; ASan/UBSan green in CI; honest capability replies
- Per-monitor DPI, device-lost recovery, software (WARP) fallback
- IME (Thai/JP/CN) correctness tests; kitty-keyboard + IME interplay

## 5. Competitive frame (summary — full matrix in research doc)

| | PuTTY | WinTerm | WindTerm | Xshell | Tabby | **Krait target** |
|---|---|---|---|---|---|---|
| Modern VT (kitty kbd/gfx, 2027) | ✗ | partial | ✗ | ✗ | partial | ✔ full |
| Session tree + inherit | ✗ | ✗ | partial | ✔ | partial | ✔ |
| Serial done well | partial | ✗ | ✔ | partial | partial | ✔ |
| SFTP panel | ✗ | ✗ | ✔ | ✔ | partial | ✔ |
| Tunnel UI | ✗ | ✗ | partial | ✔ | ✗ | ✔ |
| Config as files | ✗ | ✔ | partial | ✗ | ✔ | ✔ |
| Native perf (no Electron) | ✔ | ✔ | ✔ | ✔ | ✗ | ✔ |
| Open + maintained | ✔ | ✔ | ✗ | ✗ | ✔ | ✔ |
| Thai/complex text | ✗ | partial | ✗ | ✗ | partial | ✔✔ |

## 6. Roadmap (each milestone = shippable, demoable, gated by /preflight)

- **M0 — Skeleton that types.** CMake+vcpkg+Qt scaffold, CI, core grid +
  parser subset (CSI/SGR basics), ConPTY echo in a bare QRhi window.
  *Accept:* type into PowerShell through Krait; corpus tests green; clangd
  works; `vttest` menu renders.
- **M1 — A local terminal you prefer over conhost.** Full SGR/truecolor,
  reflow, scrollback, clipboard + paste-guard, fonts/fallback/ligatures,
  grapheme width, IME, settings core (TOML + hot reload), EN+TH locales,
  per-monitor DPI. *Accept:* daily-drivable for local shells; vttest clean on
  implemented sections; flood benchmark ≥ target.
- **M2 — SSH + sessions (the PuTTY killer).** libssh backend, host-key UX,
  vault, profile tree + inheritance + search, tabs/splits polish, palette,
  scrollback search, kitty keyboard, OSC 8/52. *Accept:* replace PuTTY for
  daily SSH; import from PuTTY works.
- **M3 — Protocol breadth.** telnet, raw, serial (auto-reconnect, hexdump),
  jump hosts, port-forward UI, agent (+FIDO2, certs), importers beyond PuTTY.
  *Accept:* network-engineer + embedded workflows end-to-end.
- **M4 — Power tools.** SFTP panel, triggers/highlights, snippets, broadcast,
  quake mode, OSC 133 integration + notifications, logging UX.
- **M5 — Beauty + protocol completeness.** Theme gallery/editor, acrylic,
  graphics protocols, OSC 66 (Thai marquee), mode 2048, CRT shader for fun.
- **M6 — Platform.** Lua API, config sync docs, installer/winget/portable,
  website, 1.0 hardening.

## 7. Risks

| Risk | Mitigation |
|---|---|
| QRhi text rendering is unproven for terminals (we'd be first) | M0 spike gate: budget 2 weeks; fallback = QPainter path for correctness while optimizing |
| libssh lacks built-in ProxyJump | Own direct-tcpip chaining (ADR-0002); contract tests with sshd fixtures |
| Scope explosion (this doc is huge) | Milestone gates are cut lines; §4 tags are commitments, not wishes |
| Solo + AI development drift | STATE.md protocol, ADR discipline, /preflight before "done", corpus tests as ground truth |
| Windows API churn (ConPTY) | Bundle OpenConsole; pin + test on update |

## 8. Non-goals for 1.0

BiDi text, macOS/Linux ports (architecture stays portable; core is
platform-clean), mosh, tmux control mode, AI features, cloud accounts,
plugins-as-processes. Say no by default; ADR to change.

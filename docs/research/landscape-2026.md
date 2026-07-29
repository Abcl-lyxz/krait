# Terminal landscape research — mid-2026 snapshot

Evidence base for `docs/IDEAS.md` and the ADRs. Compiled July 2026 from the
sources linked inline. When citing this doc, prefer re-verifying anything
load-bearing via MCP/WebFetch — adoption tables age fast.

## 1. PuTTY pain points (ranked by observed complaint frequency)

1. No tabs / one window per connection — an entire ecosystem (SuperPuTTY,
   mtPuTTY, mRemoteNG) exists only to fix this.
2. Flat session list: no folders/tags/search/bulk-edit/inheritance
   (wishlist `many-sessions`, `config-inheritance`, open ~20 years).
3. Config in registry, plaintext — can't diff/sync/backup (`config-locations`).
4. Settings dialog: dense tree, no search; editing a default across N saved
   sessions = load-edit-save × N.
5. Copy/paste: select-to-copy + right-click-instant-paste → accidental paste
   of destructive multiline commands; no confirmation.
6. Dated rendering: no ligatures, weak theming, HiDPI issues.
7. No integrated SFTP (forces WinSCP switching).
8. Key friction: PPK↔OpenSSH conversion; Pageant lacks per-use confirm +
   timeout (wishlist `pageant-key-confirm`, `pageant-timeout`).
9. No scrollback search or export (`search-scrollback`, `save-scrollback`).
10. No auto-reconnect (KiTTY fork exists mostly for this).
11. Serial: window dies on disconnect; no COM auto-reconnect on replug.
12. Weak Unicode: no grapheme clustering, no styled underlines, poor emoji.
13. Fatal errors are global-modal dialogs (breaks wrappers, blocks work).
14. Win11 Modern Standby display-freeze bug; Windows-only mindset.
15. Perceived stagnation; resize/scrollback semi-bugs acknowledged upstream.

Sources: PuTTY wishlist (chiark.greenend.org.uk/~sgtatham/putty/wishlist/),
ctrlops.io/blog/putty-alternatives-windows, umatechnology.org Tabby-migration
article, superputty#643, mRemoteNG#243, Parallax/Particle serial threads.

## 2. What makes each competitor loved

- **Windows Terminal** — free/default; AtlasEngine (DirectWrite) renderer;
  1.22 added sixel + grapheme clusters + regex scrollback search + snippets;
  1.25 (Mar 2026) added kitty keyboard + settings search + keybinding GUI.
- **Alacritty** — lowest-latency class, GPU-first, TOML config; deliberately
  minimal (no tabs/ligatures).
- **WezTerm** — Lua-programmable everything; built-in mux + reconnectable SSH
  domains; best-in-class HarfBuzz shaping/fallback; supports nearly every
  protocol extension.
- **Kitty** — protocol innovator (graphics, keyboard, OSC 66 text sizing);
  top Unicode correctness; "kittens" extensions.
- **Ghostty** — native feel, zero-config defaults, top Unicode scores;
  **no official Windows build as of mid-2026** → gap.
- **Warp** — block-based output (command+output units), team sharing, AI;
  resented for login requirement + telemetry → our counter-position.
- **Tabby** — pretty, searchable launcher, serial; hated for Electron RAM.
- **MobaXterm** — auto-SFTP sidebar, X11, broadcast; criticized as bloated.
- **Xshell** — best session manager + Compose/broadcast bar + highlight sets
  + live tunnel pane + triggers (v8); paid/dated; free-for-home only.
- **SecureCRT** — enterprise standard: triggers, button bar, scripting,
  FIPS; expensive.
- **Termius** — cross-device E2E sync, snippets, teams; subscription is the
  #1 complaint → migration pool.
- **WindTerm** — the closest "modern PuTTY" (C++, fast, SSH/telnet/serial,
  SFTP, quake mode); development went irregular / partially closed → trust
  gap to exploit.
- **mRemoteNG** — multi-protocol tree; stagnant (portable build abandoned
  Apr 2025); embeds PuTTY and inherits its modal fragility.
- **iTerm2 (reference)** — tmux -CC, triggers + automatic profile switching
  (prod-red), Python API, shell-integration marks, password manager.
- **Rio** — WebGPU renderer, CRT shaders, broad OSC support.

## 3. VT extension adoption (mid-2026) → Krait verdicts

| Extension | Adoption | Verdict |
|---|---|---|
| kitty keyboard protocol | kitty, foot, WezTerm, Ghostty, Alacritty, Rio, iTerm2, WT 1.25 | Implement fully (all flags) [M2] |
| OSC 133 shell integration | WT, iTerm2, Ghostty, kitty, foot, VSCode; zsh upstream patches 2025 | Implement + bundle scripts + SSH auto-install [M4] |
| OSC 8 hyperlinks | Universal among moderns | Implement, spoof-guarded [M2] |
| Graphics | kitty-gfx: kitty/WezTerm/Ghostty/Konsole; sixel floor incl. WT 1.22 | kitty + sixel [M5]; iTerm2 imgs [v2] |
| Grapheme clustering / mode 2027 | Ghostty+kitty lead; WT 1.22 | Internal clustering always; 2027 as signal [M1] |
| Sync output mode 2026 | Widely adopted; TUI frameworks emit it | Implement + ~150 ms timeout guard [M1] |
| Styled/colored underlines 4:x, 58/59 | All moderns; PuTTY lacks | Implement [M1]; parse colon subparams correctly |
| OSC 52 clipboard | Write universal; read gated everywhere sane | Write + permission-gated read + size cap [M2] |
| OSC 66 text sizing | Only kitty + foot; Ghostty pending | Early-adopt = Thai marquee [M5] |
| Mode 2048 resize notify | WezTerm, iTerm2 tracking | Implement [M5] |
| Mode 2031 color-scheme notify, OSC 10/11 | Contour-spec family, moderns | Implement with theme system [M5] |
| OSC 9;4 progress / OSC 9, 99 notify | ConEmu-origin; WT taskbar; Ghostty | Implement [M4] |
| OSC 7 cwd, XTGETTCAP, DECRQM | Expected by modern TUIs | Implement + honest replies [M1/M4] |

## 4. Stack verdicts (feeding ADRs)

- **Qt 6**: 6.8 is LTS but patch releases are commercial-only → open-source
  tracks latest (6.10.x/6.11.x). QRhi is public API since 6.6, abstracts
  D3D11/12/Vulkan/Metal/GL; Qt Canvas Painter (6.11 tech preview) does GPU
  text on QRhi via SDF → text-on-QRhi is viable. **No shipping terminal uses
  QRhi yet** — we'd be first (risk logged in IDEAS §7). The D3D11 +
  glyph-atlas pattern is exactly Windows Terminal's AtlasEngine.
- **libssh over libssh2**: libssh2 had a critical malicious-server RCE in
  2026 — CVE-2026-55200, CVSS 9.2, public PoC, all ≤1.11.1, fix unreleased at
  disclosure (+ CVE-2026-55199, CVE-2025-15661). For a client that connects
  to untrusted servers, disqualifying. libssh (LGPL-2.1, dynamic link):
  client+server, parses ssh_config, known_hosts API, FIDO2 sk-keys via
  ssh_sk_callbacks + libfido2, OpenSSH certs. Gap: ProxyJump shells out to
  OpenSSH → implement jump chains ourselves via direct-tcpip
  (gitlab libssh-mirror issue #178). libssh 2026 CVE-2026-0968 (SFTP heap
  read) assessed low-risk, patched.
- **Text shaping**: WezTerm = HarfBuzz shaping + DirectWrite only for system
  font *discovery* on Windows; WT = pure DirectWrite (locks into Windows);
  Ghostty = CoreText/FreeType+HarfBuzz. For Thai + emoji + Nerd Font
  fallback with future portability: **HarfBuzz + FreeType + DW enumeration**
  (the WezTerm recipe).
- **ConPTY**: passthrough still officially "Future"; DCS not passed through
  (terminal#17313). WT 1.22 shipped a rewritten host (~2× VT throughput).
  Rule: **bundle our own OpenConsole/conpty.dll** and update it (WezTerm
  practice, wezterm#7774); support win32-input-mode.
- **Unicode width**: wcwidth alone is provably wrong (farmer emoji = 4 cells
  under wcwidth). Grapheme clustering (utf8proc) + generated width tables +
  VS15/16 + configurable East-Asian-Ambiguous (WT 1.25 added the setting).
- **Packages**: vcpkg (manifest mode, Ninja, VS integration) for deps; Qt
  itself via aqtinstall/official installer (Qt-in-vcpkg = hours-long builds).
  Conan only wins for private-repo matrices we don't have.
- **MSVC C++23**: `/std:c++23preview` solid today; full `/std:c++23` lands
  with VS2026 18.x once P2564/P0533 complete. Target C++23.
- **VT core libraries**: libvterm (MIT, powers nvim :terminal) and libtsm
  (dormant) both lag modern extensions (kitty kbd/gfx, 2026/2027, OSC 66,
  reflow quality). **Every quality terminal (kitty, Ghostty, WT, WezTerm,
  Alacritty, foot, Contour) wrote its own core.** Verdict: write our own
  (Paul Williams DEC state machine), validate against vttest/esctest,
  fuzz from day one.

## 5. Hard-won failure modes (encoded into `.claude/rules/`)

- Reflow cannot be retrofitted → logical lines + wrap points from day 1.
- Never render per byte; parse chunks, coalesce damage, vsync; 2026-mode
  needs a timeout so a stuck app can't freeze the display.
- Scrollback memory O(pages): compressed paged ring; Electron-class RAM is
  the #1 churn driver.
- IME is a feature: composition positioning, never eat keys mid-composition;
  even Alacritty still breaks CJK IME (#6942).
- Glyph atlas: growth caps + LRU + D3D device-lost recovery + WARP fallback;
  per-monitor DPI change without restart.
- Clipboard: bracketed paste, strip ESC/C0, confirm multiline, gate OSC 52
  read, cap sizes.
- Per-tab errors only (SuperPuTTY/mRemoteNG modal-dialog lesson).
- Secrets never plaintext (PuTTY registry lesson) → DPAPI.
- Honest DA/DECRQM replies (iTerm2/Konsole got publicly dinged).
- BiDi is unsolved in terminals; de-scope, leave hooks.

## 6. Key sources

PuTTY wishlist · tmuxai.dev/terminal-compatibility · jeffquast.com "State of
Terminal Emulation 2025" · thottingal.in 2026 complex-scripts/OSC 66 ·
WT 1.22 devblog + 1.25 discussion · mitchellh.com grapheme-clusters article ·
Contour vt-extensions specs · kitty keyboard/graphics specs ·
CVE-2026-55200 coverage (TheHackerNews, Arctic Wolf) · api.libssh.org FIDO2
tutorial · libssh-mirror#178 · doc.qt.io QRhi/RhiWindow + Qt Canvas Painter
blog · MS ConPTY ecosystem roadmap + terminal#17313 + wezterm#7774 · MSVC
C++23 blog posts · vtdn.dev underline tables · netsarang Xshell features ·
vandyke SecureCRT docs · alternativeto WindTerm profile · portapps mRemoteNG
abandonment notice.

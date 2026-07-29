# Krait — a modern PuTTY successor (C++ / Qt 6, Windows-first)

**Read `STATE.md` first.** It has the current phase and the exact next task.
It is rewritten at the end of every session — `/handoff` does it properly.

Krait is a GPU-accelerated terminal + connection manager: SSH, local shells
(ConPTY), telnet, raw sockets, serial. The pitch: everything PuTTY does,
nothing PuTTY got stuck on — modern rendering, first-class Thai/CJK/emoji,
session management as a real product, and security UX that guides instead of
scares. Product brain: `docs/IDEAS.md`. Evidence: `docs/research/`.

## Stack (locked by ADR — read the ADR before proposing a change)

| Area | Choice | ADR |
|---|---|---|
| UI chrome | Qt 6 (QML), latest open-source release — NOT 6.8 LTS (patches are commercial-only) | 0001 |
| Terminal view | Custom QRhi glyph-atlas renderer, D3D11 primary backend | 0001 |
| SSH | libssh, dynamically linked (LGPL). libssh2 is banned | 0002 |
| VT core | Our own parser + grid. No libvterm/libtsm | 0003 |
| Text shaping | HarfBuzz + FreeType; DirectWrite only for system font discovery/fallback | 0001 |
| Unicode width | utf8proc grapheme clustering + generated tables. Bare wcwidth is banned | 0003 |
| Build | CMake presets + Ninja + MSVC `/std:c++23preview`; deps via vcpkg manifest; Qt via aqtinstall | — |
| Config | TOML files on disk (toml++); secrets ONLY in the Windows DPAPI vault | — |

ADRs live in `docs/decisions/`. Supersede with a new ADR, never edit a settled one.

## Commands

```bash
cmake --preset dev            # configure (Ninja, exports compile_commands.json)
cmake --build --preset dev    # build
ctest --preset dev            # unit + corpus tests
/preflight                    # all of the above + lint, with a pass/fail report
```

Until M0 lands the scaffold these presets do not exist yet — `STATE.md` is the
source of truth for what is real. Run `/preflight` before claiming anything
works; "it should work" is not a result.

## The two sacred rules

1. **The VT core is sacred.** `src/core/` has zero Qt/network/render deps.
   Every escape-sequence or width change lands with corpus tests in the same
   commit, and DA/DECRQM replies stay honest — never claim an unimplemented
   capability.
2. **Remote input is hostile.** Everything under `src/net/` follows
   `.claude/rules/net.md`. Host keys, secrets, and untrusted-input handling
   are never "cleaned up later".

## MCP-first (this is policy, not preference)

- **Qt API facts** → `qt-docs` MCP. **libssh / HarfBuzz / FreeType / toml++ /
  {fmt} / utf8proc facts** → `context7` MCP. Never code against a remembered
  API; verify or say explicitly that you didn't.
- **Symbol navigation / diagnostics** → the `clangd-lsp` plugin (needs
  `compile_commands.json` from the dev preset). Trust its diagnostics after
  every edit; use go-to-definition/references instead of grepping for symbols.
- **GitHub** → `gh` CLI (or the github MCP). **Standards/specs** → WebSearch +
  WebFetch on vt100.net, xterm ctlseqs, kitty/contour spec pages.
- Raw `grep`/`find` in Bash are denied by the guard hook on purpose — use the
  Grep/Glob tools. Fetching docs with curl is denied — use the MCPs above.
- Details and fallback order: `.claude/rules/mcp-first.md`.

## Map

- `src/core/` — VT parser, grid, scrollback, unicode. Pure C++23, zero deps.
- `src/net/` — backends: ssh, conpty, telnet, raw, serial. One dir per backend,
  all implementing `IBackend`.
- `src/render/` — QRhi renderer: glyph atlas, damage tracking, shaper cache.
- `src/app/` — Qt shell: QML chrome, settings registry, session manager, vault.
- `tests/` — unit + corpus + fuzz harnesses. `bench/` — perf baselines.
- `docs/decisions/` — ADRs. `docs/plan/` — the approved implementation plan.
- `.claude/rules/` — path-scoped rules; they load automatically when you touch
  the files they govern. Do not restate them here.

## Conventions (top level only — details live in rules)

- C++23, warnings-as-errors, clang-tidy clean. Formatting belongs to
  clang-format (a hook runs it); never hand-format.
- RAII everywhere; no owning raw pointers. Qt parent/child ownership is
  documented at each `new` site.
- No exceptions across module boundaries; `src/core/` uses `std::expected`.
- Minimum complexity for the current task. No speculative abstractions,
  no error handling for states that cannot happen.
- All user-facing strings go through `tr()` from day one. Thai ships as a
  first-class locale, not an afterthought.
- `.claude/hooks/` scripts: Node built-ins only, no deps, fast (<50 ms goal).

## Session protocol

- **Start:** the SessionStart hook injects STATE.md, branch, and build/test
  freshness. Trust it; do not re-derive.
- **During:** delegate full test runs to the `test-triager` subagent and diff
  reviews to `cpp-reviewer` (auto before every commit) — keep this context
  clean. Verify library facts via `docs-verifier` before coding against them.
- **End:** `/handoff` rewrites STATE.md. A session that ends without it
  strands the next one.

## Landmines (each of these has destroyed real terminal projects)

- wcwidth lies. Width comes from grapheme clustering + width tables + VS15/16
  + the East-Asian-Ambiguous setting. Mode 2027 is the negotiation signal.
- Scrollback stores logical lines + wrap points from day one. Reflow cannot be
  retrofitted into a fixed grid — that mistake is a rewrite.
- Bundle our own OpenConsole/conpty.dll (WezTerm does); inbox conhost is
  years stale and DCS passthrough has holes.
- Never render per byte. Parse in chunks, coalesce damage, present at vsync.
  Honor sync-output mode 2026 but with a ~150 ms timeout guard.
- Errors are per-tab banners. A global-modal dialog is a bug, always.
- IME is a feature: Thai/CJK composition must survive every input change.
  Test composition-window position after any input or renderer work.

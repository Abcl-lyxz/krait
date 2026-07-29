# MCP-first — the lookup order (always loaded)

Using raw Bash for a job an MCP tool or plugin covers is a policy violation,
not a preference. The `bash-guard` hook enforces the obvious cases; this rule
covers the rest. Why: guessed APIs are this project's #1 defect source — every
fact below has a cheap authoritative source.

| Question about | Use, in order |
|---|---|
| Qt API (QRhi, QML, QInputMethod, anything Qt) | `qt-docs` MCP → WebFetch doc.qt.io |
| libssh, HarfBuzz, FreeType, utf8proc, toml++, {fmt}, vcpkg ports | `context7` MCP → WebFetch official docs |
| Symbols in THIS repo (definitions, references, call sites) | `clangd-lsp` plugin tools — not grep |
| Compile errors after an edit | trust `clangd-lsp` diagnostics; fix in the same turn |
| VT/ANSI standards | WebFetch: vt100.net, invisible-island.net ctlseqs, kitty/contour spec pages |
| Win32/ConPTY behavior | WebFetch learn.microsoft.com; verify against bundled OpenConsole version |
| GitHub (PRs, issues, CI runs) | `gh` CLI first; `github` MCP for search/multi-repo |
| Current events, comparisons, "what do people use" | WebSearch |

Rules:

- Never write code against a remembered API signature for Qt, libssh,
  HarfBuzz, or FreeType. Verify first, or say in your reply that the API is
  unverified and why.
- Nontrivial verification work goes to the `docs-verifier` subagent so this
  context stays clean; its output format is claim → verdict → source link.
- If an MCP server is down or missing, say so explicitly and fall back to
  WebFetch on official docs. Guessing is the only forbidden fallback.
- `docs/research/landscape-2026.md` is evidence, not gospel — re-verify
  anything load-bearing before building on it; adoption tables age.

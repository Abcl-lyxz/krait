# .claude/ — the Krait development system

Everything here exists so that any Claude Code session (or human) starts
sharp, follows the same law, and cannot skip verification. Map:

| Piece | What it does |
|---|---|
| `settings.json` | Permissions (allow/ask/deny), hook registration, status line, MCP auto-enable. Shared — committed. `settings.local.json` is machine-local — never committed. |
| `rules/` | Path-scoped law. `mcp-first.md` always loads; the rest load automatically when files they govern are touched. |
| `skills/` | Slash commands + auto-invoked procedures: `/preflight` (the definition of "verified"), `/handoff` (STATE.md ritual), `/review`, `/add-escape-sequence`, `/add-setting`, `/new-backend`, `/release`. |
| `agents/` | Subagents: `cpp-reviewer`, `test-triager`, `docs-verifier`, `vt-spec-auditor`, `perf-auditor`. They keep noisy work out of the main context and carry project memory across sessions (`.claude/agent-memory/`). |
| `hooks/` | Deterministic enforcement (rules are advisory; hooks are not): `session-start` injects STATE.md + freshness; `bash-guard` denies force-push/no-verify/raw-grep/doc-curling (MCP-first); `post-tool` clang-formats edited C++ and tracks build/test freshness; `stop-gate` blocks ending with untested source changes (once, snoozed, inactive pre-scaffold); `session-end` journals. Node built-ins only, <50 ms goal, must never throw. |
| `statusline.cjs` | `Krait │ model │ branch* │ phase │ ctx%` — phase parsed from STATE.md's `Phase:` line. |
| `workflows/full-audit.js` | Heavy multi-agent audit (5 lenses + adversarial verification). Invoke via `/workflows` / ultracode when supported; otherwise `/review` covers the routine case. |

Conventions for changing this directory:

- Hooks: Node built-ins only, fail-open (exit 0 on any internal error),
  never block on ambiguity — deny only what is certainly banned.
- New skill: directory name = command name; description states WHAT + WHEN
  with the vocabulary you'd actually type; side-effect rituals get
  `disable-model-invocation: true`.
- New agent: description says "use proactively" + when; define an explicit
  output contract; give `memory: project` only when accumulated knowledge
  helps.
- Anything here is subject to `/review` like product code.

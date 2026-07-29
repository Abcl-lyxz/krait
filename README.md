# Krait

A GPU-accelerated terminal emulator and connection manager for Windows,
written in C++23 with Qt 6. Krait aims to be what PuTTY should have become:
SSH, local shells (ConPTY), telnet, raw sockets, and serial — with modern
rendering (ligatures, emoji, first-class Thai/CJK), a session manager that is
a real product, searchable settings stored as human-readable TOML, and
security UX that guides instead of scares.

> **Status: M0 planned.** The repository contains the product design,
> research, ADRs, the approved implementation plan (`docs/plan/`), and a
> complete Claude Code development system — no shipped code yet. See
> `STATE.md` for the current phase.

*"Krait" is a working codename (a fast, precise snake found in Thailand);
alternatives are listed in `docs/IDEAS.md §2`.*

## Where things are

| You want | Read |
|---|---|
| Current phase + exact next task | `STATE.md` |
| Product vision, feature bank, roadmap | `docs/IDEAS.md` |
| Market/tech evidence behind decisions | `docs/research/landscape-2026.md` |
| Settled architecture decisions | `docs/decisions/` |
| How this repo is developed with Claude Code | `CLAUDE.md` + `.claude/` |
| One-time machine setup (toolchain, MCP, plugins) | `SETUP.md` |
| The approved implementation plan (architecture, milestones, tasks, tests) | `docs/plan/` |

## Development model

This project is developed 100% with Claude Code. The `.claude/` directory is
the development system: path-scoped rules, skills (slash commands), subagents,
guard hooks, a status line, and workflows. `CLAUDE.md` is the contract every
session follows. Humans are welcome too — `CONTRIBUTING.md` has the
non-negotiables.

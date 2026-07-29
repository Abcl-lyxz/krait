# Machine setup

One-time setup for a new development machine. Expanded as the toolchain
lands (T2 of `docs/plan/02-m0-tasks.md` adds the exact versions/commands).

## Toolchain (needed from T2)

- Visual Studio 2026 (or 2022 17.x+) with C++ workload — MSVC `/std:c++23preview`
- CMake ≥ 3.30 + Ninja
- vcpkg checkout (`VCPKG_ROOT` set); deps come from `vcpkg.json` manifest
- Python 3 (table generators)
- Qt **6.11.1** via aqtinstall:
  `aqt install-qt windows desktop 6.11.1 win64_msvc2022_64 -m qtshadertools`
  (module list finalized in T2)
- LLVM for Windows (clang-cl) — fuzz builds (ADR-0010)

## Claude Code session tooling

- Approve the MCP servers in `.mcp.json` (qt-docs, context7) on first run.
- Plugins: `clangd-lsp` (needs `compile_commands.json` from the dev preset).
- `.claude/settings.local.json` and `.claude/.cache/` are machine-local —
  never commit.

## Repo

- `git clone https://github.com/Abcl-lyxz/krait` · read `STATE.md` first.

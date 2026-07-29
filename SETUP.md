# Machine setup

One-time setup for a new development machine. Expanded as the toolchain
lands (T2 of `docs/plan/02-m0-tasks.md` adds the exact versions/commands).

## Toolchain (needed from T2)

- Visual Studio 2026 (18.x) with C++ workload — MSVC `/std:c++23preview`.
  VS bundles CMake ≥ 4.2 and Ninja; no separate install needed.
- vcpkg checkout (`VCPKG_ROOT` set, e.g. `C:\vcpkg`); deps come from
  `vcpkg.json` manifest (baseline pinned there)
- Configure/build from a **VS x64 developer shell** (vcvars64), with
  `VCPKG_ROOT` set:
  `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`
- Python 3 (table generators)
- Qt **6.10.3** via aqtinstall (the pin is 6.11.1 — see the Qt section below
  for why that is not what you install today):
  `aqt install-qt windows desktop 6.10.3 win64_msvc2022_64 -m qtshadertools`
- LLVM for Windows (clang-cl) — fuzz builds (ADR-0010)

## Claude Code session tooling

- Approve the MCP servers in `.mcp.json` (qt-docs, context7) on first run.
- Plugins: `clangd-lsp` (needs `compile_commands.json` from the dev preset).
- `.claude/settings.local.json` and `.claude/.cache/` are machine-local —
  never commit.

## Repo

- `git clone https://github.com/Abcl-lyxz/krait` · read `STATE.md` first.

## Qt (from T11)

- Install Qt via aqtinstall; pin is 6.11.1 (ADR-0001) but aqt cannot fetch
  it - 6.10.3 msvc2022_64 + the qtshadertools module works (floor is 6.8).
  Cause (verified at T16): Qt 6.11+ moved to a new per-arch repository
  layout, aqtinstall support for it is merged but UNRELEASED, and the
  newest release on PyPI is still 3.3.0. So this blocks CI as well, and CI
  pins 6.10.3 for the same reason. Use the official Qt installer for 6.11.1
  when convenient; flip `QT_ROOT`, `QT_VERSION` in `.github/workflows/ci.yml`
  and the plan docs together.
- Set `QT_ROOT` (e.g. `C:\Qt\6.10.3\msvc2022_64`) in the dev shell; the
  dev preset reads it for CMAKE_PREFIX_PATH.

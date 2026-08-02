# Packaging Krait

ADR-0007 is the decision this implements: portable ZIP always, NSIS installer
and a winget manifest at 1.0, no MSIX.

## Building the artefacts

```bash
cmake --preset release
cmake --build --preset release

# Portable ZIP — carries the krait.portable marker, so config lives beside
# the exe and nothing is written to %APPDATA%.
cmake -S . -B build/release -DKRAIT_PORTABLE_PACKAGE=ON
cpack --config build/release/CPackConfig.cmake -G ZIP -B dist

# Installer — NO marker, so an installed copy uses the user profile.
cmake -S . -B build/release -DKRAIT_PORTABLE_PACKAGE=OFF
cpack --config build/release/CPackConfig.cmake -G NSIS -B dist
```

The two runs differ only in that flag. It exists because the portable marker is
a FILE and `settings/paths.cpp` decides portable mode by its presence — so the
same install tree cannot serve both, and teaching the NSIS script to delete a
file the ZIP needs is more moving parts than one option.

`cpack -G NSIS` needs **NSIS 3.03 or newer** on PATH (CPack's minimum since
CMake 3.22). `packaging/packaging.cmake` looks for `makensis` and only adds the
NSIS generator when it finds one — a machine without NSIS still builds and still
produces the ZIP, rather than failing configure over a tool it does not need.

## What is verified and what is not

| Piece | State |
|---|---|
| Install rules (`install(TARGETS)`, shell-integration, OpenConsole, licences) | **verified** — `cpack -G ZIP` produced a 14-entry tree with all of them |
| ZIP generator | **verified** — `krait-0.0.1-win64.zip` builds |
| Qt DLLs + QML imports | **NOT in the package** — a release-job `windeployqt` step, see below |
| NSIS generator | **NOT verified** — NSIS is not installed on the development machine |
| winget manifests | **schema-checked by hand, never submitted** |
| Code signing | **not implemented** — no certificate exists |

### Why Qt is deployed by the release job and not by CMake

`qt_generate_deploy_qml_app_script()` is the documented route and it does not
work in this tree. The script it writes calls `qt6_deploy_qml_imports()`, but
the generated `build/dev/.qt/QtDeploySupport.cmake` only ever loads
`Qt6CoreDeploySupport.cmake`, so `cpack` fails at install time with:

```
Unknown CMake command "qt6_deploy_qml_imports"
```

Finding `Qt6::Qml` explicitly — in `src/app`, and again in the top-level scope,
and after wiping the generated `.qt` directory — did not register the Qml deploy
commands either way. Qt's documentation promises only that a deployed tree is
"ready to be packaged - for example by cpack" and says nothing about CPack's
staging prefix, so this is a corner the docs leave open rather than a wrong call
on our side.

Until someone gets that working, staging Qt is one command against the built
tree:

```
windeployqt --qmldir src/app/qml --release <stage>/bin/krait-app.exe
```

**A package built without that step contains no Qt and will not start.** That is
the single most important thing on this page.

## Still missing before 1.0

These are release blockers, listed so they are visible rather than discovered.

1. **The installer still elevates.** ADR-0007 says per-user default. CPack's
   stock `NSIS.template.in` hardcodes `RequestExecutionLevel admin` and
   `SetShellVarContext all`, and no CPack variable overrides either —
   `CPACK_NSIS_INSTALL_ROOT` sets the default path and nothing more. The fix is
   to vendor CMake's `Modules/Internal/CPack/NSIS.template.in` into this
   directory, change `admin` → `user` and both `SetShellVarContext all` →
   `current`, and `list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/packaging")`.
   Registry writes already go through `SHCTX`, so they follow to HKCU on their
   own. Until that lands, `Scope: user` in the winget manifest is a claim the
   installer does not honour.

2. **Licence TEXTS are not bundled.** `THIRD-PARTY.md` lists what Krait ships
   and under what licence; the LGPL wants the actual text of the LGPL alongside
   the Qt and libssh DLLs. The release job has to copy them out of the Qt
   install and the vcpkg tree, whose paths move between versions — which is why
   this is a release-job step and not a hardcoded `install(FILES)`.

3. **No code-signing certificate.** Unsigned installers draw a SmartScreen
   warning, and ADR-0007 makes acquiring a certificate a 1.0 precondition.
   The release checklist's "SmartScreen check on a clean VM" cannot run until
   there is something signed to check.

4. **The winget manifests are templates.** `PackageVersion`, `InstallerUrl` and
   `InstallerSha256` read `0.0.0` and zeros. They are substituted by the release
   job from the CMake project version and the published asset; submitting them
   as they stand would fail validation, which is the intended failure mode.

5. **The package has never been launched.** ADR-0007's release checklist calls
   for installing into a temp location and opening one local shell and one SSH
   session. Nothing in an unattended session can do that.

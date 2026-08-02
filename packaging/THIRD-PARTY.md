# Third-party software in Krait

Krait is MIT-licensed (see `LICENSE`). It ships with the components below.

## Dynamically linked — LGPL

These are linked dynamically and shipped as separate DLLs, which is the
condition Krait relies on to remain MIT-licensed. **Do not statically link
either of them.** ADR-0001 and ADR-0002 both exist to record that.

| Component | Licence | Upstream |
|---|---|---|
| Qt 6 | LGPL-3.0 | <https://www.qt.io/> — source: <https://download.qt.io/> |
| libssh | LGPL-2.1 | <https://www.libssh.org/> |

The LGPL also requires that a user be able to replace these libraries with
their own build. They are ordinary DLLs beside `krait-app.exe`, so replacing
one is a file copy — nothing is signed in a way that prevents it, and no
integrity check rejects a substituted DLL.

## Statically linked — permissive

| Component | Licence | Upstream |
|---|---|---|
| FreeType | FTL or GPL-2.0 (Krait uses FTL) | <https://freetype.org/> |
| HarfBuzz | MIT | <https://harfbuzz.github.io/> |
| utf8proc | MIT | <https://juliastrings.github.io/utf8proc/> |
| toml++ | MIT | <https://marzer.github.io/tomlplusplus/> |
| {fmt} | MIT | <https://fmt.dev/> |

## Bundled binaries

| Component | Licence | Upstream |
|---|---|---|
| OpenConsole / conpty.dll | MIT | <https://github.com/microsoft/terminal> |

Its licence text ships beside it, in `openconsole/LICENSE`. ADR-0011 records
why Krait bundles its own rather than using the inbox conhost.

## Not shipped

Catch2 (BSL-1.0) is a test dependency and is not part of any package.

---

**Release-blocking gap.** This file LISTS the licences; a compliant package must
also carry their full TEXTS. The release job is what gathers them — see the
"Still missing" section of `packaging/README.md`. Shipping this file alone does
not discharge the LGPL obligation.

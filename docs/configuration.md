# Configuration

Krait keeps its settings in one TOML file you are meant to read and edit by
hand. Every setting is written out, including the ones still at their default,
so the file doubles as the list of what you can change.

## Where the file lives

Resolved at startup, highest priority first:

| # | Source | Location | When |
|---|---|---|---|
| 1 | `KRAIT_CONFIG_DIR` | whatever you set it to | the variable is set and non-empty |
| 2 | Portable | beside `krait-app.exe` | a file named `krait.portable` sits next to the executable |
| 3 | User profile | `%APPDATA%\Krait` | otherwise |

Krait logs which one won on startup:

```
settings: C:/Users/you/AppData/Roaming/Krait/krait.toml (user profile)
```

If Krait is not reading the file you think it is, that line is the answer.

**`KRAIT_CONFIG_DIR` is honoured even if the directory does not exist yet** — it
is created. Falling back to the user profile would silently ignore an explicit
instruction, which is the one thing an override must never do.

**Portable mode is decided by the marker file, not by the presence of
`krait.toml`.** Create an empty `krait.portable` next to the executable and the
config travels with the directory — put the whole thing on a USB stick and your
settings come along. The marker is deliberate: if the mere presence of a config
file switched modes, an installed copy in a writable directory would become
portable the first time it saved and quietly orphan your real settings.

`%APPDATA%` rather than `%LOCALAPPDATA%`: settings roam with a domain profile,
which is what you want of settings and not of a cache.

## The file

```toml
schema_version = 1

[font]
family = ""          # empty: the first installed of Cascadia Mono, Cascadia
                     # Code, Consolas, Lucida Console, Courier New
size = 20            # 6-200, in logical pixels; scaled by your display's DPI
ligatures = false

[theme]
name = "default-dark"

[unicode]
eastAsianAmbiguous = "narrow"   # or "wide"

[scrollback]
lines = 10000        # 0 turns scrollback off

[gpu]
adapter = "auto"     # or "hardware", "warp"

[notify]
longCommand = true        # notify when a slow command finishes
longCommandSeconds = 30   # 1-3600, how slow counts as slow

[ui]
language = "system"  # or "en", "th"
```

### `notify.longCommand`

When a command marked by OSC 133 shell integration takes longer than
`notify.longCommandSeconds` and finishes while Krait is **not** the focused
window, the taskbar button flashes and the tab shows a banner with how long it
took and, if the command failed, its exit status.

Two conditions, both load-bearing. It needs shell integration — without OSC 133
Krait cannot tell where one command ends and the next begins, so nothing fires
and nothing is broken. And it never fires while you are looking at the window,
because telling you what is already on your screen is how a notification becomes
something people switch off.

There is no toast and no tray icon: a banner in the tab that ran the command
says which tab, which a toast does not, and Krait's own rule is that session
messages are per-tab banners rather than anything app-modal.

### `unicode.eastAsianAmbiguous`

The East-Asian-Ambiguous character class has no correct default, only a correct
answer per person. Set it to `wide` if box-drawing and line-art in your tools
land half a cell off; leave it `narrow` otherwise. Applications can also
negotiate this at runtime through mode 2027.

### `gpu.adapter`

`auto` picks the software (WARP) rasteriser inside an RDP session and hardware
otherwise — a hardware D3D11 device over RDP is emulated anyway, badly, and on
some hosts fails to create at all. Force it with `hardware` or `warp`.

The `KRAIT_GPU` environment variable overrides this setting and takes the same
three values. That is on purpose: it is the escape hatch for a machine where the
app will not start, which is precisely when you cannot edit its config.

## Importing sessions from somewhere else

Saved connections live in `sessions.toml`, in the same directory the table above
resolves to. Three importers merge into it, from the command palette:

| Action | Reads |
|---|---|
| Import sessions from PuTTY | `HKCU\Software\SimonTatham\PuTTY\Sessions` |
| Import hosts from OpenSSH config | `%USERPROFILE%\.ssh\config` |
| Import connections from mRemoteNG | `%APPDATA%\mRemoteNG\confCons.xml` |

Each reads a FIXED location, the way the PuTTY importer reads a fixed registry
key. Each merges rather than replacing, and de-duplicates ids — so running one
twice makes copies rather than silently overwriting a profile you have since
edited.

Every importer NAMES what it left behind rather than counting it: a session
whose protocol Krait does not speak, an ssh_config `Host` line that is a pattern
rather than a name, a `Match` block. "3 skipped" only sends you hunting through
the other program to work out which three.

Two things worth knowing before relying on one:

- **`Include` in an ssh_config is reported, not followed.** Resolving one means
  glob expansion against the filesystem, and a relative-path rule that differs
  between a user config and the system one. A config that keeps its hosts in an
  included directory would otherwise import as almost nothing and still look
  like it had worked, so the summary says so instead.
- **No importer brings passwords across.** mRemoteNG encrypts its with a key
  derived from a password that defaults to a published constant; importing them
  would mean decrypting a file of credentials with a key everybody has and
  writing them into a second store. Krait asks once and keeps them in the
  DPAPI-backed Windows vault.

Importing an ssh_config is about VISIBILITY, not about making connections work:
libssh already reads `~/.ssh/config` on every connect, so a Krait session
pointed at a host inherits what that file says about it either way. What
importing buys is the host appearing in the session tree and the palette, with a
folder and a safety accent of its own.

## Hot reload

Krait watches the file. Save it and the change applies — no restart. Taking a
key out restores its default rather than keeping the last value, so you can try
something and undo it by removing the line.

## What Krait does with a broken file

A config file is user input, and none of these lose your configuration:

- **A value out of range** falls back to the default. It is not clamped:
  clamping `size = 5000` to `200` would give you a size you did not ask for and
  could not tell you had not got.
- **A value of the wrong type** falls back the same way.
- **A file that does not parse** leaves every setting at its default and logs
  the parse error. Krait still starts, so you have somewhere to fix it from.
- **A file from a newer Krait** loads, but Krait will not save over it — the
  keys a newer version added would be lost. Downgrade for an afternoon without
  losing your settings.
- **Saving** goes through a write-and-rename, so a crash mid-save leaves the
  previous file intact instead of a truncated one.

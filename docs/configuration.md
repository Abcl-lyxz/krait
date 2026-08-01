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
taskbarProgress = true    # let remote programs drive the taskbar progress bar

[triggers]
enabled = true       # run each session's trigger rules over its output
allowSend = false    # let a trigger send text back to the session
logFile = ""         # empty: <config dir>/logs/triggers.log

[ui]
language = "system"  # or "en", "th"
```

### `notify.longCommand`

When a command marked by OSC 133 shell integration takes longer than
`notify.longCommandSeconds` and finishes while Krait is **not** the focused
window, Krait tells you three ways at once: the taskbar button flashes, a
desktop notification appears, and the tab shows a banner with how long it took
and, if the command failed, its exit status.

Three rather than one, because they fail in different places. The flash is for
someone still looking at the taskbar. The notification is the only one that
reaches you in another window — it is a shell balloon, which Windows 10 files in
the notification centre and Windows 11 shows and then forgets. The banner is the
one that survives being missed, and the only one that says which *tab* finished
once you come back to several of them.

Two conditions, both load-bearing. It needs shell integration — without OSC 133
Krait cannot tell where one command ends and the next begins, so nothing fires
and nothing is broken. And it never fires while you are looking at the window,
because telling you what is already on your screen is how a notification becomes
something people switch off.

Nothing here is app-modal. A dialog that stops the other tabs because one of
them finished a build is a bug, not a notification.

### `notify.taskbarProgress`

Programs on the far end can drive the Windows taskbar progress bar by emitting
OSC 9;4 — a real percentage on the taskbar button for a long build, a big copy,
a package install. Set this to `false` to decline.

It has its own switch because the sender is *remote*. Every other surface a
remote host can reach is inside the tab it owns; this one paints a piece of your
desktop, and until now there was no way to say no. Turning it off also clears a
bar that is already showing, so a host that left one stuck cannot keep it there.

Krait still parses the sequence and still never answers it — declining changes
what Krait *does*, not what it accepts, so a program that emits it unconditionally
(most do, without asking whether the terminal has a taskbar at all) is unaffected.

### `triggers.enabled`, `triggers.allowSend`, `triggers.logFile`

A trigger is a regular expression matched against what a session prints, with
one or more actions attached. They are per session, in `sessions.toml` — see
[Triggers and snippets](#triggers-and-snippets) below for the format. These
three keys are the switches that apply to all of them.

`enabled` is the master off. A profile with no triggers already costs nothing,
but the *work* triggers do is driven by bytes the far end chooses, so there has
to be one place to stop it without editing every profile to find out which rule
turned out to be expensive on a chatty host.

**`allowSend` is off, and it is the one default here that is not about taste.**
A trigger that sends text back, fired by output the remote side controls, is a
remote-triggered input primitive you have pointed at yourself — the same shape
as a terminal answerback, which Krait rate-limits for exactly this reason. Every
other action costs you a highlight or a banner; this one runs commands. So it is
opt-in per installation, on top of the limits that always apply:

- at most **one** send leaves per chunk of output, across all rules;
- each rule may send at most three times in quick succession and then once every
  two seconds — which is what stops a trigger that matches its own echo from
  looping forever, the failure everybody hits first;
- a sent string is capped at 256 bytes;
- the sent text is what *you* wrote. Captured groups are deliberately **not**
  substituted into it, so nothing the remote host chose can end up being typed
  into your shell.

`logFile` is where the `log` action writes: one tab-separated line per match,
with a timestamp and the session name. Empty means `logs/triggers.log` beside
the session logs. The matched text is stripped of control characters and
truncated before it is written — a log line that can move your cursor is not a
log line.

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

## Triggers and snippets

Both live on the session, in `sessions.toml`, as one rule per line. Use a TOML
*literal* multi-line string (`'''`) so regex backslashes need no doubling:

```toml
[[session]]
id = "prod-web"
name = "Web 1 (prod)"
backend = "ssh"
host = "web1.example"

triggers = '''
# pattern >> action[,action]...
\berror\b|\bFATAL\b >> highlight,notify
kernel: .*oops     >> highlight,log,case
Are you sure\?     >> send:yes\r
noisy but kept     >> highlight,off
'''

snippets = '''
Disk usage  >> df -h\r
Tail syslog >> tail -f /var/log/syslog\r
Who is on   >> w\r
'''
```

Because these are ordinary profile fields, they inherit: put `triggers` in
`[defaults]` or in `[folders."prod"]` and every session under it gets them,
with a session's own list overriding rather than merging.

**Trigger actions**

| Action | What it does |
|---|---|
| `highlight` | Paints the matched text wherever it is visible on screen |
| `notify` | A per-tab banner, plus a desktop notification when Krait is not focused |
| `log` | Appends the match to `triggers.logFile` |
| `send:<text>` | Sends `<text>` to the session. Needs `triggers.allowSend`; takes the rest of the line |
| `case` | Match case-sensitively. The default is insensitive |
| `off` | Keep the rule in the file, stop running it |

Escapes in `send:` and in a snippet are `\r`, `\n`, `\t` and `\\` — nothing
else, because an escape nobody can remember is one nobody uses. Most commands
want a trailing `\r`, which is what Enter sends.

The separator is `" >> "`. A trigger splits on the **last** one, so a pattern
containing it still parses; a snippet splits on the **first**, because there it
is the tail that is free-form.

**What the matching sees.** Escape sequences are stripped before matching, so a
pattern cannot be baited from inside an invisible OSC payload and `^` means the
start of a line. Matching is per chunk of output with the trailing partial line
carried over (capped at 4 KB), so a match split across two reads is found once
and only once. A single chunk is scanned up to 64 KB; a rule with several
matches on one chunk reports at most eight of them.

**Highlights are re-derived from what is on screen**, every frame, rather than
recorded where a match landed. A recorded coordinate is invalidated by the next
window resize — Krait rewraps history — and would also miss text scrolled back
into view.

Patterns are ECMAScript regular expressions, matched by the standard library's
engine rather than Qt's. That is deliberate: Qt's `QRegularExpression` is PCRE2,
which backtracks, and Qt exposes no way to bound it. The standard library
implementation stops itself once a match gets too expensive, which is what keeps
a pattern like `(a+)+$` from handing a remote host your CPU. A pattern that does
not compile is skipped, named in a banner, and does not stop the others.

**The snippet bar** is `Ctrl+Shift+S`, or "Show the snippet bar" in the command
palette. Once it is open, `1`-`9` send the first nine snippets and `Esc` closes
it. Snippets go out through the same guard a clipboard paste does: control
characters are stripped and the text is wrapped in bracketed paste when the
application asked for it.

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

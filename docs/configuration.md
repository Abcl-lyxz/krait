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

[broadcast]
confirmDangerous = true   # hold a destructive-looking line for confirmation
idleSeconds = 300         # 0-3600; pause the broadcast after this long idle

[quake]
hotkey = ""          # empty: no drop-down and no system-wide hotkey
heightPercent = 45   # 10-100, how much of the screen the drop-down covers

[editor]
command = ""         # empty: whatever this computer opens the file with

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

### `logging.pathTemplate`, `logging.format`, `logging.includeInput`

Session logging is per tab and starts from the command palette ("Start or stop
logging this session") or **Ctrl+Shift+L**. While it runs, the tab shows a red
`● Logging to …` strip naming the file. That strip is not decoration: it is
bound to the log's real state, so if the disk fills and the log stops, the strip
goes away and a banner says which file died. A log that stopped without telling
you is worse than one you never started.

**`pathTemplate`** is where the file goes, relative to the config directory.
Empty means `logs/{session}-{date}-{time}.log`, which is exactly what earlier
builds wrote — so this key existing changes nothing until you edit it.

| Placeholder | Becomes |
|---|---|
| `{session}` | the session name, or `Shell` for an unnamed local one |
| `{host}` | the profile's host, or `none` when there is not one |
| `{date}` | `YYYYMMDD` |
| `{time}` | `HHMMSS` |

Directory separators in the *template* are yours to use — `logs/{host}/{date}.log`
gives you one folder per host. The *substituted values* are not trusted: a
profile name usually came out of an importer and a host name is influenced by
the far end, so each one is reduced to `[a-z0-9-]` before it is inserted. A
session called `../../evil` becomes `evil`; `web1.prod` becomes `web1-prod`; a
name that is nothing but Thai or CJK becomes `session`. Windows device names are
caught too — a host called `con` writes to `con-log`, not to the console driver.
A placeholder Krait does not know is left in the name where you can see it, so a
typo shows up as `{hostname}.log` rather than as a silent gap.

**`format`** is one of:

| Value | What lands in the file |
|---|---|
| `raw` | output bytes exactly as they arrived — no header, no timestamps. Replays: `type` it into a terminal and the session repaints. |
| `escaped` | *(default)* timestamped, marked `<` for output and `>` for input, control bytes written as `\x1b`. Answers "what did it send, and when". |
| `text` | escape sequences removed, then timestamped and marked. The one to paste into a bug report. |

**`includeInput` is off, and like `triggers.allowSend` that default is not about
taste.** Input is the stream a password typed at an echo-off prompt travels in —
the one place logging can capture a secret the far end never sent back. `raw`
ignores this key entirely: a byte-exact stream has no lane to put a direction
marker in, so mixing input into it would produce a file that is neither
replayable nor readable.

#### What a log file can contain

Read this before you attach one to a bug report or a ticket.

A session log is a transcript. Krait does not filter it, and could not: a
transcript that dropped the interesting line would be a transcript nobody could
trust. So:

- **With `includeInput` on, everything you typed is in the file.** Including the
  password at the `sudo` prompt. That is why it is off.
- **Turning input off does not make a log safe.** A normal shell *echoes* what
  you type, so at an ordinary prompt your keystrokes are in the output stream
  anyway. Echo stops only where the far end turns it off — which is exactly the
  password prompt. Output-only buys you that case, and nothing beyond it.
- **The far end chooses what it sends.** `env`, a token inside a `git remote -v`
  URL, a key printed by a careless script, an MOTD naming internal hosts — all
  of it lands verbatim.

The protections are procedural rather than technical, and all three matter: it
is off until a person turns it on, it is per session rather than global, and the
tab says so on screen for as long as it runs.

### `broadcast.confirmDangerous`, `broadcast.idleSeconds`

Broadcast types once and sends to many sessions at a time. `Ctrl+Shift+A` opens
its strip; the strip lists **every** open session, `1`-`9` or a click pick the
targets, and `Ctrl+Enter` starts it. Only then does anything get sent, and only
a line you finish with Enter.

The interlock is the feature, not a wrapper around it, so it is worth saying
exactly what it does:

- **The targets are on screen the whole time**, on every tab — the strip is not
  only on the tab you opened it from — and each targeted tab carries a `»` in
  the tab bar. There is no way to be looking at Krait and not know which
  sessions your line is going to.
- **The line is typed in the strip, not in the terminal.** That is what puts
  the command and the target list in front of you at the same moment.
- **It survives switching tabs**, because that is the job: watching one host's
  output while typing to all of them.
- **It does not survive Krait losing the window.** Alt-tab away and it drops
  back to *ready*: your selection is kept, and one `Ctrl+Enter` starts it again.
  Coming back from somewhere else is exactly when a mode is forgotten, and
  falling back to "keystrokes reach one session" is the safe direction to fail.
- **It pauses itself after `idleSeconds`** with nothing sent, the same way and
  for the same reason — reading a log for ten minutes never leaves Krait at all.
  `0` turns the timeout off.
- **A session that is not connected is dropped, not pretended to.** If a shell
  exits mid-broadcast, it leaves the target set, the strip stops listing it, and
  a banner names it. Krait never counts a line as delivered to somewhere it went
  nowhere.

`confirmDangerous` is the one that needed a real decision. Confirming *every*
line makes broadcast useless and teaches people to press the button without
reading it, which is how a guard stops guarding anything; confirming *nothing*
is how twelve production hosts get an `rm -rf`. So the confirmation is triggered
by the **content** of the line, using the same classifier as the paste guard —
`sudo`, `rm -rf`, `mkfs`, `dd if=`, `curl … | sh` and friends. An ordinary
command never sees a banner.

What that check cannot see, said plainly: it only reads the line you typed **in
the strip**. A command recalled with the up-arrow inside the shell on the far
end is text Krait never saw, so it is not classified. Closing that would mean
parsing every host's output, which is a different feature.

Text sent by broadcast goes through the same sanitiser as a paste and a snippet:
escape sequences and control characters are stripped, and the Enter is sent
separately so bracketed paste cannot swallow it.

### `quake.hotkey`, `quake.heightPercent`

Set `quake.hotkey` and Krait becomes a drop-down terminal: no title bar, no
taskbar button, hidden until you press the combination, and gone again when you
press it a second time. Empty — the default — leaves Krait an ordinary window
and registers no system-wide hotkey at all.

Write the combination the way every other shortcut in Krait is written:
`Ctrl+Alt+` `` ` ``, `Ctrl+Shift+F12`, `Win+Space`. Letters, digits, `` ` ``,
`Space`, `Tab`, `Esc` and `F1`-`F24` are understood. A combination with no
`Ctrl`, `Alt`, `Shift` or `Win` is refused unless the key is a function key,
because a bare letter claimed system-wide takes that letter away from every
other program on the machine.

**If another program already owns the combination**, Krait says so in a banner
naming it, and shows the window instead of hiding it. That is the common real
failure, and an app that started invisible with a hotkey that does nothing would
have no way back. Windows keeps some combinations for itself: `F12` belongs to
the debugger, and most things with the Windows key belong to Windows.

**Turning quake mode on or off needs a restart**; *changing* the combination
once it is on does not — edit `quake.hotkey` and Krait re-registers immediately,
banner and all, so a combination that turns out to be taken can be replaced
without restarting the app that just told you about it. The restart is only for
the empty↔set transition: becoming a drop-down changes the window's frame and
always-on-top behaviour, which on Windows means destroying and recreating the
window, and doing that underneath running sessions is not worth it.

The window drops down on **the monitor the mouse is on**, across the top of that
screen's usable area — under a taskbar docked at the top, not behind it — and
covers `heightPercent` of it. Everything there is in the same
DPI-independent units Qt reports screens in, so the drop-down covers the same
fraction of a 200% display as of a 100% one. Hiding it hands the keyboard back
to whatever you were doing before it appeared.

### `unicode.eastAsianAmbiguous`

The East-Asian-Ambiguous character class has no correct default, only a correct
answer per person. Set it to `wide` if box-drawing and line-art in your tools
land half a cell off; leave it `narrow` otherwise. Applications can also
negotiate this at runtime through mode 2027.

### `editor.command`

Which editor the file panel's **Edit** opens a remote file in. Empty means
whatever this computer already opens that kind of file with, which is right for
most people and is the only default that can exist — there is no editor every
machine has.

A value is split the way a command line is, so quoting works and flags survive:
`code --wait`, `"C:\Program Files\Notepad++\notepad++.exe"`, `gvim -f`. The
temporary file is appended as the last argument.

**One thing the empty default will not do.** If the remote file is something
Windows would *run* rather than show — `.exe`, `.com`, `.bat`, `.cmd`, `.scr`,
`.pif`, `.msi`, `.msp`, `.msc`, `.cpl`, `.lnk`, `.url`, `.scf`, `.hta`, `.vbs`,
`.vbe`, `.js`, `.jse`, `.wsf`, `.wsh`, `.ws`, `.reg`, `.ps1`, `.psm1` — Edit
refuses and points you at this setting instead. The file name and the bytes both
came from the server, and "open it with whatever this computer associates with
it" would mean the server chose what runs here. Set `editor.command` to a text
editor and those files open in it like anything else.

That list is fixed rather than read from `%PATHEXT%`, so it does not change from
one machine to the next: installing Python puts `.py` in `PATHEXT`, and a remote
`main.py` you cannot edit on your own laptop but can edit on the build server is
worse than either answer.

**How the round trip works, and what it cannot promise.** Krait downloads the
file to a temporary folder of its own, opens it, and watches it. Every save
uploads it back to where it came from. It keeps watching until you press **Stop
watching** in the panel — which is why the panel shows a yellow line naming
every file still open, with the remote path spelled out. A file still being
watched after you think you are finished is a save going somewhere you did not
mean it to.

The save is detected by watching the file, never by waiting for the editor to
exit. Most GUI editors hand the file to a window that is already open and the
process you launched returns immediately, so "the editor exited" says nothing
about whether anything was saved. `--wait` flags are still worth passing if your
editor has one — they keep a *new* window from being reused — but nothing
depends on them.

Pressing **Stop watching** deletes the temporary copy. So does closing the tab.
If your editor still has the file open, Windows may refuse the delete; the file
is in the OS temporary folder and is cleaned up with the rest of it.

### Shell integration

The file panel's **Shell integration** button offers to install a small script
into a shell start-up file on the machine you are connected to, so that shell
tells Krait where each prompt starts, when a command begins, and what it exited
with. That is what jump-to-prompt, the long-command notification and the
exit-status marks all read.

Krait writes to someone else's machine here, so it never does it quietly:

- It **looks** for the start-up files rather than guessing which shell you use —
  `.bashrc`, `.zshrc`, `.config/fish/config.fish`, and both places a PowerShell
  `$PROFILE` lands. If more than one exists it asks which, rather than picking.
  Installing a bash script into a fish profile is a broken login shell for
  somebody.
- It **only ever edits a file it has read**. If it could not read any of them it
  says so and stops, and it will not offer to create one. "No such file" and
  "permission denied" come back from SFTP looking identical, and writing a
  brand-new file on the strength of a failed read is how a 200-line `.bashrc`
  becomes a four-line one. If your machine genuinely has no start-up file, make
  an empty one and ask again.
- It shows **which host, which path, and the exact text** that will be written,
  and nothing happens until you press **Write the change**.
- It **never clobbers**. The script goes between two marker lines:

  ```
  # >>> krait shell integration >>>
  # <<< krait shell integration <<<
  ```

  Installing again replaces exactly what is between them and returns the rest of
  the file byte for byte — including its line endings. **Uninstall** takes the
  block out and leaves the file as it was before. If the markers are there but do
  not pair up, Krait says so and changes nothing: there is no way to tell where a
  half-deleted block ends, and guessing truncates a file on a machine you are a
  guest on.
- The file is downloaded, edited here, and uploaded back over SFTP. It is not
  appended to through the shell, because an append cannot be shown to you before
  it happens and cannot see that a block is already there.

The same four scripts ship in the `shell-integration` folder beside `krait.exe`
if you would rather install them yourself — `source` the one for your shell, or
add it to the start-up file by hand.

**What each shell reports.** All four mark the prompt and the exit status.
`bash`, `zsh` and `fish` also mark the moment a command starts running, which is
what arms the long-command notification. PowerShell has no hook between Enter and
the command running that does not mean taking over PSReadLine's Enter key, so it
does not send that mark and long-command notifications do not fire for remote
PowerShell. A PowerShell *cmdlet* that fails has no exit code at all — Krait's
script reports `1` for it, the way a POSIX shell reports a failed builtin, rather
than passing on `$LASTEXITCODE`, which at that moment still holds the exit code of
some earlier native command.

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

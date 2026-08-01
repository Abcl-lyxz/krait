---
name: project-sftp-panel-patterns
description: Review checklist for src/app/sftp_model.* and assets/shell-integration — remote-name-to-local-path flows, rc-file splicing, and the shell scripts
metadata:
  type: project
---

Checklist for the file panel / shell-integration area (T65, T73 and anything
that touches `src/app/sftp_model.{h,cpp}`, `src/app/shell_integration.{h,cpp}`,
`assets/shell-integration/*`).

**Why:** this is the only place in the app where a REMOTE-chosen string becomes
a local filesystem path, a launched process, or an `O_TRUNC` write back to
someone else's machine. Three of the four defect classes found here were
"remote value used one layer past where it was validated".

**How to apply:** walk these in order before reading the diff line by line.

1. **Hash-key vs display-key.** `m_edits` is keyed by LOCAL PATH; rows and
   requests carry the leaf NAME. Any public method taking a name and looking up
   an Edit is a wrong-entry bug waiting for two same-named files in different
   remote dirs. Check every `Q_INVOKABLE`'s key against the container's key.
2. **"Open with the OS default" on remote content is execution.**
   `QDesktopServices::openUrl` / `ShellExecute` on a file whose name and
   contents the server chose runs the shell association. An affordance labelled
   Edit must launch an editor, never the association.
3. **Absent vs unreadable.** Any probe that treats "the request failed" as "the
   file is not there" turns a transient error into a create-and-truncate.
   `Sftp::put` is `O_WRONLY|O_CREAT|O_TRUNC`, so that verdict is unrecoverable.
   The backend currently gives only ok/cancelled/message — no status code — so
   the distinction has to be added, not assumed.
4. **Staged local writes before an upload.** `QFile::write` return and close
   status must be checked; the staged file is what gets uploaded over a live
   remote file. Prefer QSaveFile.
5. **Caps.** Anything that `readAll()`s a downloaded remote file needs a byte
   ceiling (rules/net.md: cap remotely-influenced allocations). `Sftp::get`
   streams to disk, so the model is the only place a cap can live.
6. **Shell scripts** (`assets/shell-integration/`): idempotent guard variable
   set by the script itself; exit status captured on the FIRST line of the hook;
   PROMPT_COMMAND prepended (only the first entry sees `$?`); markers must not
   appear inside the payload (the panel test enforces this); `\[ \]` / `%{ %}`
   around every escape or wrapped command lines are drawn in the wrong place.
   The zsh script uses `add-zsh-hook` (APPENDS) while bash prepends —
   unverified whether zsh preserves `$?` across precmd hooks; kitty appends too,
   so do not flag it without evidence.

Related: [[project-watch-items]], [[project-qml-view-patterns]].

7. **The install/probe state machine has TWO cancel doors, and only one is
   wired.** `SftpModel::cancel()` goes through `sftpCancelAll()`, so in-flight
   requests come back with `cancelled=true` and the Probe branch resets.
   `cancelShellIntegration()` and anything else that calls `resetInstall()`
   alone leave the request id OPEN in `m_open` — the reply then resumes the
   chain against a zeroed `Install`, and `QDir("").filePath(...)` is a RELATIVE
   path in the process CWD. Any new stage-driven flow needs a stage re-check at
   the TOP of its `handleFinished` branch, not just at the entry points.
8. **`m_edits` values are handed out as references far too freely.** Two call
   sites already carry a "by value, because an event loop can erase this"
   comment; check the actual signature, not the comment. `QDesktopServices::
   openUrl`, `QProcess::startDetached` and any `emit` reaching QML can all
   deliver a queued `sftpFinished` that erases the entry.

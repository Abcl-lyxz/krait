---
name: conpty-thread-patterns
description: Recurring defect patterns in src/net threaded backends — close-before-join, start() failure leaks, un-armed prompt handshakes, unbounded joins, secret slots, upstream platform guards
metadata:
  type: project
---

T15 ConptyBackend review (2026-07-29) found two recurring pattern risks for ALL future backends (ssh/telnet/serial):

1. **close-before-join**: stop() closed pipe/process handles before joining reader/writer threads → use of closed/recycled HANDLE from worker thread. Correct order: unblock (close pty / CancelIoEx / CancelSynchronousIo) → join → close handles. Re-check in every IBackend impl.
2. **start() failure leaks**: `m_started` gate on stop() means every mid-start() failure path leaks (pipes, HPCON, attr list, HMODULE), and retry loops (ensureStarted on geometryChange) compound it — LoadLibrary/CreatePipe overwrite members. Demand a teardown that runs regardless of m_started.
3. Writer thread blocked in synchronous WriteFile ignores CV shutdown flag — needs CancelSynchronousIo or handle-close unblock path.
4. stop() waits (3s x2) run on GUI thread via destructor — net.md says no UI-thread blocking; watch when tab close lands in M1.
5. CreateProcessW with lpApplicationName=nullptr + bare "powershell.exe" → binary planting via search path. Use full System32 path.

T39-T43 SshBackend added three more (2026-07-31), all generalisable:

6. **Prompt-handshake flags are never re-armed.** A CV handshake that crosses to
   the GUI (host key, credential) needs the "answered" slot CLEARED before the
   emit, on EVERY prompt site — not just the one the author was thinking about.
   A reconnect loop replays the same object, so a leftover `answered=true` makes
   the next security prompt resolve without a human. Grep for every `emit
   *Prompt` and check each has its arm/clear.
7. **stop() joins a worker parked in a third-party blocking call.** A shutdown
   atomic + notify_all only releases OUR waits; libssh/WinSock/agent-pipe calls
   ignore it, so the join (and the UI thread) is unbounded. Demand either a
   socket-level cancel or a bounded join with a detach fallback.
8. **Reconnect attempt counters must reset on success**, or "5 attempts" quietly
   becomes "5 drops ever".
9. **Secrets held in a member slot need clearing on the cancel/timeout path**,
   not only on the consume path — an answer arriving after the wait gave up
   parks plaintext for the object's lifetime.
10. Shared secret stores (Vault) are touched from worker threads: check for a
    mutex before the first backend calls retrieve/store/save off the GUI thread.

T52 backend swap added one more (2026-07-31), and it is the sharpest:

11. **Know WHERE a backend's queued emission is posted before trusting a
    swap/teardown.** ConptyBackend does `QMetaObject::invokeMethod(this, ...,
    QueuedConnection)` — the event goes to the BACKEND, so a later
    `backend->disconnect(receiver)` really does drop it. SshBackend does a plain
    `emit outputReceived(...)` on the worker — AutoConnection posts the
    QMetaCallEvent to the RECEIVER, and disconnect() cancels nothing already in
    the queue. Any "stop(); disconnect(); deleteLater()" swap therefore needs an
    identity guard in the slot (`if (sender() != m_backend) return;` or capture
    the backend pointer in the lambda), not just a disconnect. Cross-session
    consequences: old host's bytes parsed into the new grid, DA/DSR answerbacks
    written to the NEW connection, host A's credential prompt answered with a
    password that is then sent to host B.
12. **Two signals emitted back-to-back from a worker = the second banner wins.**
    verifyHostKey emits hostKeyPrompt(Changed) then fail(); both queue to the
    GUI in order, so the generic error banner overwrites the danger banner in
    the same turn. Check every prompt-then-fail pair against what the user
    actually ends up looking at.

**How to apply:** checklist for any src/net diff with std::thread + Win32 handles, and for any src/app code that tears down or replaces a backend. See [[project-watch-items]].

T54 telnet added the inverse of 11/12 (2026-07-31):

13. **`TerminalItem::resetSession()` calls `old->stop()` on a QThreadPool
    thread — unconditionally, for every backend.** That was designed for
    SshBackend (blocking libssh join) and ConptyBackend (Win32 handles), both
    of which are thread-agnostic. Any NEW backend built on Qt objects
    (QTcpSocket, QTimer, QSerialPort, QLocalSocket) has a `stop()` that is
    illegal off the owning thread: `QTimer::stop()` → killTimer prints
    "Timers cannot be stopped from another thread" and does NOTHING (the
    pending reconnect survives), and `QAbstractSocket::abort()` races the GUI
    thread's read notifier. Check this on T55 (raw) and T56 (serial) too.
    Fix shape: guard at the top of stop() —
    `if (thread() != QThread::currentThread()) { QMetaObject::invokeMethod(this, &X::stop, Qt::QueuedConnection); return; }`
    Do NOT use BlockingQueuedConnection: ~QThreadPool waits on the GUI thread
    at shutdown, so a blocking hop deadlocks.

T60 agent bridge added the Win32 half of item 7 (2026-07-31):

14. **`CancelIoEx` is not a cancel *token*, it is a one-shot sweep.** It only
    marks I/O that is outstanding at the instant it is called. Any thread that
    can still *issue* a fresh blocking `ReadFile`/`WriteFile` after the sweep is
    uncancellable, so the following `join()` is unbounded no matter what the
    comment above it claims. A correct Win32 cancel needs BOTH an atomic flag
    the pumping thread checks before each I/O call AND either a re-issued cancel
    on a bounded loop or `FILE_FLAG_OVERLAPPED` + `WaitForMultipleObjects` on
    {io event, stop event}. Same trap as item 3's `CancelSynchronousIo`.
15. **Every NEW blocking call added to the SSH auth ladder must have a cancel
    path wired into `SshBackend::stop()`.** `m_shutdown` + `notify_all` only
    releases OUR condition variables; the ladder is only checked BETWEEN rungs.
    T60 put an agent round-trip (a FIDO/smartcard touch = human-scale latency)
    inside a rung with nothing to cancel it. Check this on every new rung.

T61/T62 additions (2026-07-31):

16. **A start/cancel/stop trio is only correct if `start()` joins the mutex
    too.** Reviewing cancel() and stop() in isolation misses the lost-cancel
    window: if start() publishes handles unlocked, a cancel() that arrives
    before the publish sees stale zeros and does nothing — and if start() also
    clears the stop flag, the cancel is erased outright. Check start() first,
    then the cancel path. (`AgentBridge::start`, T60.)
17. **libssh path options vs. libssh pki file calls are NOT interchangeable.**
    `ssh_options_set(SSH_OPTIONS_IDENTITY/CERTIFICATE)` gets `~`/`%d` expansion
    via `ssh_options_apply`; `ssh_pki_import_privkey_file` /
    `ssh_pki_import_cert_file` do not. Whenever a diff adds an importer that
    writes config paths into `Profile`, check which of the two consumes them.

T64 SFTP-on-the-shell-worker added the starvation half (2026-08-01):

18. **Anything new that runs to completion inside `pump()`'s loop starves
    EVERYTHING else pump() does.** The T64 SFTP request is serviced at the end
    of one pump iteration but can occupy the session for minutes. Its
    `interleaveShell()` hook was written to keep the shell alive and only reads
    `is_stderr=0` and drains `m_writeQueue` — it does NOT read stderr (which
    pump() reads at :905 precisely because an unread stream fills the channel
    window), does NOT call `m_forwards.service()` (which accepts AND pumps
    tunnel bytes), and does NOT drain `m_resizePending`. Rule: when a diff adds
    a long-running step to a poll loop, diff its interleave hook against the
    loop body line by line — everything the loop does per iteration has to be in
    the hook or it is starved for the duration.
19. **A blocking libssh loop with a COUNT cap still needs a cancel path.**
    `Sftp::listDir` caps iterations at 65536, which bounds memory but not TIME:
    each iteration is a round trip bounded only by `SSH_OPTIONS_TIMEOUT`. A cap
    is not a cancel. Every new blocking loop in src/net needs an `m_shutdown`
    read, not just a bound (rules/net.md).

T89 src/net security audit (2026-08-02) — the ones that repeated:

20. **Item 1 (close-before-join) regressed in `SerialBackend`.** ConptyBackend
    fixed it in T15 and documented the order in its own stop(); serial's stop()
    calls `closePort()` (CloseHandle) BEFORE joining, and `m_handle` is a plain
    non-atomic `void*`. When a checklist item is fixed in one backend, grep the
    other four for the same shape — the fix does not travel by itself.
21. **A queued lambda posted from a worker can outlive `stop()` and re-create
    what stop() just tore down.** `SerialBackend::readerLoop` posts a lambda
    that joins AND re-assigns `m_reconnect`; nothing in it checks `m_shutdown`,
    and stop() is idempotence-gated so the second stop() never joins the thread
    that lambda spawned => `~std::thread` on a joinable thread => terminate.
    Rule: any queued lambda that starts a thread or opens a handle needs the
    same shutdown check its worker has.
22. **Verify third-party platform guards, not just struct layouts.** ADR-0012
    verified `ssh_jump_callbacks_struct`'s fields and shipped ProxyJump — but
    libssh's whole ProxyJump connect path is `#ifndef _WIN32 / #ifdef
    HAVE_PTHREAD` (client.c), so on the ONLY supported platform the option is
    accepted, stored, and silently ignored, and the connection goes direct.
    When a capability rests on an upstream feature, grep the upstream source
    for `_WIN32` around the code that *uses* the option, not just the header
    that declares it.
23. **Unchecked `ssh_options_set` on a security option is fail-open.**
    `connectSession()` discards every return. For PROXYJUMP and the algorithm
    lists, "rejected" means the libssh DEFAULT stays in force — a policy that
    silently did not apply. Flag any discarded return on a security setter.

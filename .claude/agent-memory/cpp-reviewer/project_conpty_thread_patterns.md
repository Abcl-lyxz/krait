---
name: conpty-thread-patterns
description: Recurring defect patterns in src/net threaded backends — close-before-join, start() failure leaks, un-armed prompt handshakes, unbounded joins, secret slots
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

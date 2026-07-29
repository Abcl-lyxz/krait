---
name: conpty-thread-patterns
description: T15 review — recurring defect patterns in src/net threaded backends (handle close-before-join, start() failure leaks)
metadata:
  type: project
---

T15 ConptyBackend review (2026-07-29) found two recurring pattern risks for ALL future backends (ssh/telnet/serial):

1. **close-before-join**: stop() closed pipe/process handles before joining reader/writer threads → use of closed/recycled HANDLE from worker thread. Correct order: unblock (close pty / CancelIoEx / CancelSynchronousIo) → join → close handles. Re-check in every IBackend impl.
2. **start() failure leaks**: `m_started` gate on stop() means every mid-start() failure path leaks (pipes, HPCON, attr list, HMODULE), and retry loops (ensureStarted on geometryChange) compound it — LoadLibrary/CreatePipe overwrite members. Demand a teardown that runs regardless of m_started.
3. Writer thread blocked in synchronous WriteFile ignores CV shutdown flag — needs CancelSynchronousIo or handle-close unblock path.
4. stop() waits (3s x2) run on GUI thread via destructor — net.md says no UI-thread blocking; watch when tab close lands in M1.
5. CreateProcessW with lpApplicationName=nullptr + bare "powershell.exe" → binary planting via search path. Use full System32 path.

**How to apply:** checklist for any src/net diff with std::thread + Win32 handles. See [[project-watch-items]].

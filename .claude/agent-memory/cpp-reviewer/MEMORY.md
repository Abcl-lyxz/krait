# cpp-reviewer memory index

- [Watch items](project_watch_items.md) — latent issues accepted in past reviews (T3 skeleton); re-check when src/core deps, root flags, or T11 change
- [Project review patterns](project_review_patterns.md) — VT-core diff checklist: interrupted-input corpus variants, subparam-flag rejection, Params invariants
- [ConPTY/thread patterns](project_conpty_thread_patterns.md) — src/net backend checklist: close-before-join, start() failure leaks, writer unblock, UI-thread stop()
- [CI + batch-gate patterns](project_ci_batch_patterns.md) — green-but-blind gate holes in ci.yml, pwsh mid-script swallowing, verified cmd.exe/vcvars facts
- [Render/Qt patterns](project_render_qt_patterns.md) — QRhi + QQuickRhiItem checklist: geometry-less lifecycle hooks, duplicated buffer-size math, consumed dirty flags, verified Qt 6.10 facts

# cpp-reviewer memory index

- [Watch items](project_watch_items.md) — per-task latent issues + verified-correct facts from every review through T84; read before re-flagging anything
- [Project review patterns](project_review_patterns.md) — VT-core diff checklist: interrupted-input corpus variants, subparam-flag rejection, Params invariants
- [ConPTY/thread patterns](project_conpty_thread_patterns.md) — src/net backend checklist: close-before-join, start() failure leaks, writer unblock, UI-thread stop()
- [CI + batch-gate patterns](project_ci_batch_patterns.md) — green-but-blind gate holes in ci.yml, pwsh mid-script swallowing, verified cmd.exe/vcvars facts
- [Render/Qt patterns](project_render_qt_patterns.md) — QRhi + QQuickRhiItem checklist: geometry-less lifecycle hooks, duplicated buffer-size math, consumed dirty flags, verified Qt 6.10 facts
- [QML view patterns](project_qml_view_patterns.md) — src/app/qml checklist: stale itemAt() bindings, banner target routing, index/weight clamp arithmetic
- [SFTP panel patterns](project_sftp_panel_patterns.md) — sftp_model + shell-integration checklist: remote name → local path/process, absent-vs-unreadable, O_TRUNC writes, shell-script rules
- [Static service injection](project_static_service_injection.md) — the g_registry/g_vault/g_store pattern: why it is safe, and the two things that would break it

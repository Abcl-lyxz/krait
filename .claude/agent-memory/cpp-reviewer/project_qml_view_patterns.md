---
name: project-qml-view-patterns
description: QML review checklist for src/app/qml — stale itemAt() bindings, banner target routing, ListModel index/weight arithmetic; from the T53 tabs+splits review
metadata:
  type: project
---

# QML review checklist (src/app/qml)

Established during the T53 tabs/splits review. Check these before anything else
in a QML diff.

**Why:** QML has no compiler and no test harness in this repo (Catch2 cannot
reach it), so binding-reactivity bugs ship silently. Every finding below was
invisible to the build and to the KRAIT_UI_SELFTEST run.

**How to apply:** run this list against any diff touching `src/app/qml/`.

1. **`repeater.itemAt(i)` in a binding is not reactive.** Invokable method
   calls register no dependency. A `property Item foo: rep.itemAt(currentIndex)`
   only re-runs when `currentIndex` changes — and remove-handlers that do
   `idx = Math.min(idx, count - 1)` frequently leave `idx` unchanged. Result:
   the property points at a destroyed delegate, its own notify never fires, and
   downstream bindings (tab title, safety accent) freeze at the dead value.
   Fix pattern: add a real dependency, `(model.count, rep.itemAt(idx))`.
2. **`Math.min(current, count - 1)` after a middle removal is wrong.** It only
   handles removal at/after `current`; removing an item BEFORE it shifts the
   survivors and silently changes which one you are looking at. Needs
   `if (removed < current) current--`.
3. **One banner, N prompt sources.** Every `banner.target = x` site must be
   paired with a clear on the path that destroys `x`. Check `closePane`/
   `closeTab` for a missing dismiss. Also check whether a second prompt can
   steal the banner from a first that is still pending — the first prompt is
   then dropped with no record and its backend waits for an answer forever.
   `Banner.beginInput()` clearing `input.text` is the ONLY thing preventing
   actual credential misdelivery; do not let that clear be removed.
4. **`ListModel.get(i).prop` inside a binding DOES re-run on `setProperty()`**
   (cached model-object notify signals), but only for elements already
   materialised by a prior `get()`, and it is an implementation detail, not a
   documented guarantee. Not verifiable here: `qt-docs` MCP is unavailable to
   subagents and WebFetch is disabled — say so rather than asserting.
5. **Clamp arithmetic on shares/weights.** `Math.max(lo, Math.min(hi, v))`
   inverts when `hi < lo` and silently produces out-of-range results. Any
   layout that divides by a computed span needs an early-out when the span is
   below twice the minimum.
6. **Delegate `required property int index` IS re-indexed synchronously** by
   QQmlDelegateModel before `model.remove()` returns, so `onXxx: f(d.index)`
   handlers are safe. Do not flag those; flag the surrounding arithmetic.
7. **Repeater delegates are created SYNCHRONOUSLY** when the model changes at
   runtime (`AsynchronousIfNested`, and nothing is incubating post-startup).
   `Qt.callLater` to "wait for the delegate" is unnecessary AND harmful: the
   callback re-derives the target from current state, so a queued keystroke
   between the two turns retargets the action to the wrong tab/pane.

See [[project-watch-items]] for the per-task findings and
[[project-render-qt-patterns]] for the QRhi side.

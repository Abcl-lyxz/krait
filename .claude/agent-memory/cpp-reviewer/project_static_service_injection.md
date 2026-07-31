---
name: project-static-service-injection
description: Why src/app's file-scope g_registry/g_vault/g_store pointers into main()'s stack are safe, and the two changes that would make them dangle — verified T53
metadata:
  type: project
---

# Static service injection in src/app (verified T53, 2026-07-31)

`TerminalItem::setServices(Registry*, Vault*, ProfileStore*)` and
`SessionModel::setStore(ProfileStore*)` stash file-scope pointers to objects
that are LOCALS IN `main()`. QML then constructs terminals on demand (a new tab
creates one), and each copies the pointers in its constructor.

**Why this is safe:** `main()` declares `registry`, then `vault`, then `store`,
then `QQmlApplicationEngine engine` — in that order. Locals destruct in reverse,
so every QML-owned `TerminalItem`/`SessionModel` dies with the engine before the
three services. The `setServices`/`setStore` calls sit BEFORE the engine is even
constructed, so no item can be built with null services. There is exactly one
engine and nothing runs after `app.exec()` returns.

**How to apply — flag immediately if a diff:**
- declares any of the three services AFTER the `QQmlApplicationEngine` (the
  ordering is load-bearing and only the `vault` declaration says so in a
  comment; the others do not),
- adds a second `QQmlApplicationEngine`, or any object outliving the first
  engine that reads these,
- constructs a `TerminalItem`/`SessionModel` anywhere before the setters,
- adds a `SessionModel` to a test — `tests/unit/CMakeLists.txt` compiles
  `session_model.cpp` straight into `krait-qt-tests` and never calls
  `setStore()`, so `m_store` is null there. `load()` guards it; `refresh()`,
  `profileById()`, `profileByName()` and `importFromPutty()` do not.

Related: `g_launchProfile` (same file) is CLAIMED by the first-constructed
`TerminalItem`. `SpikeGrid` is a separate C++ element
(`src/render/spike/grid_item.h`), not a terminal, so it cannot steal the claim —
re-check that if another QML element ever embeds a `TerminalView`.

See [[project-qml-view-patterns]], [[project-watch-items]].

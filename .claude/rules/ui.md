---
paths:
  - "src/app/**"
  - "**/*.qml"
---

# App/UI rules (src/app/ + QML)

- **The settings registry is the only path for settings.** Schema (id, type,
  default, doc key, search keywords EN+TH, migration) → TOML IO → QML
  settings UI → command palette — all generated from one declaration.
  Adding a bare QSettings/TOML key by hand is a defect; run `/add-setting`.
- QML is views only: no business logic, no network, no file IO in QML.
  View-models are C++ `QObject`s with typed properties; QML binds.
- **Every user-facing string** goes through `tr()` with a translator comment
  when ambiguous. English + Thai ship together — a string landing without
  both locales is incomplete work.
- **Keyboard-first:** every action is a registered Action (id, default
  shortcut, palette entry, doc key). A feature reachable only by mouse is
  incomplete work.
- Errors are per-tab banners with error-taxonomy codes from the backend
  layer. `QMessageBox` and any app-modal surface are banned in session flows.
- Paste-guard, safety accents (prod = red), and host-pattern profile
  switching are core UX invariants — never behind "advanced" toggles, never
  removed to simplify a test.
- Blocking work never runs on the UI thread; QML never waits on a promise
  longer than a frame — use busy states.
- Theme tokens (colors, spacing, radii) come from the theme system; hex
  literals in QML are a defect.

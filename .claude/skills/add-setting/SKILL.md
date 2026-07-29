---
name: add-setting
description: Add a user-facing setting end-to-end through the settings registry — schema, default, TOML IO, settings UI page, search keywords (EN+TH), docs, migration. Use whenever adding, renaming, or changing the type of any user-visible option.
allowed-tools: "Read, Grep, Glob, Edit, Write, Bash"
---

"Configurable" is the product promise; the settings registry is how we keep
hundreds of settings coherent (rules/ui.md). A setting added by hand in TOML
or QSettings is a defect. Work the checklist:

- [ ] **1. One declaration.** Add the setting to the settings registry
  (single source of truth): stable id (`section.name`, snake_case), type +
  constraints (range/enum), **default with a one-line rationale comment**,
  scope (global / per-profile / both — per-profile wins on conflict),
  hot-reload behavior (live | needs-reconnect | needs-restart — prefer live;
  justify anything else).

- [ ] **2. Searchable.** Doc key with tooltip text, and search keywords in
  BOTH English and Thai (users type "ฟอนต์" and expect the font settings).
  Settings search is a flagship feature — thin keywords are incomplete work.

- [ ] **3. Surfaces.** The registry generates: TOML read/write, the settings
  UI row (correct widget for the type, reset-to-default, "copy as TOML"),
  and the command palette entry. Verify each surface actually appears; if a
  new widget type is needed, build it once in the settings framework, never
  inline.

- [ ] **4. Migration.** Renamed or retyped? Add a migration step (old key →
  new key) and a test proving an old config file loads. Removed? Add a
  tombstone so old files never error.

- [ ] **5. Consume it properly.** Code reads the typed accessor, never raw
  TOML. Hot-reload: consumers observe changes; test flip-at-runtime if live.

- [ ] **6. Prove it.** Unit test: default value, TOML round-trip, constraint
  clamping, migration (if any). Then `/preflight`.

Anti-scope guard: if the "setting" is really a per-profile override of an
existing global, wire the override — do not mint a second id.

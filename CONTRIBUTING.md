# CONTRIBUTING — the non-negotiables

These override convenience, deadlines, and "just this once". A test that can
only pass by weakening one of these must fail.

1. **No telemetry. Ever.** Not opt-out, not "anonymous", not crash-only by
   default. Crash reporting may exist only as explicit per-incident opt-in.
2. **Secrets live in the DPAPI vault only.** Never in TOML, logs, clipboard
   history, or debug output. Redaction is mandatory in every log path.
3. **The VT core stays dependency-free.** `src/core/` must build standalone
   with no Qt, no network, no rendering includes. There is a CI target that
   proves it.
4. **Never weaken a denial to make something pass.** Security checks, the
   Bash guard hook, permission rules, and host-key verification are not
   adjustable test fixtures.
5. **Honest capability replies.** DA/DECRQM/XTGETTCAP answers reflect what is
   actually implemented. Lying to TUIs breaks feature detection ecosystems.
6. **Untrusted input is fuzzed.** New parser states and new network message
   handling ship with fuzz corpus seeds in the same change.
7. **i18n is not a layer.** Every user-facing string goes through `tr()`;
   Thai and English ship together; IME regressions block release.
8. **Perf budgets are release gates.** The budgets in `.claude/rules/render.md`
   (and bench baselines in `bench/`) fail the build when regressed >5%.
9. **Errors are per-tab.** No global-modal dialogs, no exceptions across
   module boundaries.
10. **ADRs are append-only.** Supersede with a new numbered ADR; never edit a
    settled one. Re-opening a settled question without new evidence is noise.

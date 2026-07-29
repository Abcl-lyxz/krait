#!/usr/bin/env node
// Stop hook: "verified beats claimed". If source changed since the last
// successful test run, block the stop ONCE with a reminder to run /preflight.
// Snoozes after one block so it can never loop or nag repeatedly.
// Inactive until the project is scaffolded (no CMakeLists.txt -> no gate).
// Node built-ins only. Must never throw; on any error, exit 0 (allow stop).
'use strict';
const fs = require('fs');
const path = require('path');

function safe(fn, fallback) { try { return fn(); } catch { return fallback; } }
function mtime(p) { return safe(() => fs.statSync(p).mtimeMs, 0); }

function main() {
  const input = safe(() => JSON.parse(fs.readFileSync(0, 'utf8')), {});
  if (input.stop_hook_active) { return; } // never loop

  const proj = process.env.CLAUDE_PROJECT_DIR || input.cwd || process.cwd();
  if (!fs.existsSync(path.join(proj, 'CMakeLists.txt'))) { return; } // pre-M0 phase

  const cache = path.join(proj, '.claude', '.cache');
  const dirty = mtime(path.join(cache, 'src-dirty'));
  if (!dirty) { return; }
  const pass = mtime(path.join(cache, 'last-test-pass'));
  if (pass >= dirty) { return; } // tests ran after the last source change

  const snooze = path.join(cache, 'stop-gate-snooze');
  if (mtime(snooze) >= dirty) { return; } // already reminded for this change set
  safe(() => { fs.mkdirSync(cache, { recursive: true }); fs.writeFileSync(snooze, String(Date.now())); });

  const reason =
    'Source files changed but no successful test run followed (Stop gate). ' +
    'Run /preflight (or `ctest --preset dev`) and report the result — or state ' +
    'explicitly why tests do not apply to this change. This reminder fires once.';
  process.stdout.write(JSON.stringify({
    decision: 'block',
    reason,
    hookSpecificOutput: { hookEventName: 'Stop', decision: 'block', reason },
  }));
}

safe(main);
process.exit(0);

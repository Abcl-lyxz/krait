#!/usr/bin/env node
// SessionStart hook: inject phase, branch, and build/test freshness.
// Node built-ins only. Must never throw; on any error, exit 0 silently.
'use strict';
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

function safe(fn, fallback) { try { return fn(); } catch { return fallback; } }

function main() {
  const input = safe(() => JSON.parse(fs.readFileSync(0, 'utf8')), {});
  const proj = process.env.CLAUDE_PROJECT_DIR || input.cwd || process.cwd();
  const lines = [];

  // STATE.md — the handoff contract
  const statePath = path.join(proj, 'STATE.md');
  const state = safe(() => fs.readFileSync(statePath, 'utf8'), null);
  if (state) {
    const head = state.split('\n').slice(0, 80).join('\n').trim();
    lines.push('=== STATE.md (session handoff — trust this, do not re-derive) ===');
    lines.push(head);
  } else {
    lines.push('WARNING: STATE.md is missing. Recreate it via /handoff before doing anything else.');
  }

  // Git snapshot
  const git = (args) => safe(() => {
    const r = spawnSync('git', args, { cwd: proj, encoding: 'utf8', timeout: 3000 });
    return r.status === 0 ? r.stdout.trim() : null;
  }, null);
  const branch = git(['rev-parse', '--abbrev-ref', 'HEAD']);
  if (branch) {
    const dirty = (git(['status', '--porcelain']) || '').split('\n').filter(Boolean).length;
    const last = git(['log', '-1', '--format=%h %s']) || 'no commits yet';
    lines.push('');
    lines.push(`=== Git === branch: ${branch} | uncommitted files: ${dirty} | last: ${last}`);
  } else {
    lines.push('');
    lines.push('=== Git === not a git repository yet (SETUP.md step 3).');
  }

  // Build/test freshness (markers maintained by post-tool hook)
  const cache = path.join(proj, '.claude', '.cache');
  const age = (f) => safe(() => {
    const ms = Date.now() - fs.statSync(path.join(cache, f)).mtimeMs;
    const m = Math.round(ms / 60000);
    return m < 120 ? `${m} min ago` : `${Math.round(m / 60)} h ago`;
  }, 'never this machine');
  if (fs.existsSync(path.join(proj, 'CMakeLists.txt'))) {
    lines.push(`=== Freshness === last successful test run: ${age('last-test-pass')} | last source edit: ${age('src-dirty')}`);
  }

  lines.push('');
  lines.push('Reminders: MCP-first (qt-docs/context7/clangd-lsp — never code against a guessed API). ' +
    'Verify with /preflight before claiming anything works. End the session with /handoff.');

  process.stdout.write(lines.join('\n'));
}

safe(main);
process.exit(0);

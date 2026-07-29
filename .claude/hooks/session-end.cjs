#!/usr/bin/env node
// SessionEnd hook: append one line to the local session journal.
// Useful for "what happened on this machine last week" archaeology.
// Node built-ins only. Must never throw; exit 0 always.
'use strict';
const fs = require('fs');
const path = require('path');

function safe(fn, fallback) { try { return fn(); } catch { return fallback; } }

function main() {
  const input = safe(() => JSON.parse(fs.readFileSync(0, 'utf8')), {});
  const proj = process.env.CLAUDE_PROJECT_DIR || input.cwd || process.cwd();
  const cache = path.join(proj, '.claude', '.cache');
  fs.mkdirSync(cache, { recursive: true });
  const line = `${new Date().toISOString()} session=${input.session_id || '?'} reason=${input.reason || '?'}\n`;
  fs.appendFileSync(path.join(cache, 'journal.log'), line);
}

safe(main);
process.exit(0);

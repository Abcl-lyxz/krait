#!/usr/bin/env node
// PreToolUse(Bash) guard: safety denials + MCP-first nudges.
// Policy lives in .claude/rules/mcp-first.md; this hook is the enforcement.
// Node built-ins only. Must never throw; on any error, exit 0 (allow).
'use strict';
const fs = require('fs');

function safe(fn, fallback) { try { return fn(); } catch { return fallback; } }

function decide(cmd) {
  const c = ` ${cmd.replace(/\s+/g, ' ').trim()} `;

  // --- Hard denials ---------------------------------------------------------
  if (/\bgit push\b(?![^|;&]*--force-with-lease)[^|;&]*(\s--force\b|\s-f\b)/.test(c)) {
    return { d: 'deny', r: 'Force push is banned. Use --force-with-lease, and only on your own branch — never main.' };
  }
  if (/\bgit (commit|push|merge)\b[^|;&]*--no-verify/.test(c)) {
    return { d: 'deny', r: 'Bypassing hooks (--no-verify) is banned. Fix what the hook found instead.' };
  }
  if (/\brm\s+-[a-zA-Z]*[rf][a-zA-Z]*\s+(\/|~|[A-Za-z]:[\\/]?\s|\.\s*$|\*\s*$)/.test(c)) {
    return { d: 'deny', r: 'Refusing rm -rf on a root-like or bare-wildcard path.' };
  }

  // --- MCP-first: repo searching belongs to tools, not raw shell -----------
  // (only when grep/find IS the command, not a downstream pipe filter)
  const firstTokens = c.trim().split(/\s+/, 1)[0];
  if (/^(grep|egrep|fgrep|rg)$/.test(firstTokens)) {
    return { d: 'deny', r: 'MCP-first: use the Grep tool for content search (or clangd-lsp go-to-references for symbols) instead of raw grep in Bash.' };
  }
  if (firstTokens === 'find') {
    return { d: 'deny', r: 'MCP-first: use the Glob tool to find files instead of raw find in Bash.' };
  }

  // --- MCP-first: documentation fetching belongs to MCP/WebFetch -----------
  if (/\b(curl|wget|Invoke-WebRequest|iwr)\b/.test(c) &&
      /(doc\.qt\.io|learn\.microsoft\.com|cppreference\.com|api\.libssh\.org|harfbuzz\.github\.io|freetype\.org)/.test(c)) {
    return { d: 'deny', r: 'MCP-first: use the qt-docs / context7 MCP servers (or WebFetch) for documentation, not curl/wget in Bash.' };
  }

  // --- Destructive-but-sometimes-legit: make the human confirm -------------
  if (/\bgit reset --hard\b/.test(c) || /\bgit clean\b[^|;&]*-[a-zA-Z]*f/.test(c)) {
    return { d: 'ask', r: 'Destructive git operation — confirm you want to discard local work (checkpoints exist via /rewind, but still).' };
  }
  if (/\brm\s+-[a-zA-Z]*[rf]/.test(c) && !/\brm\s+-[a-zA-Z]*[rf][a-zA-Z]*\s+(\.\/)?(build|out)\b/.test(c)) {
    return { d: 'ask', r: 'Recursive/forced delete outside build|out — confirm the target is right.' };
  }
  return null;
}

function main() {
  const input = safe(() => JSON.parse(fs.readFileSync(0, 'utf8')), {});
  const cmd = input && input.tool_input && input.tool_input.command;
  if (typeof cmd !== 'string' || !cmd) { return; }
  const v = decide(cmd);
  if (!v) { return; }
  const out = {
    hookSpecificOutput: {
      hookEventName: 'PreToolUse',
      permissionDecision: v.d,
      permissionDecisionReason: v.r,
    },
  };
  if (v.d === 'deny') { out.decision = 'block'; out.reason = v.r; } // legacy-schema compat
  process.stdout.write(JSON.stringify(out));
}

safe(main);
process.exit(0);

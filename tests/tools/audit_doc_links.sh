#!/usr/bin/env bash
# =============================================================
# audit_doc_links.sh — do the docs point at things that exist?
#
# Two kinds of reference rot, both invisible on the page:
#
#   [label](../reference/thing.md)   a link to a page that was renamed
#   `web/static/src/components/X.js` a backticked path to a file that moved
#
# The second is the common one: a path written from the wrong directory looks
# perfectly plausible and nobody clicks it.
#
#     ./tests/tools/audit_doc_links.sh
#
# Reports and exits non-zero; never edits. `deprecated/` is skipped — it is
# frozen and its links point at a repository that no longer exists.
# =============================================================
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1

node - "$@" <<'JS'
const fs = require('fs');
const path = require('path');

// Files the running system CREATES, and placeholders inside examples. Naming
// one is correct even though it is not in the tree.
const EXPECTED_ABSENT = new Set([
    'config/tenants.json',            // copied from tenants.json.example on setup
    'modules/mymod/MyModule.hpp',     // the "adding a module" walkthrough
    'modules/mymod/MyModule.cpp',
]);

function walk(d, out = []) {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
        const p = path.join(d, e.name);
        if (e.isDirectory()) { if (e.name !== 'deprecated') walk(p, out); }
        else if (e.name.endsWith('.md')) out.push(p);
    }
    return out;
}

// Fenced code blocks hold C++ and JSON that look like markdown links.
const stripCode = t => t.replace(/```[\s\S]*?```/g, '');

let broken = 0, checked = 0;
for (const f of walk('docs')) {
    const text = stripCode(fs.readFileSync(f, 'utf8'));
    const dir  = path.dirname(f);

    for (const m of text.matchAll(/\[[^\]]*\]\(([^)\s]+)\)/g)) {
        const t = m[1].split('#')[0].trim();
        if (!t || /^(https?:|mailto:)/.test(t)) continue;
        checked++;
        if (!fs.existsSync(path.resolve(dir, t))) {
            console.log(`BROKEN LINK   ${f}\n              -> ${t}`); broken++;
        }
    }

    for (const m of text.matchAll(/`([A-Za-z0-9_./-]+\.(cpp|hpp|js|mjs|sh|py|json|css|html|md|sql|dump))`/g)) {
        const t = m[1];
        if (!t.includes('/') || t.startsWith('.') || EXPECTED_ABSENT.has(t)) continue;
        checked++;
        if (!fs.existsSync(t) && !fs.existsSync(path.resolve(dir, t))) {
            console.log(`MISSING PATH  ${f}\n              -> ${t}`); broken++;
        }
    }
}
console.log(`\n${checked} references checked, ${broken} broken.`);
process.exit(broken ? 1 : 0);
JS

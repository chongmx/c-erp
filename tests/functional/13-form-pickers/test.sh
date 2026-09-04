#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Every form that holds a picker, opened in a REAL browser.
#
# Why this exists, when tests/integration/core/m2o-picker already passes: an
# API test proves the server answers correctly. It cannot see that the screen
# is blank.
#
# The m2o rewrite replaced a plain <select> with an OWL sub-component in ~30
# places across 15 form classes. OWL resolves a sub-component by name at FIRST
# RENDER, and a class that uses <M2OSelect/> without naming it in its
# `static components` throws
#
#     Cannot find the definition of component "M2OSelect"
#
# only when a user opens that particular form. The server logs nothing, every
# RPC returns 200, and the whole integration suite stays green in front of a
# screen that renders nothing at all. That is exactly what happened to
# LocationFormView and WarehouseFormView while this change was being written —
# both were caught by this harness and by nothing else.
#
# tests/lib/render_forms.mjs does the driving; see
# tests/docs/browser-render-checks.md for the traps it works around.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

CHROME=${CHROME_PATH:-/usr/bin/google-chrome}

# -------------------------------------------------------------------------
sec "1. can this machine drive a browser at all?"
# -------------------------------------------------------------------------
# Skip rather than fail where Chrome or puppeteer is absent: a missing browser
# is a missing tool, not a broken product, and a red suite that means "you did
# not install Chrome" trains people to ignore red suites.
if [ ! -x "$CHROME" ]; then
    echo "    NOTE  no Chrome at $CHROME — skipping the browser checks"
    verdict; exit $?
fi
if [ ! -d node_modules/puppeteer-core ]; then
    echo "    NOTE  puppeteer-core is not installed — skipping the browser checks"
    verdict; exit $?
fi
ok "Chrome and puppeteer-core are present"

# -------------------------------------------------------------------------
sec "2. every converted form renders, with its pickers"
# -------------------------------------------------------------------------
# render_forms.mjs opens each model's FORM — existing record where the table
# has one, "New" where it does not — and reports every console error, failed
# request and blank panel. It exits non-zero if any model fails.
OUT=$(SHOTDIR=/tmp/render_forms_test BASE="$BASE" DBN="$DBN" \
      timeout 400 node tests/lib/render_forms.mjs --all 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'

if [ "$RC" -eq 0 ]; then ok "all forms rendered without a browser error"
else no "at least one form failed to render (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the specific screens the user reported"
# -------------------------------------------------------------------------
# A count of zero pickers on the rental contract would mean the widget silently
# rendered nothing — the form would look fine and be unusable, which is the
# failure this whole change exists to remove.
RC_LINE=$(printf '%s' "$OUT" | grep 'rental.contract ')
t_contains "$RC_LINE" "PASS" "the rental contract form renders"
case "$RC_LINE" in
    *pickers=0*) no "the rental contract form shows NO pickers" ;;
    *pickers=*)  ok "the rental contract form shows its pickers" ;;
    *)           no "no picker count reported for the rental contract" ;;
esac

CT_LINE=$(printf '%s' "$OUT" | grep 'res.partner ')
t_contains "$CT_LINE" "PASS" "the contact form renders"
case "$CT_LINE" in
    *pickers=0*) no "the contact form shows NO company picker" ;;
    *pickers=*)  ok "the contact form shows its company picker" ;;
    *)           no "no picker count reported for the contact form" ;;
esac

# -------------------------------------------------------------------------
sec "4. the picker is actually usable, not merely present"
# -------------------------------------------------------------------------
# Rendering is not working. A debounce that never fires, a dropdown that opens
# behind the card, an option whose mousedown loses the race with the input's
# blur — every one of those renders perfectly and cannot be used. So this types
# into the Customer picker on a new rental contract, picks the match, clears it,
# and pages the Browse dialog.
PICK=$(SHOT=/tmp/render_pick_test.png BASE="$BASE" DBN="$DBN" \
       timeout 250 node tests/lib/render_pick.mjs 2>&1)
PRC=$?
echo "$PICK" | sed 's/^/      /'
if [ "$PRC" -eq 0 ]; then ok "the picker can be typed into, chosen from and cleared"
else no "driving the picker failed (see above)"; fi

# -------------------------------------------------------------------------
sec "5. no form was left using the old control"
# -------------------------------------------------------------------------
# A static check, but it belongs with the browser ones: it is the same class of
# mistake and it is far cheaper to catch here than by opening 15 screens.
#
# The one remaining <select> bound to an m2o is the expense line's tax picker,
# which is deliberate: that list carries rate and price_include, which the form
# reads to keep the totals live as the user types. It is not a plain picker.
LEFTOVER=$(grep -c 't-att-selected=".*[mM]2oId(' web/static/src/app.js)
t_eq "1" "$LEFTOVER" "only the expense tax select still uses the old pattern"

# Every class that renders the widget must also declare it, which is the exact
# defect section 2 catches at runtime.
MISSING=$(node - <<'JS'
const fs = require('fs');
const lines = fs.readFileSync('web/static/src/app.js', 'utf8').split('\n');
const bounds = [];
lines.forEach((l, i) => { const m = l.match(/^class (\w+)/); if (m) bounds.push([i, m[1]]); });
bounds.push([lines.length, '(end)']);
const bad = [];
for (let b = 0; b < bounds.length - 1; b++) {
    const body = lines.slice(bounds[b][0], bounds[b + 1][0]).join('\n');
    if (!/<M2OSelect[\s/>]/.test(body)) continue;
    if (!/static components\s*=\s*\{[^}]*\bM2OSelect\b/.test(body)) bad.push(bounds[b][1]);
}
process.stdout.write(bad.join(','));
JS
)
t_eq "" "$MISSING" "every class that renders M2OSelect also declares it"

verdict

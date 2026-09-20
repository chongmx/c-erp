#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# A deploy reaches the browser at once.
#
# Reported: "User access api keys get me into an internal error."
#
# The new binary had seeded the API Keys menu; the browser was still running
# the OLD app.js, which had never heard of that screen, fell back to a generic
# list of model 'api.keys', and got "internal error". c-erp sent no caching
# header on its scripts, so Cloudflare applied its default four-hour browser
# TTL — every deploy was invisible for up to four hours.
#
# Pinned here:
#   1. the app shell is no-cache, and every local script and stylesheet URL in
#      it carries ?v=<modification time>;
#   2. the version changes when the file does, and a versioned URL serves;
#   3. asking for a model that does not exist says so — not "internal error".
# (The deploy half — the host's checkout fast-forwarded to the built commit —
# is scripts/deploy.sh Step 3/4b.)
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

sec "1. the shell"
CODE=$(curl -s -D "$TMP/h" -o "$TMP/shell.html" -w '%{http_code}' "$BASE/login")
t_eq "200" "$CODE" "the app shell is served at /login"
t_contains "$(tr 'A-Z' 'a-z' < "$TMP/h")" "cache-control: no-cache" "and is never cached without revalidating"
TOTAL=$(grep -oE '(src|href)="/(src|lib)/[^"]+\.(js|css)[^"]*"' "$TMP/shell.html" | wc -l)
BARE=$(grep -oE '(src|href)="/(src|lib)/[^"?]+\.(js|css)"' "$TMP/shell.html" | wc -l)
t_ge "$TOTAL" "20" "it references the app's scripts and styles ($TOTAL)"
t_eq "0" "$BARE" "every one of them carries ?v= (none bare)"
APPV=$(grep -oE 'src="/src/app\.js\?v=[0-9a-f]+"' "$TMP/shell.html" | head -1)
t_nonempty "$APPV" "app.js is versioned ($APPV)"

sec "2. the version follows the file"
URL=$(echo "$APPV" | sed 's/^src="//; s/"$//')
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE$URL")" "the versioned URL serves"
F=web/static/src/components/apikeys.css
BEFORE=$(curl -s "$BASE/login" | grep -oE 'href="/src/components/apikeys\.css\?v=[0-9a-f]+"')
touch -d '@1600000000' "$F"
AFTER=$(curl -s "$BASE/login" | grep -oE 'href="/src/components/apikeys\.css\?v=[0-9a-f]+"')
touch "$F"
t_ne "$BEFORE" "$AFTER" "changing a file changes its URL"
UNTOUCHED1=$(echo "$(curl -s "$BASE/login")" | grep -oE 'src="/lib/owl\.iife\.js\?v=[0-9a-f]+"')
t_contains "$(cat "$TMP/shell.html")" "$UNTOUCHED1" "an unchanged file keeps its URL (and its cache)"

sec "3. an unknown model says what it is"
auth_or_die
R=$(call no.such.model search_read '[[]]')
has_error "$R" && ok "an unknown model is refused" || no "an unknown model answered"
t_contains "$R" "has no model 'no.such.model'" "by name"
t_lacks "$R" "An internal error occurred" "not as an internal error"

verdict

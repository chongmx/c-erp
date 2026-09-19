#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Settings → Users & Access → API Keys, on screen (tests/lib/render_api_keys.mjs).
#
# Asked for: "Create an API management ui page at the settings" — so a person
# can make a key for a script or an agent, scoped and limited, and revoke it.
#
# The key is made and revoked by CLICKING. Between the two, this script uses
# the token the screen showed against the real API: the page is only right if
# what it hands out works, and stops working when it says "revoked".
# The API itself is pinned in tests/integration/api/tickets-v1.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
PFX='ZZAK'
TOKFILE=$(mktemp)

cleanup() {
    pg "DELETE FROM res_users_apikey WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM project_project WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    rm -f "$TOKFILE"
}
trap cleanup EXIT
cleanup
TOKFILE=$(mktemp)
auth_or_die

CHROME=${CHROME_PATH:-/usr/bin/google-chrome}
sec "1. the browser tooling"
if [ ! -x "$CHROME" ] || [ ! -d node_modules/puppeteer-core ]; then
    echo "    NOTE  no Chrome or puppeteer-core — skipping the on-screen journey"
    verdict; exit $?
fi
ok "Chrome and puppeteer-core are present"
# The project the key will be limited to. It is not what this journey is
# about — the key is — so it may be seeded directly.
P=$(pgid "INSERT INTO project_project (name, task_prefix) VALUES ('${PFX} Project', '${PFX}P') RETURNING id")
t_nonempty "$P" "a project to limit the key to"

sec "2. creating a key, on screen"
OUT=$(SHOTDIR=/tmp/api_keys_test BASE="$BASE" DBN="$DBN" timeout 200 node tests/lib/render_api_keys.mjs "$PFX" "$TOKFILE" create 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
[ "$RC" -eq 0 ] && ok "a key is created from the Settings page" || no "creating the key on screen failed"
TOKEN=$(cat "$TOKFILE" 2>/dev/null)

sec "3. what the screen handed out works"
t_eq "200" "$(curl -s -o /tmp/ak_me -w '%{http_code}' -H "Authorization: Bearer $TOKEN" "$BASE/api/v1/me")" \
     "the shown key authenticates"
t_contains "$(cat /tmp/ak_me)" "comments:write" "with the permission that was ticked"
t_contains "$(cat /tmp/ak_me)" "\"project_ids\":[$P]" "limited to the project that was ticked"
t_eq "403" "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Authorization: Bearer $TOKEN" \
              -H 'Content-Type: application/json' --data '{"project":"ZZAKP","title":"x"}' "$BASE/api/v1/tickets")" \
     "and without the one that was not (tickets:write)"
t_ge "$(pg "SELECT (expires_at - now() BETWEEN interval '29 days' AND interval '31 days')::int FROM res_users_apikey WHERE name='${PFX} key'")" \
     "1" "it expires in 30 days, as chosen"

sec "4. revoking it, on screen"
OUT=$(SHOTDIR=/tmp/api_keys_test BASE="$BASE" DBN="$DBN" timeout 200 node tests/lib/render_api_keys.mjs "$PFX" "$TOKFILE" revoke 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
[ "$RC" -eq 0 ] && ok "the key is revoked from the Settings page" || no "revoking the key on screen failed"
t_eq "401" "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" "$BASE/api/v1/me")" \
     "and the API refuses it at once"

verdict

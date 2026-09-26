#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# /api/v1/parts/lookup, and editing a key's permissions.  (CERP-13, CERP-14)
#
# The lookup was reachable only from the Part Lookup screen, so every
# diagnosis of a bad model reply needed somebody sitting at it. It is now a
# REST endpoint with its own scope: POST starts the job (CERP-10) and answers
# at once, GET polls it.
#
# `parts:lookup` is deliberately its OWN scope and is implied by nothing. It
# spends money — each lookup is a paid call to the model provider — so a key
# that reads tickets must not acquire it because somebody widened a list.
#
# And because granting one more permission used to mean revoking the key and
# pasting a new one everywhere it was used, permissions are now editable on an
# existing key, without the token changing (CERP-14).
#
# The provider is forced to `mock` throughout: this suite makes no network
# calls, and a test that quietly spent money on somebody's API key would be a
# bad test however green it was.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
PFX='ZZPA'
TMP=$(mktemp -d)

SNAP=$(pgv "SELECT format('UPDATE ir_ai_settings SET provider=%L, enabled=%s WHERE id=1',
              provider, enabled) FROM ir_ai_settings WHERE id=1" | sed 's/^ *//')
cleanup(){
    [ -n "$SNAP" ] && pg "$SNAP" >/dev/null 2>&1
    pg "DELETE FROM ir_ai_job WHERE query LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_users_apikey WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_users WHERE login='${PFX}_dev@t.test'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name='${PFX} Dev'" >/dev/null 2>&1
}
cleanup; trap 'cleanup; rm -rf "$TMP"' EXIT
auth_or_die
pg "UPDATE ir_ai_settings SET provider='mock', enabled=true WHERE id=1" >/dev/null

api(){
    local m="$1" p="$2" k="$3" b="${4:-}"
    if [ -n "$b" ]; then
        curl -s -o "$TMP/out" -w '%{http_code}' -X "$m" -H "Authorization: Bearer $k" \
             -H 'Content-Type: application/json' --data "$b" "$BASE$p"
    else
        curl -s -o "$TMP/out" -w '%{http_code}' -X "$m" -H "Authorization: Bearer $k" "$BASE$p"
    fi
    printf ' '; cat "$TMP/out"
}
st(){ echo "${1%% *}"; }
bd(){ echo "${1#* }"; }
jget(){ printf '%s' "$1" | python3 -c "import json,sys
try: r=json.load(sys.stdin)
except Exception: print('<not json>'); sys.exit()
try: print($2)
except Exception: print('')" 2>/dev/null; }
newkey(){ call api.key create_key "[{\"name\":\"$1\",\"scopes\":$2}]"; }

# -------------------------------------------------------------------------
sec "1. the permission exists, and nothing hands it out by accident"
# -------------------------------------------------------------------------
S=$(call api.key scopes '[]')
t_contains "$S" 'parts:lookup' "parts:lookup is offered when a key is made"
t_contains "$S" 'daily call cap' "and says plainly that it spends money"

KR_R=$(newkey "${PFX} reader" '["tickets:read"]')
KR=$(jget "$KR_R" "r['result']['token']")
KR_ID=$(jget "$KR_R" "r['result']['id']")
t_nonempty "$KR" "a key that may only read tickets"
t_eq "0" "$(pg "SELECT count(*) FROM res_users_apikey WHERE id=${KR_ID:-0} AND 'parts:lookup' = ANY(scopes)")" \
     "which did NOT acquire parts:lookup along the way"

# A write scope brings tickets:read with it; it must not bring this.
KW=$(jget "$(newkey "${PFX} writer" '["tickets:write"]')" "r['result']['token']")
t_eq "0" "$(pg "SELECT count(*) FROM res_users_apikey WHERE name='${PFX} writer' AND 'parts:lookup' = ANY(scopes)")" \
     "nor does a ticket-write key"

# -------------------------------------------------------------------------
sec "2. without the permission, the endpoint is closed"
# -------------------------------------------------------------------------
A=$(api POST /api/v1/parts/lookup "$KR" '{"query":"'${PFX}' 4k7 resistor"}')
t_eq "403" "$(st "$A")" "starting a lookup without parts:lookup is 403"
t_contains "$(bd "$A")" 'parts:lookup' "and the refusal names the permission that is missing"
t_eq "0" "$(pg "SELECT count(*) FROM ir_ai_job WHERE query LIKE '${PFX}%'")" \
     "nothing was queued — no call to the provider was paid for"
t_eq "403" "$(st "$(api GET /api/v1/parts/lookup/1 "$KR")")" "reading one is 403 too"
t_eq "401" "$(st "$(api POST /api/v1/parts/lookup 'not-a-key' '{"query":"x"}')")" "no key: 401"

# -------------------------------------------------------------------------
sec "3. with it, a lookup runs as a job"
# -------------------------------------------------------------------------
KL_R=$(newkey "${PFX} looker" '["parts:lookup"]')
KL=$(jget "$KL_R" "r['result']['token']")
KL_ID=$(jget "$KL_R" "r['result']['id']")
t_nonempty "$KL" "a key that may look parts up"

START=$(date +%s%3N)
A=$(api POST /api/v1/parts/lookup "$KL" '{"query":"'${PFX}' 8Mhz temperature controlled crystal"}')
ELAPSED=$(( $(date +%s%3N) - START ))
echo "    POST answered in ${ELAPSED} ms: $(bd "$A" | head -c 120)"
t_eq "202" "$(st "$A")" "starting one is 202 Accepted — the answer is not ready yet"
JID=$(jget "$(bd "$A")" "r['job_id']")
t_nonempty "$JID" "with a job id to poll"
t_contains "$(bd "$A")" '/api/v1/parts/lookup/' "and the URL to poll it at"
# The point of the shape: the request does not wait for the model.
[ "$ELAPSED" -lt 2000 ] && ok "and it returned immediately (${ELAPSED} ms)" \
                        || no "POST took ${ELAPSED} ms — it is waiting for the model"

for i in $(seq 1 30); do
    STATE=$(pg "SELECT state FROM ir_ai_job WHERE id=${JID:-0}")
    case "$STATE" in done|failed|cancelled) break ;; esac
    sleep 1
done
G=$(api GET /api/v1/parts/lookup/"$JID" "$KL")
t_eq "200" "$(st "$G")" "polling it is 200"
t_eq "done" "$(jget "$(bd "$G")" "r['state']")" "and it finished"
t_eq "True" "$(jget "$(bd "$G")" "r['ok']")" "with an answer"
t_ge "$(jget "$(bd "$G")" "len(r['candidates'])")" 1 "carrying candidates"
t_contains "$(bd "$G")" 'seconds' "and how long it took, for a caller that wants to show progress"

# An empty question is not work, and an unknown field is a typo worth saying.
t_eq "400" "$(st "$(api POST /api/v1/parts/lookup "$KL" '{"query":""}')")" "an empty query is 400"
t_eq "400" "$(st "$(api POST /api/v1/parts/lookup "$KL" '{"q":"typo"}')")" "an unknown field is 400"

# -------------------------------------------------------------------------
sec "4. a lookup belongs to the key's owner"
# -------------------------------------------------------------------------
PART=$(call res.partner create "[{\"name\":\"${PFX} Dev\",\"email\":\"${PFX}_dev@t.test\"}]" | rid)
U2=$(call res.users create "[{\"login\":\"${PFX}_dev@t.test\",\"password\":\"Zzpa-Pass-1\",\"partner_id\":${PART},\"active\":true}]" | rid)
pg "INSERT INTO res_groups_users_rel (gid, uid) VALUES (2, ${U2:-0}) ON CONFLICT DO NOTHING" >/dev/null
CO=$(pg "SELECT company_id FROM res_users WHERE login='admin'")
pg "UPDATE res_users SET company_id=${CO:-1} WHERE id=${U2:-0}" >/dev/null
pg "INSERT INTO res_company_users_rel (company_id, user_id) VALUES (${CO:-1}, ${U2:-0}) ON CONFLICT DO NOTHING" >/dev/null
S2=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
     --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"${PFX}_dev@t.test\",\"password\":\"Zzpa-Pass-1\"}}" \
     | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('session_id') or (d.get('result') or {}).get('session_id') or '')" 2>/dev/null)
t_nonempty "$S2" "a second user can sign in"
K2=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
     --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"api.key\",\"method\":\"create_key\",\"args\":[{\"name\":\"${PFX} other\",\"scopes\":[\"parts:lookup\"]}],\"kwargs\":{\"context\":{\"session_id\":\"$S2\"}}}}" \
     | python3 -c "import json,sys; print(json.load(sys.stdin).get('result',{}).get('token',''))" 2>/dev/null)
t_nonempty "$K2" "with a key of their own, holding the same permission"
# The permission says this key MAY look parts up. It does not say whose
# lookups it may read.
X=$(api GET /api/v1/parts/lookup/"$JID" "$K2")
t_eq "404" "$(st "$X")" "somebody else's lookup is 404, not somebody else's answer"
t_lacks "$(bd "$X")" 'candidates' "and none of it leaks"

# -------------------------------------------------------------------------
sec "5. a key's permissions can be changed without re-issuing it"
# -------------------------------------------------------------------------
BEFORE_HASH=$(pg "SELECT token_hash FROM res_users_apikey WHERE id=${KR_ID:-0}")
call api.key set_scopes "[{\"id\":${KR_ID:-0},\"scopes\":[\"tickets:read\",\"parts:lookup\"]}]" >/dev/null
t_eq "parts:lookup,tickets:read" \
     "$(pg "SELECT array_to_string(ARRAY(SELECT unnest(scopes) ORDER BY 1), ',') FROM res_users_apikey WHERE id=${KR_ID:-0}")" \
     "the permission is granted"
t_eq "$BEFORE_HASH" "$(pg "SELECT token_hash FROM res_users_apikey WHERE id=${KR_ID:-0}")" \
     "and the token is unchanged — nothing has to be pasted anywhere again"
# The same key, which was 403 in section 2.
t_eq "202" "$(st "$(api POST /api/v1/parts/lookup "$KR" '{"query":"'${PFX}' now allowed"}')")" \
     "so the key that was refused can now do it"

# Narrowing takes effect on the next request, not at the next rotation.
call api.key set_scopes "[{\"id\":${KR_ID:-0},\"scopes\":[\"tickets:read\"]}]" >/dev/null
t_eq "403" "$(st "$(api POST /api/v1/parts/lookup "$KR" '{"query":"'${PFX}' taken away"}')")" \
     "and taking it away stops it immediately"

# The column is `operation`, not `action` — widening a credential that acts as
# a person is exactly the kind of change that should be answerable later.
t_ge "$(pg "SELECT count(*) FROM audit_log WHERE model='api.key' AND operation='set_scopes'")" 1 \
     "a permission change is audited"
t_eq "$(pg "SELECT id FROM res_users WHERE login='admin'")" \
     "$(pg "SELECT uid FROM audit_log WHERE model='api.key' AND operation='set_scopes' ORDER BY id DESC LIMIT 1")" \
     "against the person who made it"

BAD=$(call api.key set_scopes "[{\"id\":${KR_ID:-0},\"scopes\":[\"everything\"]}]")
has_error "$BAD" && ok "an unknown permission is refused" || no "scope 'everything' was accepted"
EMPTY=$(call api.key set_scopes "[{\"id\":${KR_ID:-0},\"scopes\":[]}]")
has_error "$EMPTY" && ok "and a key cannot be left with none" || no "a key was left with no permissions"

# A revoked key is a closed account, not a narrow one.
call api.key revoke "[{\"id\":${KR_ID:-0}}]" >/dev/null
DEAD=$(call api.key set_scopes "[{\"id\":${KR_ID:-0},\"scopes\":[\"tickets:read\"]}]")
has_error "$DEAD" && ok "a revoked key cannot be edited back into use" || no "a revoked key was edited"

# Somebody else's key is not yours to widen.
OTHER_ID=$(pg "SELECT id FROM res_users_apikey WHERE name='${PFX} other'")
MINE=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
       --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"api.key\",\"method\":\"set_scopes\",\"args\":[{\"id\":${KL_ID:-0},\"scopes\":[\"tickets:read\",\"parts:lookup\"]}],\"kwargs\":{\"context\":{\"session_id\":\"$S2\"}}}}")
has_error "$MINE" && ok "an ordinary user cannot widen somebody else's key" \
                  || no "a second user edited the admin's key"

verdict

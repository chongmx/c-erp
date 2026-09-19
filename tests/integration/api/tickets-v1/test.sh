#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# API keys and the issue-tracker REST API (/api/v1) — modules/api.
#
# Asked for: "a granularized api access starting with this issue management.
# I will then create an API key at the remote location, and pass it to you.
# After that, you may close or update the tracker as we proceed."
#
# What this file pins down:
#   1. keys: shown once, stored as a hash, named, scoped, optionally limited
#      to projects, expiring, revocable — and managed only over a SESSION;
#   2. authentication: missing, malformed, unknown, revoked, expired keys and
#      a deactivated owner are all refused, each with its own reason;
#   3. scopes: a read-only key cannot write, comment or attach;
#   4. the ticket lifecycle — create, read, search, update, close — with the
#      history written as the key's OWNER, exactly as from the screen;
#   5. comments and attachments, including a byte-for-byte download;
#   6. project limits: another project's ticket is simply not there (404);
#   7. a key is not a session: JSON-RPC ignores it.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
PFX='ZZAPI'
TMP=$(mktemp -d)

jget(){ printf '%s' "$1" | python3 -c "import json,sys
try:
    r=json.load(sys.stdin)
except Exception as e:
    print('<not json>'); sys.exit()
try:
    v=($2)
except Exception as e:
    v='<err:'+str(e)+'>'
print(v if not isinstance(v,(dict,list)) else json.dumps(v, ensure_ascii=False))" 2>/dev/null; }
rpcres(){ jget "$1" "r.get('result')"; }

# api METHOD PATH KEY [JSON-BODY]  -> prints "STATUS BODY"
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
st(){ echo "${1%% *}"; }          # the status of an api() answer
bd(){ echo "${1#* }"; }           # its body

cleanup() {
    pg "DELETE FROM res_users_apikey WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM mail_message WHERE res_model='project.task' AND res_id IN
          (SELECT t.id FROM project_task t JOIN project_project p ON p.id=t.project_id WHERE p.name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM ir_attachment WHERE res_model='project.task' AND res_id IN
          (SELECT t.id FROM project_task t JOIN project_project p ON p.id=t.project_id WHERE p.name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM project_project WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM project_tag WHERE lower(name) LIKE 'zzapi%'" >/dev/null 2>&1
    pg "DELETE FROM res_users WHERE login LIKE 'zzapi_%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
}
cleanup
trap 'cleanup; rm -rf "$TMP"' EXIT
auth_or_die
ADMIN=$(pg "SELECT id FROM res_users WHERE login='admin'")

P1=$(call project.project create "[{\"name\":\"${PFX} One\",\"task_prefix\":\"ZZAPA\"}]" | rid)
P2=$(call project.project create "[{\"name\":\"${PFX} Two\",\"task_prefix\":\"ZZAPB\"}]" | rid)
t_nonempty "$P1" "project ZZAPA"; t_nonempty "$P2" "project ZZAPB"

newkey(){   # newkey NAME SCOPES-JSON [PROJECT-IDS-JSON] [DAYS]
    call api.key create_key "[{\"name\":\"$1\",\"scopes\":$2,\"project_ids\":${3:-[]},\"expires_days\":${4:-90}}]"
}

# -------------------------------------------------------------------------
sec "1. keys are created over a session, shown once, stored as a hash"
# -------------------------------------------------------------------------
R=$(newkey "${PFX} full" '["tickets:write","comments:write","attachments:write"]')
KW=$(jget "$R" "r['result']['token']")
KW_ID=$(jget "$R" "r['result']['id']")
if [[ "$KW" =~ ^cerp_[0-9a-f]{48}$ ]]; then ok "a key looks like cerp_ + 48 hex characters"
else no "unexpected key shape: '$KW'"; fi
t_eq "0" "$(pg "SELECT count(*) FROM res_users_apikey WHERE token_hash = '$KW' OR token_prefix = '$KW'")" \
     "the token itself is not stored"
t_eq "1" "$(pg "SELECT count(*) FROM res_users_apikey WHERE id=${KW_ID:-0} AND token_hash = encode(sha256('$KW'::bytea),'hex')")" \
     "its SHA-256 is"
t_eq "${KW:0:13}" "$(pg "SELECT token_prefix FROM res_users_apikey WHERE id=${KW_ID:-0}")" \
     "the visible prefix is its first 13 characters"
t_eq "attachments:write,comments:write,tickets:read,tickets:write" \
     "$(pg "SELECT array_to_string(ARRAY(SELECT unnest(scopes) ORDER BY 1), ',') FROM res_users_apikey WHERE id=${KW_ID:-0}")" \
     "a write scope brings tickets:read with it"
L=$(call api.key list '[]')
t_lacks "$L" "$KW" "the key list never contains a token"
t_contains "$L" "${PFX} full" "but lists the key by name"

KR=$(jget "$(newkey "${PFX} read" '["tickets:read"]')" "r['result']['token']")
KP=$(jget "$(newkey "${PFX} proj" '["tickets:write","comments:write"]' "[${P1}]")" "r['result']['token']")
t_nonempty "$KR" "a read-only key"; t_nonempty "$KP" "a key limited to ZZAPA"

has_error "$(newkey "" '["tickets:read"]')"            && ok "a key needs a name"      || no "an unnamed key was created"
has_error "$(newkey "${PFX} x" '["everything"]')"      && ok "an unknown scope is refused" || no "scope 'everything' accepted"
has_error "$(newkey "${PFX} x" '[]')"                  && ok "a key needs a permission"  || no "a key with no scope was created"
has_error "$(newkey "${PFX} x" '["tickets:read"]' '[99999999]')" && ok "a nonexistent project is refused" || no "project 99999999 accepted"

# -------------------------------------------------------------------------
sec "2. authentication"
# -------------------------------------------------------------------------
t_eq "401" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/v1/me")" "no key: 401"
t_eq "401" "$(st "$(api GET /api/v1/me 'not-a-key')")" "a malformed key: 401"
A=$(api GET /api/v1/me "cerp_$(printf '0%.0s' $(seq 1 48))")
t_eq "401" "$(st "$A")" "an unknown key: 401"
A=$(api GET /api/v1/me "$KW")
t_eq "200" "$(st "$A")" "the key works"
t_eq "admin" "$(jget "$(bd "$A")" "r['user']['login']")" "and acts as its owner"
t_nonempty "$(pg "SELECT last_used_at FROM res_users_apikey WHERE id=${KW_ID}")" "its last use is recorded"

KX=$(jget "$(newkey "${PFX} revoke-me" '["tickets:read"]')" "r['result']['token']")
KX_ID=$(pg "SELECT id FROM res_users_apikey WHERE name='${PFX} revoke-me'")
call api.key revoke "[{\"id\":${KX_ID}}]" >/dev/null
A=$(api GET /api/v1/me "$KX")
t_eq "401|revoked" "$(st "$A")|$(jget "$(bd "$A")" "r['error']")" "a revoked key is refused at once, and says so"

KE=$(jget "$(newkey "${PFX} expire-me" '["tickets:read"]')" "r['result']['token']")
pg "UPDATE res_users_apikey SET expires_at = now() - interval '1 minute' WHERE name='${PFX} expire-me'" >/dev/null
A=$(api GET /api/v1/me "$KE")
t_eq "401|expired" "$(st "$A")|$(jget "$(bd "$A")" "r['error']")" "an expired key is refused, and says so"

# A plain employee's key, whose owner is then deactivated.
PART=$(call res.partner create "[{\"name\":\"${PFX} Dev\",\"email\":\"zzapi_dev@t.test\"}]" | rid)
U2=$(call res.users create "[{\"login\":\"zzapi_dev@t.test\",\"password\":\"Zzapi-Pass-1\",\"partner_id\":${PART},\"active\":true}]" | rid)
pg "INSERT INTO res_groups_users_rel (gid, uid) VALUES (2, ${U2:-0}) ON CONFLICT DO NOTHING" >/dev/null
CO=$(pg "SELECT company_id FROM res_users WHERE id=$ADMIN")
pg "UPDATE res_users SET company_id=${CO:-1} WHERE id=${U2:-0}" >/dev/null
pg "INSERT INTO res_company_users_rel (company_id, user_id) VALUES (${CO:-1}, ${U2:-0}) ON CONFLICT DO NOTHING" >/dev/null
S2=$(login 'zzapi_dev@t.test' 'Zzapi-Pass-1')
K2=$(jget "$(call_as "$S2" api.key create_key "[{\"name\":\"${PFX} dev\",\"scopes\":[\"comments:write\"]}]")" "r['result']['token']")
t_nonempty "$K2" "an ordinary employee can make a key for themselves"
t_eq "zzapi_dev@t.test" "$(jget "$(bd "$(api GET /api/v1/me "$K2")")" "r['user']['login']")" "and it acts as them"
R=$(call_as "$S2" api.key revoke "[{\"id\":${KW_ID}}]")
has_error "$R" && ok "they cannot revoke someone else's key" || no "an employee revoked the admin's key"
L=$(call_as "$S2" api.key list '[]')
t_eq "0" "$(jget "$L" "len(r['result']['all'])")" "nor see everyone's keys"
pg "UPDATE res_users SET active=false WHERE id=${U2}" >/dev/null
t_eq "401" "$(st "$(api GET /api/v1/me "$K2")")" "a deactivated owner's key stops working"
pg "UPDATE res_users SET active=true WHERE id=${U2}" >/dev/null

# -------------------------------------------------------------------------
sec "3. scopes"
# -------------------------------------------------------------------------
A=$(api POST /api/v1/tickets "$KR" '{"project":"ZZAPA","title":"nope"}')
t_eq "403|insufficient_scope" "$(st "$A")|$(jget "$(bd "$A")" "r['error']")" "a read-only key cannot create a ticket"
t_eq "200" "$(st "$(api GET '/api/v1/tickets?project=ZZAPA' "$KR")")" "but can search"

# -------------------------------------------------------------------------
sec "4. the ticket lifecycle"
# -------------------------------------------------------------------------
A=$(api POST /api/v1/tickets "$KW" '{"project":"ZZAPA","title":"ZZAPI save stays on Saving","type":"bug","priority":"high","labels":["zzapi-ui"],"status":"In Progress","assignee":"admin","description":"Steps: press Save."}')
t_eq "201" "$(st "$A")" "a ticket is created"
T=$(bd "$A")
KEY=$(jget "$T" "r['key']")
t_eq "ZZAPA-1" "$KEY" "with the project's next key"
t_eq "bug|high|In Progress|zzapi-ui|Administrator" \
     "$(jget "$T" "'|'.join([r['type'], r['priority'], r['status'], ','.join(r['labels']), r['assignee']['name']])")" \
     "type, priority, status, labels and assignee as sent"
t_eq "Administrator" "$(jget "$T" "r['reporter']['name']")" "the reporter is the key's owner"

A=$(api PATCH "/api/v1/tickets/$KEY" "$KW" '{"status":"Done","priority":"urgent","due":"2026-12-31","blocked":false}')
t_eq "200" "$(st "$A")" "a ticket is updated"
t_eq "True|urgent|2026-12-31" "$(jget "$(bd "$A")" "'|'.join([str(r['closed']), r['priority'], r['due']])")" \
     "closed, re-prioritised and dated"
A=$(api GET "/api/v1/tickets/$KEY/activity" "$KW")
HIST=$(jget "$(bd "$A")" "[m for m in r if m['kind']=='history'][-1]")
t_contains "$HIST" "Status: In Progress → Done" "the history says what changed"
t_contains "$HIST" '"name": "Administrator"' "and who did it — the key's owner"

A=$(api PATCH "/api/v1/tickets/$KEY" "$KW" '{"status":"Nowhere"}')
t_eq "400" "$(st "$A")" "an unknown status is refused"
t_contains "$(bd "$A")" "Statuses: New" "listing the ones that exist"
t_eq "400" "$(st "$(api PATCH "/api/v1/tickets/$KEY" "$KW" '{"colour":"red"}')")" "an unknown field is refused, not ignored"
t_eq "400" "$(st "$(api PATCH "/api/v1/tickets/$KEY" "$KW" '{"priority":"whenever"}')")" "an unknown priority is refused"
t_eq "400" "$(st "$(api PATCH "/api/v1/tickets/$KEY" "$KW" '{"due":"31/12/2026"}')")" "a date not in YYYY-MM-DD is refused"
t_eq "400" "$(st "$(api PATCH "/api/v1/tickets/$KEY" "$KW" '{"assignee":"nobody-here"}')")" "an unknown assignee is refused"
t_eq "404" "$(st "$(api GET /api/v1/tickets/ZZAPA-999 "$KW")")" "a ticket that does not exist is 404"

A=$(api POST /api/v1/tickets "$KW" '{"project":"ZZAPA","title":"ZZAPI second","type":"feature"}')
KEY2=$(jget "$(bd "$A")" "r['key']")
t_eq "200" "$(st "$(api PATCH "/api/v1/tickets/$KEY2" "$KW" "{\"parent\":\"$KEY\"}")")" "a parent is set by key"
t_contains "$(bd "$(api GET "/api/v1/tickets/$KEY" "$KW")")" "$KEY2" "and the parent lists it as a subtask"

S=$(bd "$(api GET '/api/v1/tickets?project=ZZAPA&state=all' "$KW")")
t_eq "2" "$(jget "$S" "r['total']")" "search: both tickets in the project"
t_eq "$KEY2" "$(jget "$(bd "$(api GET '/api/v1/tickets?project=ZZAPA' "$KW")")" "','.join(t['key'] for t in r['tickets'])")" \
     "the default is open tickets only"
t_eq "$KEY" "$(jget "$(bd "$(api GET '/api/v1/tickets?project=ZZAPA&state=closed' "$KW")")" "','.join(t['key'] for t in r['tickets'])")" \
     "state=closed finds the closed one"
t_eq "$KEY" "$(jget "$(bd "$(api GET '/api/v1/tickets?project=ZZAPA&state=all&label=zzapi-ui' "$KW")")" "','.join(t['key'] for t in r['tickets'])")" \
     "by label"
t_eq "$KEY2" "$(jget "$(bd "$(api GET '/api/v1/tickets?project=ZZAPA&state=all&type=feature' "$KW")")" "','.join(t['key'] for t in r['tickets'])")" \
     "by type"
t_eq "$KEY" "$(jget "$(bd "$(api GET '/api/v1/tickets?state=all&q=stays%20on' "$KW")")" "','.join(t['key'] for t in r['tickets'])")" \
     "by words in the title"
t_eq "$KEY" "$(jget "$(bd "$(api GET '/api/v1/tickets?state=all&project=ZZAPA&assignee=me' "$KW")")" "','.join(t['key'] for t in r['tickets'])")" \
     "assignee=me"
t_eq "Done" "$(jget "$(bd "$(api GET /api/v1/projects/ZZAPA/statuses "$KW")")" "[s['name'] for s in r if s['closed']][0]")" \
     "the project's statuses, marking the closing ones"
t_contains "$(bd "$(api GET /api/v1/labels "$KW")")" "zzapi-ui" "labels"
t_contains "$(jget "$(bd "$(api GET '/api/v1/users?q=admin' "$KW")")" "','.join(u['login'] for u in r)")" "admin" "users to assign to"
t_contains "$(bd "$(api GET /api/v1/projects "$KW")")" "ZZAPB" "projects"

# -------------------------------------------------------------------------
sec "5. comments and attachments"
# -------------------------------------------------------------------------
A=$(api POST "/api/v1/tickets/$KEY/comments" "$KW" '{"body":"Fixed in abc1234."}')
t_eq "201" "$(st "$A")" "a comment is posted"
CID=$(jget "$(bd "$A")" "r['id']")
t_eq "Administrator" "$(jget "$(bd "$A")" "r['author']['name']")" "signed by the key's owner"
t_eq "403" "$(st "$(api POST "/api/v1/tickets/$KEY/comments" "$KR" '{"body":"x"}')")" "a read-only key cannot comment"
t_eq "200" "$(st "$(api PATCH "/api/v1/comments/$CID" "$KW" '{"body":"Fixed in abc1234 (and tested)."}')")" "the owner's comment can be edited"
t_eq "True" "$(jget "$(bd "$(api GET "/api/v1/tickets/$KEY/comments" "$KW")")" "r[0]['edited']")" "and is marked edited"
# An employee's key, on someone else's comment:
pg "UPDATE res_users_apikey SET scopes = ARRAY['tickets:read','comments:write'] WHERE name='${PFX} dev'" >/dev/null
A=$(api PATCH "/api/v1/comments/$CID" "$K2" '{"body":"defaced"}')
t_eq "400" "$(st "$A")" "another person's key cannot edit it"
t_contains "$(bd "$A")" "Only the author" "and is told why"

printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89\x00\x00\x00\rIDATx\x9cc\xf8\x0f\x00\x00\x01\x01\x00\x05\x18\xd8N\x00\x00\x00\x00IEND\xaeB`\x82' > "$TMP/shot.png"
CODE=$(curl -s -o "$TMP/up" -w '%{http_code}' -H "Authorization: Bearer $KW" \
       -F "file=@$TMP/shot.png;type=image/png" "$BASE/api/v1/tickets/$KEY/attachments")
t_eq "201" "$CODE" "a screenshot is uploaded"
AID=$(jget "$(cat "$TMP/up")" "r['id']")
t_eq "![shot.png](/web/content/$AID)" "$(jget "$(cat "$TMP/up")" "r['markdown']")" \
     "the answer includes the markdown to put it in a comment"
curl -s -o "$TMP/down.png" -H "Authorization: Bearer $KW" "$BASE/api/v1/attachments/$AID"
cmp -s "$TMP/shot.png" "$TMP/down.png" && ok "and downloads byte for byte" || no "the downloaded file differs"
H=$(curl -s -D - -o /dev/null -H "Authorization: Bearer $KW" "$BASE/api/v1/attachments/$AID" | tr -d '\r')
t_contains "$(echo "$H" | tr 'A-Z' 'a-z')" "content-disposition: attachment" "always as a download, never rendered"
printf 'MZ' > "$TMP/evil.exe"
t_eq "400" "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $KW" \
             -F "file=@$TMP/evil.exe" "$BASE/api/v1/tickets/$KEY/attachments")" \
     "a disallowed type is refused — the same allowlist as the browser"
t_eq "403" "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $KR" \
             -F "file=@$TMP/shot.png;type=image/png" "$BASE/api/v1/tickets/$KEY/attachments")" \
     "a read-only key cannot attach"
t_eq "1" "$(jget "$(bd "$(api GET "/api/v1/tickets/$KEY/attachments" "$KR")")" "len(r)")" "but can list"
t_eq "200" "$(st "$(api DELETE "/api/v1/attachments/$AID" "$KW")")" "an attachment is removed"
t_eq "200" "$(st "$(api DELETE "/api/v1/comments/$CID" "$KW")")" "a comment is removed"
t_eq "200" "$(st "$(api PUT "/api/v1/tickets/$KEY/watch" "$KW")")" "watch"
t_eq "200" "$(st "$(api DELETE "/api/v1/tickets/$KEY/watch" "$KW")")" "unwatch"

# -------------------------------------------------------------------------
sec "6. a key limited to one project sees only that project"
# -------------------------------------------------------------------------
A=$(api POST /api/v1/tickets "$KW" '{"project":"ZZAPB","title":"ZZAPI in the other project"}')
KEYB=$(jget "$(bd "$A")" "r['key']")
t_eq "ZZAPB-1" "$KEYB" "a ticket in the other project"
t_eq "404" "$(st "$(api GET "/api/v1/tickets/$KEYB" "$KP")")" "is not there for the ZZAPA-only key (404, not 403)"
t_eq "404" "$(st "$(api POST /api/v1/tickets "$KP" '{"project":"ZZAPB","title":"x"}')")" "nor can it create there"
t_eq "404" "$(st "$(api POST "/api/v1/tickets/$KEYB/comments" "$KP" '{"body":"x"}')")" "nor comment there"
S=$(bd "$(api GET '/api/v1/tickets?state=all' "$KP")")
t_eq "0" "$(jget "$S" "len([t for t in r['tickets'] if t['project']!='ZZAPA'])")" "its search never leaves ZZAPA"
t_eq "ZZAPA" "$(jget "$(bd "$(api GET /api/v1/projects "$KP")")" "','.join(p['key'] for p in r)")" "and it lists only ZZAPA"

# -------------------------------------------------------------------------
sec "7. a key is not a session"
# -------------------------------------------------------------------------
R=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
         -H "Authorization: Bearer $KW" \
         --data '{"jsonrpc":"2.0","method":"call","params":{"model":"api.key","method":"create_key","args":[{"name":"ZZAPI minted","scopes":["tickets:read"]}],"kwargs":{}}}')
has_error "$R" && ok "JSON-RPC ignores a key — so a key cannot mint another key" || no "a key reached JSON-RPC"
t_eq "0" "$(pg "SELECT count(*) FROM res_users_apikey WHERE name='ZZAPI minted'")" "nothing was created"

verdict

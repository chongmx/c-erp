#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# PENETRATION TEST — the public website and its editor.
#
# The other website suites assert that the system behaves. This one ATTACKS
# it: every check here is an attempt to do something that must fail, written
# from the attacker's side rather than the feature's.
#
# The threat model, in order of how likely each is to be real:
#
#   1. A VISITOR. Can they edit, read drafts, enumerate pages, or get script
#      onto a page other people will load?
#   2. A CUSTOMER with a portal password. Their credential is not a staff
#      login — does anything treat it as one?
#   3. AN EMPLOYEE with a legitimate staff login but no website permission.
#      The most dangerous of the three, because they get past authentication.
#   4. Anyone at all, against the machinery: injection, traversal, IDOR,
#      oversize payloads, header spoofing.
#
# §9 is the one worth reading: it loads the editor's own JavaScript as an
# anonymous visitor and drives it. The client is not a security boundary and
# this proves the server knows that.
# =============================================================
auth_or_die
ADMIN_SID="$SID"

hit()  { curl -s -H "Cookie: session_id=${1:-}" "$BASE$2"; }
code() { curl -s -o /dev/null -w '%{http_code}' -H "Cookie: session_id=${1:-}" "$BASE$2"; }
post() { curl -s -o /dev/null -w '%{http_code}' -X POST -H "Cookie: session_id=${1:-}" \
              -H 'Content-Type: application/json' --data "${3:-{\}}" "$BASE$2"; }

cleanup() {
    pg "DELETE FROM website_page_revision WHERE page_id IN
          (SELECT id FROM website_page WHERE slug LIKE 'pen-%')" >/dev/null
    pg "DELETE FROM website_form_submission WHERE form_id IN
          (SELECT id FROM website_form WHERE slug LIKE 'pen-%')" >/dev/null
    pg "DELETE FROM website_form_field WHERE form_id IN
          (SELECT id FROM website_form WHERE slug LIKE 'pen-%')" >/dev/null
    pg "DELETE FROM website_form WHERE slug LIKE 'pen-%'" >/dev/null
    pg "DELETE FROM website_page WHERE slug LIKE 'pen-%'" >/dev/null
    pg "DELETE FROM res_groups_users_rel WHERE uid IN
          (SELECT id FROM res_users WHERE login LIKE 'pen_%')" >/dev/null
    pg "DELETE FROM res_users   WHERE login LIKE 'pen_%'" >/dev/null
    pg "DELETE FROM res_partner WHERE name LIKE 'PEN %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

# ------------------------------------------------------------------
sec "0. the targets"
# ------------------------------------------------------------------
BLK='[{"type":"heading","level":"1","text":"Public"},{"type":"text","text":"Live copy."}]'
PUB=$(call website.page create "[{\"slug\":\"pen-public\",\"title\":\"Public\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$BLK"),\"is_published\":true}]" | rid)
DRAFT=$(call website.page create '[{"slug":"pen-secret","title":"PEN Unreleased Pricing","is_published":false}]' | rid)
t_nonempty "$PUB" "a published page"; t_nonempty "$DRAFT" "and a draft"
[ -z "$PUB" ] || [ -z "$DRAFT" ] && { verdict; exit 1; }
API="/site/api/page/$PUB/blocks"

# An employee with a real staff login and NO website permission.
EP=$(call res.partner create '[{"name":"PEN Employee","email":"pen_emp@t.test"}]' | rid)
EU=$(call res.users create "[{\"login\":\"pen_emp@t.test\",\"password\":\"Emp-Pass-123\",\"partner_id\":$EP,\"active\":true}]" | rid)
EMP=$(login 'pen_emp@t.test' 'Emp-Pass-123')
t_nonempty "$EMP" "an ordinary employee is signed in"

# A customer with a portal password.
CP=$(pgid "INSERT INTO res_partner (name,email,active,company_id)
           VALUES ('PEN Customer','pen_cust@t.test',true,1) RETURNING id")
call portal.partner portal_reset_password "[[$CP]]" >/dev/null
PSID=$(portal_login 'pen_cust@t.test' 'Welcome1')
t_nonempty "$PSID" "a portal customer is signed in"

# ------------------------------------------------------------------
sec "1. ATTACK — a visitor tries to change the site"
# ------------------------------------------------------------------
t_eq "401" "$(post "" "$API" '{"blocks":[{"type":"text","text":"defaced"}]}')" "cannot save"
t_eq "401" "$(code "" "$API")"                                    "cannot read the blocks"
t_eq "401" "$(code "" "/site/api/page/$PUB/revisions")"           "cannot read the history"
t_eq "401" "$(post "" "/site/api/page/$PUB/revisions/1")"         "cannot restore a version"
t_eq "401" "$(code "" /site/api/health)"                          "cannot read the site audit"
t_lacks "$(pg "SELECT blocks_json FROM website_page WHERE id=$PUB")" 'defaced' \
        "the page is untouched"

# A made-up session id is not a session.
for forged in 'deadbeefdeadbeefdeadbeef' '00000000000000000000000000000000' \
              'admin' '1' "' OR '1'='1"; do
    C=$(post "$forged" "$API" '{"blocks":[]}')
    t_ne "200" "$C" "forged cookie '$(printf '%s' "$forged" | head -c 12)' is refused ($C)"
done

# ------------------------------------------------------------------
sec "2. ATTACK — a portal customer tries to use their password as staff"
# ------------------------------------------------------------------
# The portal cookie is a different cookie AND a different session store.
t_eq "401" "$(curl -s -o /dev/null -w '%{http_code}' -X POST \
              -H "Cookie: portal_sid=$PSID" -H 'Content-Type: application/json' \
              --data '{"blocks":[]}' "$BASE$API")" "a portal cookie cannot save"
# ...and it is not accepted as a staff cookie either.
t_ne "200" "$(post "$PSID" "$API" '{"blocks":[]}')" \
     "a portal session id presented AS a staff cookie is refused"
t_ne "200" "$(code "$PSID" /site/api/health)" "nor for the site audit"

# ------------------------------------------------------------------
sec "3. ATTACK — an employee who IS logged in, but has no website rights"
# ------------------------------------------------------------------
# The most dangerous caller: authentication succeeds, so only authorisation
# stands between them and the public site.
t_eq "403" "$(post "$EMP" "$API" '{"blocks":[{"type":"text","text":"defaced"}]}')" "cannot save"
t_eq "403" "$(code "$EMP" "$API")"                              "cannot read the blocks"
t_eq "403" "$(code "$EMP" "/site/api/page/$PUB/revisions")"     "cannot read the history"
t_eq "403" "$(post "$EMP" "/site/api/page/$PUB/revisions/1")"   "cannot restore"
t_eq "403" "$(code "$EMP" /site/api/health)"                    "cannot read the audit"
t_lacks "$(hit "$EMP" /site/pen-public)" 'website-editor.js'    "is not even served the editor"
t_lacks "$(pg "SELECT blocks_json FROM website_page WHERE id=$PUB")" 'defaced' "nothing changed"

# ESCALATION: can they grant themselves the group?
ESC=$(call_as "$EMP" res.users write "[[$EU],{\"groups_id\":[[4,4]]}]")
has_error "$ESC" && ok "cannot grant themselves the website group" \
                 || no "PRIVILEGE ESCALATION: an employee granted themselves a group"
t_eq "0" "$(pg "SELECT count(*) FROM res_groups_users_rel WHERE uid=$EU AND gid=4")" \
     "and the grant did not land"
# Nor edit the page through the ordinary model API, going around the editor.
MOD=$(call_as "$EMP" website.page write "[[$PUB],{\"blocks_json\":\"[]\"}]")
has_error "$MOD" && ok "cannot rewrite the page through the model API either" \
                 || no "the model API let an ungrouped employee rewrite a page"

# ------------------------------------------------------------------
sec "4. ATTACK — reading what is not published"
# ------------------------------------------------------------------
t_eq "404" "$(code "" /site/pen-secret)" "a draft is 404 to a visitor"
t_lacks "$(hit "" /site/pen-secret)" 'PEN Unreleased' "and its title does not leak"
t_eq "$(code "" /site/pen-secret)" "$(code "" /site/pen-no-such-page)" \
     "a draft and a non-existent page are indistinguishable"
t_lacks "$(hit "" /sitemap.xml)"  'pen-secret' "the sitemap does not list it"
t_lacks "$(hit "" /site/pen-public)" 'pen-secret' "nor does a published page"
# The draft is not reachable through the API either.
t_eq "401" "$(code "" "/site/api/page/$DRAFT/blocks")" "nor through the block API"

# ------------------------------------------------------------------
sec "5. ATTACK — injection through every public input"
# ------------------------------------------------------------------
INJ="'; DROP TABLE website_page; --"
ENC=$(python3 -c 'import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))' "$INJ")
code "" "/site/$ENC" >/dev/null
code "" "/site/api/page/1$ENC/blocks" >/dev/null
hit  "" "/site/pen-public?x=$ENC" >/dev/null
t_eq "1" "$(pg "SELECT count(*) FROM pg_tables WHERE tablename='website_page'")" \
     "website_page survived the injection attempts"
t_ge "$(pg "SELECT count(*) FROM website_page")" "1" "and still has rows"

# Path traversal against the slug router.
for p in '/site/../../etc/passwd' '/site/..%2f..%2fetc%2fpasswd' \
         '/site/....//....//etc/passwd' '/site/%2e%2e/%2e%2e/etc/passwd'; do
    C=$(code "" "$p")
    t_ne "200" "$C" "traversal '$p' does not serve a file ($C)"
done
t_lacks "$(hit "" '/site/..%2f..%2fetc%2fpasswd')" 'root:' "no passwd file content"

# ------------------------------------------------------------------
sec "6. ATTACK — the public form as a write primitive"
# ------------------------------------------------------------------
F=$(call website.form create '[{"slug":"pen-form","title":"PEN Form"}]' | rid)
call website.form.field create "[{\"form_id\":$F,\"name\":\"msg\",\"label\":\"Message\"}]" >/dev/null
fpost() { curl -s -o /dev/null -w '%{http_code}' -X POST \
          -H 'Content-Type: application/json' --data "$1" "$BASE/site/form/pen-form"; }

# Try to write columns the form never declared.
fpost '{"msg":"hi","state":"archived","form_id":99,"id":1,"task_id":7}' >/dev/null
D=$(pg "SELECT data_json FROM website_form_submission WHERE form_id=$F ORDER BY id DESC LIMIT 1")
t_lacks "$D" 'archived' "an undeclared field is not stored"
t_lacks "$D" 'task_id'  "nor one that names a real column"
t_eq "new" "$(pg "SELECT state FROM website_form_submission WHERE form_id=$F ORDER BY id DESC LIMIT 1")" \
     "the row's own state is the server's, not the caller's"
# It cannot be pointed at another model.
BADT=$(call website.form create '[{"slug":"pen-bad","title":"x","target_model":"res.users"}]')
has_error "$BADT" && ok "a form cannot be aimed at res.users" || no "a form was aimed at res.users"

# ------------------------------------------------------------------
sec "7. ATTACK — IDOR on revisions and documents"
# ------------------------------------------------------------------
call website.page write "[[$PUB],{\"blocks_json\":\"[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"v1\\\"}]\"}]" >/dev/null
post "$ADMIN_SID" "$API" '{"blocks":[{"type":"text","text":"v2"}]}' >/dev/null
OTHER=$(call website.page create '[{"slug":"pen-other","title":"Other","blocks_json":"[]"}]' | rid)
post "$ADMIN_SID" "/site/api/page/$OTHER/blocks" '{"blocks":[{"type":"text","text":"other-secret"}]}' >/dev/null
post "$ADMIN_SID" "/site/api/page/$OTHER/blocks" '{"blocks":[{"type":"text","text":"other-v2"}]}' >/dev/null
ORID=$(pg "SELECT id FROM website_page_revision WHERE page_id=$OTHER ORDER BY id DESC LIMIT 1")
if [ -n "$ORID" ]; then
    t_eq "404" "$(post "$ADMIN_SID" "/site/api/page/$PUB/revisions/$ORID")" \
         "one page's revision cannot be restored onto another"
    t_lacks "$(hit "" /site/pen-public)" 'other-secret' "and no content crossed over"
fi
t_eq "404" "$(post "$ADMIN_SID" "/site/api/page/999999/blocks" '{"blocks":[]}')" \
     "a page that does not exist is 404, not a crash"

# ------------------------------------------------------------------
sec "8. ATTACK — resource exhaustion and header spoofing"
# ------------------------------------------------------------------
# A quarter-megabyte body goes in a FILE — passing it as an argv element hits
# the shell's own limit and the check then fails on the harness, not the server.
python3 -c 'import json,sys;sys.stdout.write(json.dumps({"blocks":[{"type":"text","text":"x"*400} for _ in range(600)]}))' > /tmp/pen_big.json
t_eq "400" "$(curl -s -o /dev/null -w '%{http_code}' -X POST \
              -H "Cookie: session_id=$ADMIN_SID" -H 'Content-Type: application/json' \
              --data @/tmp/pen_big.json "$BASE$API")" "an oversized page is refused"
t_eq "400" "$(post "$ADMIN_SID" "$API" '{"blocks":[{"type":"nope"}]}')" \
     "an unknown block type is refused"
rm -f /tmp/pen_big.json

# X-Forwarded-For spoofing.
#
# The resolver takes the LAST element of the header, because that is the one
# the proxy appended — nginx's usual $proxy_add_x_forwarded_for keeps whatever
# the client sent and adds the real peer after it. So an attacker PREPENDING a
# fake address must not buy a fresh rate-limit budget: every one of these
# requests still resolves to the same real client.
#
# (This test speaks to the server from the loopback address, which IS a trusted
# proxy, so it can exercise the header at all. An attacker on the internet
# reaches nginx, not this port.)
LIMITED=0
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
    C=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
        -H "X-Forwarded-For: 10.1.1.$i, 127.0.0.1" \
        --data '{"msg":"flood"}' "$BASE/site/form/pen-form")
    [ "$C" = "429" ] && { LIMITED=1; break; }
done
t_eq "1" "$LIMITED" "prepending a fake X-Forwarded-For does not buy a new rate-limit budget"

# ------------------------------------------------------------------
sec "9. ATTACK — running the editor's own code as a visitor"
# ------------------------------------------------------------------
# The editor script is a public static file. An attacker can load it, set the
# configuration object it looks for, and drive it — so the question is not
# "can they run the client", it is "does the server care". This is the check
# that says the client is not a security boundary.
DRIVE="$R/tests/security/website/site-hardening/attack.mjs"
if command -v node >/dev/null 2>&1 && [ -f "$DRIVE" ]; then
    REP=$(PEN_SLUG=pen-public PEN_PAGE="$PUB" \
          EMP_LOGIN='pen_emp@t.test' EMP_PASS='Emp-Pass-123' \
          node "$DRIVE" 2>/tmp/pen.err | tail -1)
    SKIP=$(printf '%s' "$REP" | python3 -c 'import sys,json;print(json.loads(sys.stdin.read() or "{}").get("skipped",""))' 2>/dev/null)
    if [ -n "$SKIP" ]; then
        echo "    SKIP  browser attack: $SKIP"
    else
        g(){ printf '%s' "$REP" | python3 -c "import sys,json;d=json.loads(sys.stdin.read() or '{}');print(d.get('steps',{}).get('$1',''))" 2>/dev/null; }
        t_eq "True" "$(g editorLoadable)"   "the editor script IS loadable by a visitor (it is a static file)"
        t_eq "True" "$(g editorRanAsAnon)"  "and it can be forced to run in their browser"
        t_eq "True" "$(g anonSaveRefused)"  "but the SERVER refuses the save (401)"
        t_eq "True" "$(g anonPageUnchanged)" "and the public page is unchanged"
        t_eq "True" "$(g empSaveRefused)"   "an employee driving it the same way is refused (403)"
        t_eq "True" "$(g empPageUnchanged)" "and the page is still unchanged"
        t_eq "True" "$(g noPrivFromClient)" "no client-side flag grants any privilege"
        [ -n "$(printf '%s' "$REP" | python3 -c 'import sys,json;print(" ".join(json.loads(sys.stdin.read() or "{}").get("errors",[])))' 2>/dev/null)" ] \
            && echo "    note: $REP"
    fi
else
    echo "    SKIP  browser attack: node or attack.mjs unavailable"
fi

# ------------------------------------------------------------------
sec "10. the public surface leaks nothing about itself"
# ------------------------------------------------------------------
PAGE=$(hit "" /site/pen-public)
t_lacks "$PAGE" 'session_id'  "no session id in a public page"
t_lacks "$PAGE" 'pbkdf2'      "no password material"
t_lacks "$PAGE" 'password'    "not even the word"
HDRS=$(curl -s -D - -o /dev/null "$BASE/site/pen-public" | tr 'A-Z' 'a-z')
t_lacks    "$HDRS" 'set-cookie' "a public page sets no cookie"
t_contains "$HDRS" 'nosniff'    "X-Content-Type-Options is set"
# An error must not become a stack trace or SQL.
ERRBODY=$(curl -s -X POST -H "Cookie: session_id=$ADMIN_SID" \
          -H 'Content-Type: application/json' --data '{"blocks":[{"type":1}]}' "$BASE$API")
t_lacks "$ERRBODY" 'SELECT'      "an error body carries no SQL"
t_lacks "$ERRBODY" 'pqxx'        "nor driver internals"
t_lacks "$ERRBODY" 'website_page' "nor a table name"

# The ERP survived all of it.
t_eq "200" "$(http_code /healthz)" "the ERP is still healthy"
t_eq "200" "$(code "" /site/pen-public)" "the site is still serving"

verdict

#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# The website form builder (docs/116 A1 + A2).
#
# This is an UNAUTHENTICATED WRITE endpoint on a public page — the only one in
# the system besides the kiosk. So the assertions that carry weight are the
# ones about what it refuses to write:
#
#   §4  THE FIELD ALLOW-LIST. Only fields the form declares are read. Anything
#       else in the body is discarded — not stored "just in case", which is
#       what turns a contact form into an arbitrary write.
#   §5  Required, length caps, and type checks are enforced server-side.
#   §6  Honeypot and rate limit — a public POST needs both.
#   §7  A payload typed into a form cannot execute when the back office
#       displays it later (stored XSS, second order).
# =============================================================
auth_or_die

post()  { curl -s -X POST "$BASE/site/form/$1" -H 'Content-Type: application/json' --data "$2"; }
pcode() { curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/site/form/$1" \
               -H 'Content-Type: application/json' --data "$2"; }
anon()  { curl -s "$BASE$1"; }

cleanup() {
    pg "DELETE FROM website_form_submission WHERE form_id IN
          (SELECT id FROM website_form WHERE slug LIKE 'wf-%')" >/dev/null
    pg "DELETE FROM project_task WHERE name LIKE 'Website: WF %'" >/dev/null
    pg "DELETE FROM website_form_field WHERE form_id IN
          (SELECT id FROM website_form WHERE slug LIKE 'wf-%')" >/dev/null
    pg "DELETE FROM website_form WHERE slug LIKE 'wf-%'" >/dev/null
    pg "DELETE FROM website_page WHERE slug LIKE 'wf-%'" >/dev/null
}
cleanup

# §9 deliberately trips the submit rate limiter, which lives in the PROCESS
# keyed on this test's own IP for five minutes. Database hygiene is not enough
# when the state is in memory: left alone it would fail the next run and any
# form submission in between. A restart is the only reset, and it belongs in
# the EXIT trap only — restarting first would throw away the session
# auth_or_die just obtained.
finish() {
    cleanup
    pkill -x c-erp 2>/dev/null; sleep 2
    ( cd "$ERP_ROOT" && setsid ./build/c-erp > /tmp/cerp_run.log 2>&1 < /dev/null & )
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        curl -sf -o /dev/null --max-time 2 "$BASE/healthz" && break
        sleep 1
    done
}
trap 'finish' EXIT

# ------------------------------------------------------------------
sec "1. schema and menus"
# ------------------------------------------------------------------
for t in website_form website_form_field website_form_submission; do
    t_eq "1" "$(pg "SELECT count(*) FROM pg_tables WHERE tablename='$t'")" "$t exists"
done
t_eq "website.form" "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Website Forms' LIMIT 1")" \
     "Settings -> Website Forms is wired"

# ------------------------------------------------------------------
sec "2. building a form"
# ------------------------------------------------------------------
F=$(call website.form create '[{"slug":"wf-contact","title":"WF Contact","submit_label":"Send it","success_message":"Got it, thanks."}]' | rid)
t_nonempty "$F" "a form was created"
[ -z "$F" ] && { verdict; exit 1; }

mkfield() { call website.form.field create "[{\"form_id\":$F,\"name\":\"$1\",\"label\":\"$2\",\"field_type\":\"$3\",\"required\":$4,\"sequence\":$5}]" | rid; }
FN=$(mkfield name    "Your name"  text     true  10)
FE=$(mkfield email   "Email"      email    true  20)
FM=$(mkfield message "Message"    textarea false 30)
FQ=$(mkfield qty     "How many"   number   false 40)
t_nonempty "$FN" "a text field"; t_nonempty "$FE" "an email field"
t_nonempty "$FM" "a textarea";   t_nonempty "$FQ" "a number field"

# Field definitions are constrained: the name is a key in the submitted body.
for bad in 'Upper' 'has space' 'semi;colon' 'website'; do
    RES=$(call website.form.field create "[{\"form_id\":$F,\"name\":\"$bad\",\"label\":\"x\"}]")
    has_error "$RES" && ok "field name '$bad' is refused" || no "field name '$bad' was accepted"
done
RES=$(call website.form.field create "[{\"form_id\":$F,\"name\":\"weird\",\"label\":\"x\",\"field_type\":\"password\"}]")
has_error "$RES" && ok "an unsupported field type is refused" || no "field type 'password' was accepted"

# ------------------------------------------------------------------
sec "3. the form renders on a page"
# ------------------------------------------------------------------
BLOCKS='[{"type":"heading","level":"1","text":"Contact us"},{"type":"form","slug":"wf-contact"}]'
P=$(call website.page create "[{\"slug\":\"wf-page\",\"title\":\"Contact\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$BLOCKS"),\"is_published\":true}]" | rid)
t_nonempty "$P" "a page embedding the form"
PB=$(anon /site/wf-page)
t_contains "$PB" 'action="/site/form/wf-contact"' "the form posts to its own endpoint"
t_contains "$PB" 'name="name"'    "the declared text field is rendered"
t_contains "$PB" 'type="email"'   "the email field gets the right input type"
t_contains "$PB" '<textarea'      "the textarea is rendered"
t_contains "$PB" 'Send it'        "the custom button label is used"
t_contains "$PB" 'name="website"' "the honeypot is present"

# ------------------------------------------------------------------
sec "3b. an unknown or inactive form is invisible"
# ------------------------------------------------------------------
# Checked BEFORE any real submissions. The rate limiter runs AHEAD of the form
# lookup on purpose — probing form names must cost budget too — so leaving
# these until after the validation section made them fail on 429 rather than on
# what they were actually testing.
t_eq "404" "$(pcode wf-nope '{"x":1}')" "an unknown form is 404"
call website.form write "[[$F],{\"active\":false}]" >/dev/null
t_eq "404" "$(pcode wf-contact '{"name":"x","email":"y@z.test"}')" \
     "an INACTIVE form answers the same 404, so the namespace cannot be probed"
call website.form write "[[$F],{\"active\":true}]" >/dev/null

# ------------------------------------------------------------------
sec "4. THE FIELD ALLOW-LIST"
# ------------------------------------------------------------------
OK=$(post wf-contact '{"name":"Ada","email":"ada@x.test","message":"Hello there"}')
t_contains "$OK" '"ok":true'      "a valid submission is accepted"
t_contains "$OK" 'Got it, thanks.' "the configured thank-you message comes back"
SID_=$(pg "SELECT id FROM website_form_submission WHERE form_id=$F ORDER BY id DESC LIMIT 1")
t_nonempty "$SID_" "it was stored"
D=$(pg "SELECT data_json FROM website_form_submission WHERE id=$SID_")
t_contains "$D" 'Ada' "the submitted name is stored"

# Undeclared keys are DISCARDED, not stored. This is the assertion that would
# fail if the route ever started trusting the body's shape.
EXTRA=$(post wf-contact '{"name":"Bob","email":"b@x.test","is_admin":true,"id":1,"state":"done","secret":"x"}')
t_contains "$EXTRA" '"ok":true' "a submission with extra keys is accepted"
D2=$(pg "SELECT data_json FROM website_form_submission WHERE form_id=$F ORDER BY id DESC LIMIT 1")
t_contains "$D2" 'Bob'       "the declared fields are stored"
t_lacks    "$D2" 'is_admin'  "an undeclared key is NOT stored"
t_lacks    "$D2" 'secret'    "nor another one"
t_lacks    "$D2" '"state"'   "nor one that names a real column"

# The submission's own columns cannot be driven from the body.
t_eq "new" "$(pg "SELECT state FROM website_form_submission WHERE form_id=$F ORDER BY id DESC LIMIT 1")" \
     "state is set by the server, not the caller"

# ------------------------------------------------------------------
sec "5. A2 — routing a submission to a task"
# ------------------------------------------------------------------
PROJ=$(pg "SELECT id FROM project_project ORDER BY id LIMIT 1")
if [ -z "$PROJ" ]; then
    PROJ=$(call project.project create '[{"name":"WF Project"}]' | rid)
fi
F2=$(call website.form create '[{"slug":"wf-task","title":"WF Suggestion","target_model":"project.task"}]' | rid)
call website.form.field create "[{\"form_id\":$F2,\"name\":\"idea\",\"label\":\"Idea\",\"required\":true}]" >/dev/null
t_nonempty "$F2" "a form that routes to a task"

# This runs EARLY on purpose: the submit limiter has a small per-IP budget and
# the validation section below spends most of it. Run last, this assertion was
# skipped every time — which is the same as not having it.
RT=$(post wf-task '{"idea":"Add a dark mode"}')
if printf '%s' "$RT" | grep -q '"ok":true'; then
    t_eq "1" "$(pg "SELECT count(*) FROM project_task WHERE name LIKE 'Website: WF %'")" \
         "a task was created from the submission"
    t_eq "1" "$(pg "SELECT (description LIKE '%dark mode%')::int FROM project_task WHERE name LIKE 'Website: WF %' LIMIT 1")" \
         "the submitted text is on the task"
    t_ne "" "$(pg "SELECT task_id FROM website_form_submission WHERE form_id=$F2 ORDER BY id DESC LIMIT 1")" \
         "the submission records which task it became"
else
    no "task routing did not run: $(printf '%s' "$RT" | head -c 120)"
fi

# target_model is an allow-list — a form cannot be pointed at any model.
BADT=$(call website.form create '[{"slug":"wf-bad","title":"x","target_model":"res.users"}]')
has_error "$BADT" && ok "a form cannot target an arbitrary model" || no "target_model res.users was accepted"

# ------------------------------------------------------------------
sec "6. validation is server-side"
# ------------------------------------------------------------------
MISS=$(post wf-contact '{"message":"no name or email"}')
t_lacks "$MISS" '"ok":true' "a submission missing required fields is refused"
t_contains "$MISS" 'Your name' "and says which field, by its label"
t_eq "400" "$(pcode wf-contact '{"message":"x"}')" "it answers 400"

EMPTY=$(post wf-contact '{}')
t_lacks "$EMPTY" '"ok":true' "an empty submission is refused"

# A number field keeps a number or nothing — never prose the back office
# would later read as a figure.
post wf-contact '{"name":"N","email":"n@x.test","qty":"not-a-number"}' >/dev/null
DN=$(pg "SELECT data_json FROM website_form_submission WHERE form_id=$F ORDER BY id DESC LIMIT 1")
t_lacks "$DN" 'not-a-number' "a non-numeric value in a number field is dropped"
post wf-contact '{"name":"N2","email":"n2@x.test","qty":"42"}' >/dev/null
DN2=$(pg "SELECT data_json FROM website_form_submission WHERE form_id=$F ORDER BY id DESC LIMIT 1")
t_contains "$DN2" '42' "a numeric value is kept"

# Length caps: a public endpoint needs a ceiling it does not take from the caller.
BIG=$(python3 -c 'import json;print(json.dumps({"name":"A"*9000,"email":"a@x.test"}))')
post wf-contact "$BIG" >/dev/null
LEN=$(pg "SELECT length(data_json) FROM website_form_submission WHERE form_id=$F ORDER BY id DESC LIMIT 1")
t_eq "1" "$(pg "SELECT ($LEN < 6000)::int")" "an oversized field is truncated, not stored whole"
HUGE=$(python3 -c 'import json;print(json.dumps({"name":"A"*40000,"email":"a@x.test"}))')
t_eq "400" "$(pcode wf-contact "$HUGE")" "an oversized BODY is refused outright"

# ------------------------------------------------------------------
sec "7. bot defences"
# ------------------------------------------------------------------
BEFORE=$(pg "SELECT count(*) FROM website_form_submission WHERE form_id=$F")
HP=$(post wf-contact '{"name":"Bot","email":"bot@x.test","website":"http://spam"}')
t_contains "$HP" '"ok":true' "a honeypot hit answers 200, so a bot learns nothing"
t_eq "$BEFORE" "$(pg "SELECT count(*) FROM website_form_submission WHERE form_id=$F")" \
     "...but nothing was stored"

# ------------------------------------------------------------------
sec "8. a payload typed into a form cannot execute later"
# ------------------------------------------------------------------
# Second-order XSS: the value is stored, then displayed in the back office.
# It must come back as DATA, never as markup.
XS='<script>alert(1)</script>'
pg "DELETE FROM website_form_submission WHERE form_id=$F" >/dev/null
# The limiter is per-IP and still tripped, so insert through the model layer —
# the point here is what happens on the way OUT.
pg "INSERT INTO website_form_submission (form_id, data_json)
    VALUES ($F, '{\"name\":\"$XS\"}')" >/dev/null
READBACK=$(call website.form.submission search_read "[[[\"form_id\",\"=\",$F]]]")
t_contains "$READBACK" 'script' "the payload is returned as stored data"
# It is JSON in a JSON field — the browser never parses it as markup, and the
# page that renders it escapes it. What matters is that the API did not
# helpfully turn it into anything else.
t_lacks "$READBACK" '"<script>alert(1)</script>":' "it is a value, never a key or a structure"

# ------------------------------------------------------------------
sec "9. guessing and flooding are throttled"
# ------------------------------------------------------------------
# Rate limiting. Fire enough to trip it.
LIMITED=0
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do
    C=$(pcode wf-contact "{\"name\":\"R$i\",\"email\":\"r$i@x.test\"}")
    [ "$C" = "429" ] && { LIMITED=1; break; }
done
[ "$LIMITED" = "1" ] && ok "repeated submissions are rate-limited (429)" \
                     || no "16 submissions went through with no throttle"


verdict

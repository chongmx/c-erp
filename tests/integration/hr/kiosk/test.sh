#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# The staff kiosk (docs/113 §3a).
#
# A tablet in a public place, so the security model IS the feature and most of
# these assertions are refusals:
#
#   * the page carries no session and the punch route issues none. A stolen
#     kiosk must be worth one person's clock, not the company's data — §6
#     checks the response sets no cookie and the ERP stays shut.
#   * the PIN is stored hashed and never leaves the server, in any shape.
#   * the kiosk cannot enumerate employees; it answers about exactly the one
#     person whose PIN was entered.
#   * a wrong PIN is refused, and repeated wrong PINs are rate-limited —
#     the one place an attacker can stand and guess 4 digits all day.
# =============================================================
auth_or_die

PIN='481907'
PIN2='250413'

kpunch() { curl -s -X POST "$BASE/kiosk/api/punch" -H 'Content-Type: application/json' \
                --data "{\"pin\":\"$1\"}"; }
kcode()  { curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/kiosk/api/punch" \
                -H 'Content-Type: application/json' --data "{\"pin\":\"$1\"}"; }

cleanup_rows() {
    pg "DELETE FROM hr_attendance WHERE employee_id IN
          (SELECT id FROM hr_employee WHERE name LIKE 'KSK %')" >/dev/null
    pg "DELETE FROM hr_employee WHERE name LIKE 'KSK %'" >/dev/null
}

# §9 deliberately trips the punch rate limiter, which lives in the PROCESS and
# is keyed on this test's own IP for three minutes. Database hygiene is not
# enough when the state is in memory: left alone it would fail the next run of
# this test and any kiosk call in between. A restart is the only reset — but it
# belongs in the EXIT trap only. Restarting before the test would throw away the
# session auth_or_die just obtained and every call after it would fail for the
# wrong reason.
finish() {
    cleanup_rows
    pkill -x c-erp 2>/dev/null; sleep 2
    ( cd "$ERP_ROOT" && setsid ./build/c-erp > /tmp/cerp_run.log 2>&1 < /dev/null & )
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        curl -sf -o /dev/null --max-time 2 "$BASE/healthz" && break
        sleep 1
    done
}
cleanup_rows
trap 'finish' EXIT

# ------------------------------------------------------------------
sec "1. the page is served, and is not the ERP"
# ------------------------------------------------------------------
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/kiosk")" "GET /kiosk is served"
PAGE=$(curl -s "$BASE/kiosk")
t_contains "$PAGE" 'Clock In' "it is the kiosk page"
# It must not ship the ERP application, or the tablet becomes a back door.
t_lacks "$PAGE" 'app.js'   "the kiosk does not load the ERP frontend"
t_lacks "$PAGE" 'session_id' "nor anything about sessions"
# Serving the page must not hand out a cookie of any kind.
HDRS=$(curl -s -D - -o /dev/null "$BASE/kiosk")
t_lacks "$HDRS" 'Set-Cookie' "GET /kiosk sets no cookie"

# ------------------------------------------------------------------
sec "2. an employee with a PIN"
# ------------------------------------------------------------------
EMP=$(call hr.employee create '[{"name":"KSK Puncher"}]' | rid)
t_nonempty "$EMP" "employee created"
[ -z "$EMP" ] && { verdict; exit 1; }

t_eq "0" "$(pg "SELECT (pin_hash IS NOT NULL)::int FROM hr_employee WHERE id=$EMP")" \
     "a new employee has no PIN"
R=$(call_k hr.employee set_pin "[[$EMP]]" "\"pin\":\"$PIN\"")
has_error "$R" && no "set_pin failed: $(echo "$R" | head -c 160)"
t_eq "1" "$(pg "SELECT (pin_hash IS NOT NULL)::int FROM hr_employee WHERE id=$EMP")" "the PIN is set"

# Stored hashed, never in clear — the single most consequential check here.
t_eq "0" "$(pg "SELECT (pin_hash = '$PIN')::int FROM hr_employee WHERE id=$EMP")" \
     "the PIN is stored hashed, not in clear"
t_eq "1" "$(pg "SELECT (pin_hash LIKE '\$pbkdf2%')::int FROM hr_employee WHERE id=$EMP")" \
     "it is a PBKDF2 hash"

# And it never comes back out through the ordinary read path.
READ=$(call hr.employee read "[[$EMP]]")
t_lacks "$READ" "$PIN"      "reading the employee does not return the PIN"
t_lacks "$READ" 'pin_hash'  "nor the hash"
SR=$(call hr.employee search_read "[[[\"id\",\"=\",$EMP]]]")
t_lacks "$SR" 'pin_hash' "nor does search_read"

# ------------------------------------------------------------------
sec "3. weak PINs are refused"
# ------------------------------------------------------------------
for bad in '123' '' 'abcd' '12a4'; do
    R=$(call_k hr.employee set_pin "[[$EMP]]" "\"pin\":\"$bad\"")
    has_error "$R" && ok "PIN '$bad' is refused" || no "PIN '$bad' was accepted"
done
t_eq "1" "$(pg "SELECT (pin_hash IS NOT NULL)::int FROM hr_employee WHERE id=$EMP")" \
     "the existing PIN survived the rejected attempts"

# ------------------------------------------------------------------
sec "4. punching the clock"
# ------------------------------------------------------------------
P1=$(kpunch "$PIN")
t_contains "$P1" '"ok":true'       "a valid PIN is accepted"
t_contains "$P1" '"checked_in"'    "it checks the employee in"
t_contains "$P1" 'KSK Puncher'     "and greets them by name"
t_eq "1" "$(pg "SELECT count(*) FROM hr_attendance WHERE employee_id=$EMP AND check_out IS NULL")" \
     "one open attendance exists"

P2=$(kpunch "$PIN")
t_contains "$P2" '"checked_out"'   "punching again checks them out"
t_eq "0" "$(pg "SELECT count(*) FROM hr_attendance WHERE employee_id=$EMP AND check_out IS NULL")" \
     "nothing is left open"
t_contains "$P2" 'worked_hours_today' "it reports the hours worked today"

# The kiosk goes through the same attendance table the back office reads.
t_ge "$(pg "SELECT count(*) FROM hr_attendance WHERE employee_id=$EMP")" "1" \
     "the punches are ordinary attendance records"

# ------------------------------------------------------------------
sec "5. a wrong PIN gets nothing, and leaks nothing"
# ------------------------------------------------------------------
BAD=$(kpunch '000000')
t_eq "401" "$(kcode '000000')" "an unknown PIN is refused"
t_lacks "$BAD" 'KSK Puncher' "the refusal names nobody"
t_lacks "$BAD" '"ok":true'   "and grants nothing"
# A short PIN is refused before any lookup happens.
t_eq "401" "$(kcode '12')" "a too-short PIN is refused"

# The kiosk must not be able to enumerate staff. There is no route that lists
# employees, and a punch answers only about the PIN's own owner.
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/kiosk/api/employees")" \
     "there is no employee-listing route on the kiosk"

# ------------------------------------------------------------------
sec "6. the kiosk is not a session"
# ------------------------------------------------------------------
# The whole security argument: a successful punch must NOT hand back anything
# that opens the ERP. If it did, the tablet by the door would be a login.
PH=$(curl -s -D - -o /dev/null -X POST "$BASE/kiosk/api/punch" \
       -H 'Content-Type: application/json' --data "{\"pin\":\"$PIN\"}" \
     | tr 'A-Z' 'a-z')   # header names are case-insensitive; compare lowercased
t_lacks "$PH" 'set-cookie' "a successful punch sets no cookie"
kpunch "$PIN" >/dev/null   # leave them checked out again

# And the security headers are on, like every other public surface here.
t_contains "$PH" 'x-frame-options' "the response carries X-Frame-Options"
t_contains "$PH" 'nosniff'         "and X-Content-Type-Options"
# One Content-Type, not two. addHeader() appends, so the JSON routes here
# used to answer with drogon's default text/html AND application/json; a
# client honouring the first renders the payload as markup.
t_eq "1" "$(printf '%s\n' "$PH" | grep -c '^content-type:')" "exactly one Content-Type header"
t_contains "$PH" 'application/json' "and it is application/json"

# ------------------------------------------------------------------
sec "7. two employees, two PINs, no confusion"
# ------------------------------------------------------------------
# The punch route identifies a person BY VERIFYING the PIN against every
# candidate, so this is the check that it lands on the right one.
EMP2=$(call hr.employee create '[{"name":"KSK Second"}]' | rid)
call_k hr.employee set_pin "[[$EMP2]]" "\"pin\":\"$PIN2\"" >/dev/null
A=$(kpunch "$PIN2")
t_contains "$A" 'KSK Second' "the second PIN identifies the second employee"
t_eq "1" "$(pg "SELECT count(*) FROM hr_attendance WHERE employee_id=$EMP2 AND check_out IS NULL")" \
     "it clocked the right person"
t_eq "0" "$(pg "SELECT count(*) FROM hr_attendance WHERE employee_id=$EMP AND check_out IS NULL")" \
     "and left the first employee alone"
kpunch "$PIN2" >/dev/null

# ------------------------------------------------------------------
sec "8. clearing a PIN closes the door"
# ------------------------------------------------------------------
R=$(call hr.employee clear_pin "[[$EMP2]]")
has_error "$R" && no "clear_pin failed"
t_eq "0" "$(pg "SELECT (pin_hash IS NOT NULL)::int FROM hr_employee WHERE id=$EMP2")" "the PIN is gone"
t_eq "401" "$(kcode "$PIN2")" "the cleared PIN no longer works"
t_eq "200" "$(kcode "$PIN")"  "the other employee is unaffected"
kpunch "$PIN" >/dev/null

# An archived employee cannot punch either — the punch query requires active.
call hr.employee write "[[$EMP],{\"active\":false}]" >/dev/null
t_eq "401" "$(kcode "$PIN")" "an archived employee cannot punch"
call hr.employee write "[[$EMP],{\"active\":true}]" >/dev/null

# ------------------------------------------------------------------
sec "9. guessing is rate-limited"
# ------------------------------------------------------------------
# 4-digit PINs are only defensible because guessing is throttled. Fire enough
# wrong PINs to trip the limiter and assert it actually trips.
LIMITED=0
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    C=$(kcode "0000$i")
    [ "$C" = "429" ] && { LIMITED=1; break; }
done
[ "$LIMITED" = "1" ] && ok "repeated wrong PINs are rate-limited (429)" \
                     || no "the kiosk accepted 12 wrong PINs without throttling"

verdict

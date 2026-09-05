#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# /rental/calendar — the day arithmetic behind the booking strips.
#
# The functional test drives the screen and proves a booking can be made and
# a double-let cannot. It cannot economically prove the MATHS, because every
# case worth checking is a date-boundary case and clicking each one through a
# browser would take minutes:
#
#   * a let that starts before the month shown        -> clamped to day 1
#   * a let that ends after it                        -> clamped to the last day
#   * an open-ended let                               -> runs to the month end
#   * a let entirely outside the month                -> contributes nothing
#   * two lets on one unit                            -> the union, not a sum
#   * a retired unit                                  -> not lettable stock
#
# Clamping is where this kind of code goes wrong, and it goes wrong quietly:
# an off-by-one at a month edge shows as a strip that looks plausible and an
# occupancy figure that is subtly false. Each case below is one unit, so a
# failure names the case rather than a total.
#
# Occupancy is DERIVED, never stored (RentalCalendar.cpp), so these assertions
# are also the check that the calendar and the billing run read the same dates.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZOC'
MONTH='2026-04'            # 30 days, fixed, so the expected numbers are exact
cleanup() {
    pg "DELETE FROM rental_contract_line WHERE unit_id IN
          (SELECT id FROM rental_unit WHERE code LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_event WHERE unit_id IN
          (SELECT id FROM rental_unit WHERE code LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_unit WHERE code LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM rental_unit_type WHERE code LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

# letdays <unit code> — the let-day count the endpoint reports for one unit.
letdays() {
    http_get "/rental/calendar?month=${MONTH}" | python3 -c "
import sys, json
code = sys.argv[1]
d = json.load(sys.stdin)
for u in d['units']:
    if u['code'] == code:
        print(u['let_days']); break
else:
    print('MISSING')
" "$1"
}

# strip <unit code> — the day bitmap as dots and hashes, for a readable diff.
strip() {
    http_get "/rental/calendar?month=${MONTH}" | python3 -c "
import sys, json
code = sys.argv[1]
d = json.load(sys.stdin)
for u in d['units']:
    if u['code'] == code:
        print(''.join('#' if x else '.' for x in u['days'])); break
else:
    print('MISSING')
" "$1"
}

# -------------------------------------------------------------------------
sec "1. fixtures — one unit per boundary case"
# -------------------------------------------------------------------------
TY=$(call rental.unit.type create "[{\"name\":\"${PFX} Bay\",\"code\":\"${PFX}B\"}]" | rid)
t_nonempty "$TY" "a unit type for the aggregate check"
PA=$(call res.partner create "[{\"name\":\"${PFX} Holder\"}]" | rid)
t_nonempty "$PA" "a customer to hold the lets"
CT=$(call rental.contract create \
     "[{\"name\":\"${PFX}-CT\",\"partner_id\":$PA,\"date_start\":\"${MONTH}-01\"}]" | rid)
t_nonempty "$CT" "a contract to hang the lines on"

mkunit() {   # mkunit <suffix>  -> id
    call rental.unit create "[{\"code\":\"${PFX}-$1\",\"name\":\"${PFX} $1\",\"type_id\":$TY}]" | rid
}
# The lines are inserted directly: this test is about the READ path, and
# writing them through the booking endpoint would make a failure ambiguous
# between the two. The booking endpoint has its own test.
mkline() {   # mkline <unit id> <start> <end|NULL>
    local end="$3"
    if [ "$end" = "NULL" ]; then end="NULL"; else end="'$end'"; fi
    pg "INSERT INTO rental_contract_line
          (contract_id, unit_id, partner_id, date_start, date_end, state, billing_mode)
        VALUES ($CT, $1, $PA, '$2', $end, 'active', 'oneoff')" >/dev/null 2>&1
}

U_IN=$(mkunit IN);       mkline "$U_IN"  "${MONTH}-10" "${MONTH}-14"
U_PRE=$(mkunit PRE);     mkline "$U_PRE" "2026-03-25"  "${MONTH}-03"
U_POST=$(mkunit POST);   mkline "$U_POST" "${MONTH}-28" "2026-05-06"
U_OPEN=$(mkunit OPEN);   mkline "$U_OPEN" "${MONTH}-25" "NULL"
U_OUT=$(mkunit OUT);     mkline "$U_OUT" "2026-01-05"  "2026-01-09"
U_TWO=$(mkunit TWO);     mkline "$U_TWO" "${MONTH}-02" "${MONTH}-04"
                         mkline "$U_TWO" "${MONTH}-06" "${MONTH}-08"
U_FREE=$(mkunit FREE)
t_nonempty "$U_FREE" "seven units, one per case"

# -------------------------------------------------------------------------
sec "2. a let inside the month"
# -------------------------------------------------------------------------
t_eq "5" "$(letdays "${PFX}-IN")" "10th to 14th inclusive is five days"
t_eq ".........#####................" "$(strip "${PFX}-IN")" \
     "and it lands on the right days"

# -------------------------------------------------------------------------
sec "3. clamping at both edges"
# -------------------------------------------------------------------------
# The case that breaks first: a tenancy that began last month must fill from
# day 1, not from the day-of-month its start happens to name.
t_eq "3" "$(letdays "${PFX}-PRE")" "a let starting before the month is clamped to day 1"
t_eq "###..........................." "$(strip "${PFX}-PRE")" "…and stops on the 3rd"

t_eq "3" "$(letdays "${PFX}-POST")" "a let ending after the month is clamped to the last day"
t_eq "...........................###" "$(strip "${PFX}-POST")" "…and starts on the 28th"

# -------------------------------------------------------------------------
sec "4. open-ended lets run to the end of the month"
# -------------------------------------------------------------------------
# "Rent until termination" is date_end IS NULL — the common case for a real
# tenancy, and the one an occupancy report must not treat as a single day.
t_eq "6" "$(letdays "${PFX}-OPEN")" "an open-ended let fills to the month end"
t_eq "........................######" "$(strip "${PFX}-OPEN")" "…from the 25th"

# -------------------------------------------------------------------------
sec "5. lets outside the month contribute nothing"
# -------------------------------------------------------------------------
t_eq "0" "$(letdays "${PFX}-OUT")" "a January let adds nothing to April"
t_eq "0" "$(letdays "${PFX}-FREE")" "a unit with no lines at all reads zero"
t_contains "$(http_get "/rental/calendar?month=${MONTH}")" "${PFX}-FREE" \
     "and it is still LISTED — an empty unit is the one you want to see"

# -------------------------------------------------------------------------
sec "6. two lets on one unit are a union, not a sum"
# -------------------------------------------------------------------------
t_eq "6" "$(letdays "${PFX}-TWO")" "2nd-4th plus 6th-8th is six days"
t_eq ".###.###......................" "$(strip "${PFX}-TWO")" "…with the 5th free between them"

# -------------------------------------------------------------------------
sec "7. the type aggregate"
# -------------------------------------------------------------------------
# 5+3+3+6+0+6+0 = 23 let-days over 7 units x 30 days = 210.
AGG=$(http_get "/rental/calendar?month=${MONTH}" | python3 -c "
import sys, json
d = json.load(sys.stdin)
for t in d['types']:
    if t['code'] == '${PFX}B':
        print(t['units'], t['let_days'], t['possible'], round(t['pct'], 2)); break
else:
    print('MISSING')
")
t_eq "7 23 210 10.95" "$AGG" "the type rolls its units up correctly"

# -------------------------------------------------------------------------
sec "8. month navigation, and a month that is not a month"
# -------------------------------------------------------------------------
t_contains "$(http_get '/rental/calendar?month=2026-02')" '"days":28' \
     "February 2026 is 28 days"
t_contains "$(http_get '/rental/calendar?month=2024-02')" '"days":29' \
     "February 2024 is 29 — the leap year is PostgreSQL's problem, not ours"
# A broken link should still draw a calendar rather than erroring.
t_contains "$(http_get '/rental/calendar?month=not-a-month')" '"month":' \
     "an unparseable month falls back to the current one"

# -------------------------------------------------------------------------
sec "9. retired units are not lettable stock"
# -------------------------------------------------------------------------
pg "UPDATE rental_unit SET state='retired' WHERE id=${U_FREE:-0}" >/dev/null
t_lacks "$(http_get "/rental/calendar?month=${MONTH}")" "${PFX}-FREE" \
     "a retired unit drops out of the calendar"
# …and out of the denominator, or occupancy is understated for ever.
AGG2=$(http_get "/rental/calendar?month=${MONTH}" | python3 -c "
import sys, json
d = json.load(sys.stdin)
for t in d['types']:
    if t['code'] == '${PFX}B':
        print(t['units'], t['possible']); break
")
t_eq "6 180" "$AGG2" "and out of the denominator with it"

# -------------------------------------------------------------------------
sec "10. the endpoint needs a session"
# -------------------------------------------------------------------------
# Occupancy and tenant names are commercially sensitive; the first cut of the
# neighbouring rental routes had no auth at all (RentalModule.cpp).
t_eq "401" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/rental/calendar")" \
     "an unauthenticated caller gets 401, not the tenant list"

verdict

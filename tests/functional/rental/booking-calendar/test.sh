#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# The booking calendar: daily occupancy, and letting a unit from it.
#
# Asked for: "a calendar for daily booking and usage … a left sidebar list of
# available assets … click a category and see an overall occupancy indicator."
#
# The feature needed one change to an existing rule, and it is the risky part
# of the whole thing. Migration 803 put a partial UNIQUE index on
# (unit_id) WHERE state IN ('pending','active') — the double-let guard. It
# also made a booking calendar impossible: one live line per unit means a unit
# can be let ONCE, ever, so Alice 3-7 December and Bob 12-20 December could not
# both exist.
#
# Migration 820 replaces it with an overlap exclusion. That is strictly
# sharper — the rule was never "one line per unit", it was "never two tenants
# in one unit at the same time" — but relaxing a safety constraint is exactly
# the kind of change that goes wrong quietly, so this test pins BOTH halves:
#
#   * two non-overlapping bookings on one unit are ALLOWED (new capability)
#   * an overlapping booking is REFUSED, naming the dates (guard intact)
#
# Everything is clicked. The units are made in the New-unit dialog, the
# customer on the contact form, and the bookings by clicking days on the
# calendar — because a booking planted by `create` would not exercise the
# screen that was built for this.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZBK'
cleanup() {
    pg "DELETE FROM rental_contract_line WHERE unit_id IN
          (SELECT id FROM rental_unit WHERE code LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_event WHERE unit_id IN
          (SELECT id FROM rental_unit WHERE code LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE name LIKE 'BK-${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_unit WHERE code LIKE '${PFX}%'" >/dev/null 2>&1
    pg "UPDATE res_partner SET parent_id=NULL WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

# -------------------------------------------------------------------------
sec "1. the guard that had to change"
# -------------------------------------------------------------------------
# Named explicitly: if someone re-adds the old index, sequential bookings stop
# working and the failure appears three screens away as "the calendar refuses
# my second booking".
t_eq "1" "$(pg "SELECT count(*) FROM pg_constraint WHERE conname='rental_cl_unit_no_overlap'")" \
     "the overlap exclusion constraint is installed"
t_eq "0" "$(pg "SELECT count(*) FROM pg_indexes WHERE indexname='rental_cl_unit_live_uniq'")" \
     "and the one-live-line-per-unit index is gone"
t_eq "1" "$(pg "SELECT count(*) FROM pg_trigger WHERE tgname='rental_line_overlap_trg'")" \
     "the readable half of the guard is a trigger"

# -------------------------------------------------------------------------
sec "2. the screen exists and is reachable"
# -------------------------------------------------------------------------
t_eq "rental.booking" "$(pg "SELECT res_model FROM ir_act_window WHERE id=127")" \
     "the Booking action points at the Booking screen"
t_eq "127" "$(pg "SELECT action_id FROM ir_ui_menu WHERE id=315")" \
     "and the Rental -> Booking menu opens it"

# -------------------------------------------------------------------------
sec "3. the browser tooling"
# -------------------------------------------------------------------------
CHROME=${CHROME_PATH:-/usr/bin/google-chrome}
if [ ! -x "$CHROME" ]; then
    echo "    NOTE  no Chrome at $CHROME — skipping the on-screen journey"
    verdict; exit $?
fi
if [ ! -d node_modules/puppeteer-core ]; then
    echo "    NOTE  puppeteer-core is not installed — skipping the on-screen journey"
    verdict; exit $?
fi
ok "Chrome and puppeteer-core are present"

# -------------------------------------------------------------------------
sec "4. booking a unit from the calendar, by clicking"
# -------------------------------------------------------------------------
OUT=$(SHOTDIR=/tmp/booking_test BASE="$BASE" DBN="$DBN" \
      timeout 420 node tests/lib/render_booking.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "the calendar books, shows usage, and refuses a double-let"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "5. the same facts, checked independently of the driver"
# -------------------------------------------------------------------------
U1=$(pg "SELECT id FROM rental_unit WHERE code='${PFX}-C1'")
t_nonempty "$U1" "the first unit was created through the dialog"

t_eq "2" "$(pg "SELECT count(*) FROM rental_contract_line WHERE unit_id=${U1:-0}
                 AND state IN ('pending','active')")" \
     "the unit carries TWO live lines — impossible under the old index"

# The lines the calendar drew must be the lines billing will read. One query,
# both claims: they belong to the customer, and they do not overlap.
t_eq "0" "$(pg "SELECT count(*) FROM rental_contract_line a, rental_contract_line b
                 WHERE a.unit_id=${U1:-0} AND b.unit_id=${U1:-0} AND a.id<b.id
                   AND a.state IN ('pending','active') AND b.state IN ('pending','active')
                   AND daterange(a.date_start, COALESCE(a.date_end,'infinity'::date),'[]')
                    && daterange(b.date_start, COALESCE(b.date_end,'infinity'::date),'[]')")" \
     "and they do not overlap each other"

t_eq "9" "$(pg "SELECT COALESCE(SUM(date_end - date_start + 1),0)
                  FROM rental_contract_line WHERE unit_id=${U1:-0}
                   AND state IN ('pending','active')")" \
     "nine let-days in total, which is what the strip drew"

U2=$(pg "SELECT id FROM rental_unit WHERE code='${PFX}-C2'")
t_eq "0" "$(pg "SELECT count(*) FROM rental_contract_line WHERE unit_id=${U2:-0}")" \
     "the second unit was never booked, and has no lines"

# -------------------------------------------------------------------------
sec "6. the database refuses an overlap even when the screen is bypassed"
# -------------------------------------------------------------------------
# The UI checks first so it can name the clash. This is the check that holds
# when someone writes SQL by hand, or two operators race.
FROM=$(pgv "SELECT to_char(MIN(date_start),'YYYY-MM-DD') FROM rental_contract_line
              WHERE unit_id=${U1:-0}" | tr -d ' ')
CT=$(pg "SELECT contract_id FROM rental_contract_line WHERE unit_id=${U1:-0} ORDER BY id LIMIT 1")
PA=$(pg "SELECT partner_id  FROM rental_contract_line WHERE unit_id=${U1:-0} ORDER BY id LIMIT 1")
RAW=$(pgv "INSERT INTO rental_contract_line
             (contract_id, unit_id, partner_id, date_start, date_end, state, billing_mode)
           VALUES (${CT:-0}, ${U1:-0}, ${PA:-0}, '${FROM}'::date, '${FROM}'::date,
                   'pending', 'oneoff')" 2>&1)
case "$RAW" in
    *"already let"*|*"exclusion constraint"*|*"conflicting key"*)
        ok "a hand-written overlapping INSERT is refused by the database" ;;
    *) no "a hand-written overlapping INSERT was ACCEPTED: ${RAW:-<no error>}" ;;
esac

verdict

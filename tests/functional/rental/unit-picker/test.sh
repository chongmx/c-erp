#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Putting a unit on a rental contract — the whole way, by clicking.
#
# Reported: "unit picker never really work."
#
# It was three defects stacked, and each one hid the next:
#
#   1. NOTHING TO PICK. A clean database has zero rental units, so the picker
#      answered "No match" forever. Correct, and useless.
#   2. NO WAY OUT. The "＋" beside it posted create({name}), and rental.unit
#      requires a CODE — so the escape hatch came back "An internal error
#      occurred", on the one screen where the picker is empty precisely because
#      no unit exists yet. A duplicate code said the same thing.
#   3. THE REST OF THE LINE WAS UNUSABLE. billing_mode and state were
#      registered FieldType::Char, so Billing and Status rendered as free TEXT
#      BOXES: you had to type "recurring" and "active" letter-perfect into a
#      form that never showed the valid values, and a CHECK constraint refused
#      anything else on save. The dates were text boxes too.
#
# Not one of those is visible to an API test, which creates a unit in a single
# call and never opens the form — which is exactly how all three survived a
# green suite. So this file drives the screen and plants nothing over the API.
# tests/lib/render_unit_picker.mjs does the driving; this owns the fixtures,
# the cleanup and the verdict, and re-checks the database independently.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZUP'
cleanup() {
    pg "DELETE FROM rental_contract_line WHERE contract_id IN
          (SELECT id FROM rental_contract WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_contract_line WHERE unit_id IN
          (SELECT id FROM rental_unit WHERE code LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_event WHERE unit_id IN
          (SELECT id FROM rental_unit WHERE code LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_unit WHERE code LIKE '${PFX}%'" >/dev/null 2>&1
    pg "UPDATE res_partner SET parent_id=NULL WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

CHROME=${CHROME_PATH:-/usr/bin/google-chrome}

# -------------------------------------------------------------------------
sec "1. the browser tooling"
# -------------------------------------------------------------------------
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
sec "2. starting from nothing: create a unit and rent it, on screen"
# -------------------------------------------------------------------------
#   0. a customer, through the contact form
#   1. a new contract, "+ Add a line"
#   2. the line row's shape — dropdowns and date inputs, not text boxes
#   3. an empty picker, then "＋" asking for Name AND Code — three units made
#   4. a duplicate code refused in words, not as an internal error
#   5. CLICK the picker: it lists all three (photographed, dropdown open)
#   6. typing narrows it, by code and by name
#   7. fill the line, pick the customer, save
#   8. reopen: the line, its unit and its billing mode all survived
OUT=$(SHOTDIR=/tmp/unit_picker_test BASE="$BASE" DBN="$DBN" \
      timeout 420 node tests/lib/render_unit_picker.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "a unit can be created and rented without leaving the form"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the same facts, checked independently of the driver"
# -------------------------------------------------------------------------
UNIT=$(pg "SELECT id FROM rental_unit WHERE code='${PFX}-A101'")
t_nonempty "$UNIT" "the unit was created from the contract form"
t_eq "1" "$(pg "SELECT count(*) FROM rental_unit WHERE code='${PFX}-A101'")" \
     "exactly one — the duplicate attempt did not slip through"
t_eq "3" "$(pg "SELECT count(*) FROM rental_unit WHERE code LIKE '${PFX}-%'")" \
     "all three units were created through the ＋ dialog"

# The dropdown claimed to list exactly three. Confirm from the database that
# three is also the number of units that EXIST — otherwise "exactly three"
# would be satisfied by a picker showing three of a larger set, which is the
# failure this whole test is about.
t_eq "3" "$(pg "SELECT count(*) FROM rental_unit WHERE active")" \
     "and three is every active unit there is — the picker showed the whole list"

CT=$(pg "SELECT id FROM rental_contract WHERE name='${PFX}-RC'")
t_nonempty "$CT" "the contract was saved"
t_eq "1" "$(pg "SELECT count(*) FROM rental_contract_line WHERE contract_id=${CT:-0}")" \
     "with exactly one line"
t_eq "$UNIT" "$(pg "SELECT unit_id FROM rental_contract_line WHERE contract_id=${CT:-0}")" \
     "and the line carries the unit that was picked"

# The values chosen from the two dropdowns must be what a CHECK constraint
# accepts — the whole reason they stopped being text boxes.
t_eq "recurring" "$(pg "SELECT billing_mode FROM rental_contract_line WHERE contract_id=${CT:-0}")" \
     "the Billing chosen from the dropdown was stored"
t_eq "active" "$(pg "SELECT state FROM rental_contract_line WHERE contract_id=${CT:-0}")" \
     "and so was the Status"

# -------------------------------------------------------------------------
sec "4. a duplicate code is a 400 with words, not a 500"
# -------------------------------------------------------------------------
# Checked at the API too, because the message is produced in BaseModel::create
# and every model with a UNIQUE constraint depends on it.
DUP=$(call rental.unit create "[{\"code\":\"${PFX}-A101\",\"name\":\"clash\"}]")
if has_error "$DUP"; then ok "a duplicate code is refused"
else no "a duplicate unit code was accepted"; fi
t_lacks "$DUP" "An internal error occurred" "the refusal is not an opaque internal error"
t_contains "$DUP" "already uses this code" "it names what clashed"

verdict

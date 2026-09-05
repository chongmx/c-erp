#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Clicking a unit opens it for editing.
#
# Reported: "after I create a unit, Rental → Operations → Units has a unit. I
# want to be able to click and edit the unit again. maybe change its name and
# zone."
#
# Both views already called openUnit() on click. openUnit() called an optional
# prop that nothing supplies, and returned. So the click did nothing at all —
# on the ONLY screen that lists units, since this component replaces the
# generic list view for rental.unit. A typo in a code, or a locker moved to
# another zone, could not be corrected anywhere in the product.
#
# The dialog now serves create AND edit, one code path, because two would drift
# into accepting different fields — which is how "I can set a zone on the way in
# but not afterwards" happens.
#
# The assertion that matters most is that the dialog opens FILLED. An edit form
# that opens blank is worse than none: saving it wipes every field the user did
# not retype, and the damage is invisible until someone looks for the data.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZUE'
cleanup() {
    pg "DELETE FROM rental_contract_line WHERE unit_id IN
          (SELECT id FROM rental_unit WHERE code LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_event WHERE unit_id IN
          (SELECT id FROM rental_unit WHERE code LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_unit WHERE code LIKE '${PFX}%'" >/dev/null 2>&1
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
sec "2. create it, click it, change it — all on screen"
# -------------------------------------------------------------------------
OUT=$(SHOTDIR=/tmp/unit_edit_test BASE="$BASE" DBN="$DBN" \
      timeout 420 node tests/lib/render_unit_edit.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "a unit can be reopened and corrected from the grid"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the same facts, checked independently of the driver"
# -------------------------------------------------------------------------
U=$(pg "SELECT id FROM rental_unit WHERE code='${PFX}-E1'")
t_nonempty "$U" "the unit exists"

# pgv, not pg: the values contain spaces, and pg() strips them — comparing
# "ZZUENewWing" against "ZZUE New Wing" would fail for the wrong reason.
NAME=$(pgv "SELECT name FROM rental_unit WHERE id=${U:-0}" | sed 's/^ *//;s/ *$//')
ZONE=$(pgv "SELECT zone FROM rental_unit WHERE id=${U:-0}" | sed 's/^ *//;s/ *$//')
t_eq "$NAME" "${PFX} After"    "the new name was written to the database"
t_eq "$ZONE" "${PFX} New Wing" "and the new zone with it"

# The edit must not have created a second unit — a create/update path that
# falls back to create on save is a classic duplicate factory.
t_eq "1" "$(pg "SELECT count(*) FROM rental_unit WHERE code='${PFX}-E1'")" \
     "exactly one unit — editing updated, it did not insert a copy"

# -------------------------------------------------------------------------
sec "4. editing does not disturb what the unit is FOR"
# -------------------------------------------------------------------------
# state is derived from contract lines, and a write that clobbered it would
# silently free an occupied locker. The dialog sends it, so this is worth
# pinning: with no contracts, a unit stays available.
t_eq "available" "$(pg "SELECT state FROM rental_unit WHERE id=${U:-0}")" \
     "an unlet unit is still available after an edit"
t_eq "t" "$(pg "SELECT active FROM rental_unit WHERE id=${U:-0}")" \
     "and still active — editing is not archiving"

verdict

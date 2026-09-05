#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# A new unit type must reach the picker that uses it — searchable, at once.
#
# Reported: "make sure when I create a new unit type, it reflects on that drop
# down and can be searched."
#
# It did not. The New-unit dialog's Type control was a plain <select> filled
# from one search_read(limit 200) issued when the Units screen first opened —
# the three defects M2OSelect exists to kill, all present at once:
#
#   * STALE       a type created afterwards was not in the list until the whole
#                 screen was reloaded. That is precisely the reported symptom.
#   * TRUNCATED   200 rows and no ORDER BY, so the default id ASC keeps the
#                 OLDEST 200 and a new type is absent once the table passes it.
#   * NO SEARCH   a <select> cannot be searched at all.
#
# The load-bearing part of the journey is that the app is NOT reloaded between
# creating the type and opening the picker. A prefetch passes any test that
# refreshes the page in between, which is why this was never caught.
#
# tests/lib/render_unit_type.mjs does the driving, entirely by clicking and
# planting nothing over the API. This file owns the fixtures, the cleanup and
# the verdict, and re-checks the database independently afterwards.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZUT'
cleanup() {
    pg "DELETE FROM rental_contract_line WHERE unit_id IN
          (SELECT id FROM rental_unit WHERE code LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_event WHERE unit_id IN
          (SELECT id FROM rental_unit WHERE code LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_unit WHERE code LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM rental_unit_type WHERE code LIKE '${PFX}%'" >/dev/null 2>&1
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
sec "2. create a type, then find it in the picker — without reloading"
# -------------------------------------------------------------------------
#   1. Rental → Unit Types → New → save a type that did not exist
#   2. Rental → Units → "+ New unit", with NO page reload in between
#   3. type part of its name: it must be offered, and visibly drawn
#   4. pick it, create the unit, and the unit must carry that type
OUT=$(SHOTDIR=/tmp/unit_type_test BASE="$BASE" DBN="$DBN" \
      timeout 420 node tests/lib/render_unit_type.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "a new unit type is searchable in the picker immediately"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the same facts, checked independently of the driver"
# -------------------------------------------------------------------------
TY=$(pg "SELECT id FROM rental_unit_type WHERE code='${PFX}V'")
t_nonempty "$TY" "the unit type was created through its own form"

UN=$(pg "SELECT id FROM rental_unit WHERE code='${PFX}-V1'")
t_nonempty "$UN" "the unit was created from the Units screen"
t_eq "$TY" "$(pg "SELECT type_id FROM rental_unit WHERE id=${UN:-0}")" \
     "and it carries the type that was picked, not a blank"

# -------------------------------------------------------------------------
sec "4. the picker's own query would offer it"
# -------------------------------------------------------------------------
# Separating "the server can find it" from "the screen shows it" is what tells
# you which half to look at when this is reported again. This is the exact
# domain M2OSelect sends when the user types "Walk-in".
HITS=$(call_k rental.unit.type search_read \
       "[[\"|\",[\"name\",\"ilike\",\"Walk-in\"],[\"code\",\"ilike\",\"Walk-in\"]]]" \
       "\"fields\":[\"id\",\"name\",\"code\"],\"limit\":20,\"order\":\"name ASC\"")
t_contains "$HITS" "${PFX} Walk-in Vault" "search_read finds the new type by name"

# Ordering matters as much as the search: `id ASC` on a capped fetch is what
# made a NEW row the first one to fall off the end.
ORD=$(call_k rental.unit.type search_read "[[]]" \
      "\"fields\":[\"id\",\"name\"],\"limit\":200,\"order\":\"name ASC\"")
t_contains "$ORD" "${PFX} Walk-in Vault" "and an ordered full list carries it too"

verdict

#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# FUNCTIONAL — clearing a `part.lookup` error from the review desk.  (CERP-8)
#
# Reported with a screenshot: a 32-bit MCU proposal stuck at INVALID on
# "Cannot read value 'MIPS32 M4K'" and "Unknown unit 'Mbps' — see
# describe.units", with nothing to click. Both are what a growing catalogue
# produces — a new part brings a unit nobody has entered and a value that is
# not a magnitude — so both now carry a button, and taking either re-validates
# the proposal.
#
# The journey is clicks (tests/lib/render_lookup_fixups.mjs). This script owns
# the fixture, and afterwards re-reads the DATABASE: adding a unit and keeping
# a value as text both change stored data, and a screen that only says it did
# is worth nothing.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
PFX='ZZLF'
KIND="${PFX}_rate"

cleanup(){
    pg "DELETE FROM part_parameter WHERE product_id IN
          (SELECT id FROM product_product WHERE name LIKE '%${PFX}%')" >/dev/null
    pg "DELETE FROM part_lookup_result WHERE query LIKE '${PFX}%' OR mpn LIKE '${PFX}%'" >/dev/null
    pg "DELETE FROM part_manufacturer_info WHERE product_id IN
          (SELECT id FROM product_product WHERE name LIKE '%${PFX}%')" >/dev/null
    pg "DELETE FROM product_product  WHERE name LIKE '%${PFX}%'" >/dev/null
    pg "DELETE FROM product_template WHERE name LIKE '%${PFX}%'" >/dev/null
    pg "DELETE FROM part_unit WHERE quantity_kind='${KIND}' OR symbol='Mbps'" >/dev/null
}
cleanup; trap cleanup EXIT
auth_or_die

# -------------------------------------------------------------------------
sec "1. the tooling, and the premise"
# -------------------------------------------------------------------------
t_eq "0" "$(pg "SELECT count(*) FROM part_unit WHERE symbol='Mbps'")" \
     "'Mbps' is not a unit this catalogue knows — which is the whole problem"

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
sec "2. the journey, on screen"
# -------------------------------------------------------------------------
OUT=$(SHOTDIR=/tmp/lookup_fixups BASE="$BASE" DBN="$DBN" \
      timeout 340 node tests/lib/render_lookup_fixups.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "both errors can be cleared from the screen they appear on"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. what it changed, checked independently"
# -------------------------------------------------------------------------
t_eq "1" "$(pg "SELECT count(*) FROM part_unit WHERE symbol='Mbps'")" \
     "the unit was created from the review desk"
t_eq "$KIND" "$(pg "SELECT quantity_kind FROM part_unit WHERE symbol='Mbps'")" \
     "under the quantity that was named on the form"
t_eq "t" "$(pg "SELECT is_base FROM part_unit WHERE symbol='Mbps'")" \
     "as the base of that quantity — it is the first unit to measure it"
t_eq "1" "$(pg "SELECT factor::numeric(10,0) FROM part_unit WHERE symbol='Mbps'")" \
     "with factor 1, which is what being the base means"

LID=$(pg "SELECT id FROM part_lookup_result WHERE mpn LIKE '${PFX}%' ORDER BY id DESC LIMIT 1")
t_nonempty "$LID" "the proposal is still there"
t_eq "pending" "$(pg "SELECT state FROM part_lookup_result WHERE id=${LID:-0}")" \
     "and has re-validated itself out of 'Needs fixing'"
ISS=$(pgv "SELECT issues::text FROM part_lookup_result WHERE id=${LID:-0}")
t_lacks    "$ISS" '"level": "error"' "with no errors left"
t_contains "$ISS" "kept as text"     "and the text decision recorded where the next reader will see it"
t_contains "$(pgv "SELECT payload::text FROM part_lookup_result WHERE id=${LID:-0}")" \
           "true" "the payload carries the reviewer's choice, not a re-typed value"

verdict

#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# A list shows NAMES in its relation columns, never ids.
#
# Reported: "rental -> operation -> contract shows a list of contract. however,
# the customer column is showing a number, not the customer name."
#
# read and search_read project COLUMNS — rowsToJson_ returns what the SELECT
# produced, not the [id, name] pairs the reference ERP sends — so a many2one
# reaches the browser as a bare integer. The generic ListView rendered it raw,
# and the column that says WHOSE contract it is showed "1766".
#
# The label wanted is "name, company" for a person at a company and the plain
# name for an individual, which is exactly res_partner.display_name. So the
# list asks for display_name and falls back to `name`; buildSelectCols_ drops a
# column the model does not have rather than erroring, so one request shape
# serves every relation.
#
# The ids are resolved in ONE read per relation, not one per row — 80 rows with
# three relation columns would otherwise be 240 round trips, which is the
# pattern docs/040 §3.4 exists to prevent.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZLL'
cleanup() {
    pg "DELETE FROM rental_contract_line WHERE contract_id IN
          (SELECT id FROM rental_contract WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
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
sec "2. both kinds of customer, on screen, in the contract list"
# -------------------------------------------------------------------------
#   a company, a person inside it   -> "Person, Company"
#   an individual with no company   -> "Individual", and no stray comma
OUT=$(SHOTDIR=/tmp/list_labels_test BASE="$BASE" DBN="$DBN" \
      timeout 420 node tests/lib/render_list_labels.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "relation columns render names, not ids"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the same facts, checked independently of the driver"
# -------------------------------------------------------------------------
P=$(pg "SELECT id FROM res_partner WHERE name='${PFX} Mina'")
t_nonempty "$P" "the person exists"
t_eq "$P" "$(pg "SELECT partner_id FROM rental_contract WHERE name='${PFX}-C1'")" \
     "the contract really points at that partner id"

# The label the screen drew must be the stored one, not something assembled in
# the browser — otherwise the list and every picker could disagree.
DN=$(pgv "SELECT display_name FROM res_partner WHERE id=${P:-0}" | sed 's/^ *//;s/ *$//')
t_eq "$DN" "${PFX} Mina, ${PFX} Orchard Bhd" "and its stored display_name is what the cell showed"

L=$(pg "SELECT id FROM res_partner WHERE name='${PFX} Walkin'")
DL=$(pgv "SELECT display_name FROM res_partner WHERE id=${L:-0}" | sed 's/^ *//;s/ *$//')
t_eq "$DL" "${PFX} Walkin" "an individual with no company has no suffix to show"

verdict

#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# "Carol, Big Carrots" — in the combobox, not just in the column.
#
# Reported: "an Individual, if connected to a company, should be displayed as
# Carol, Big Carrots. a Company will stay as it is. please update all of the
# customer/supplier/vendor etc picker combobox."
#
# tests/integration/core/partner-display-name proves the STORED value follows
# the rule, including through a rename and a re-parent. It cannot prove the
# user's actual complaint, which is about what a dropdown says. Those come
# apart easily: a picker formats its own label, or asks for columns that do not
# include this one, or shows a list it fetched before the contact existed.
#
# So this file drives the screen. Every record is typed and clicked into being
# through the real forms — the contact through Contacts with its badges, the
# contract through the contract form's own picker — and nothing at all is
# planted over the API. tests/lib/render_display_name.mjs does the driving;
# this file owns the fixtures, the cleanup and the verdict, and re-checks the
# database independently so a driver that stopped early cannot pass by silence.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZDSP'
cleanup() {
    pg "DELETE FROM rental_contract_line WHERE contract_id IN
          (SELECT id FROM rental_contract WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM sale_order WHERE partner_id IN
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
# Skip rather than fail when the tooling is absent: a missing Chrome is a
# missing tool, and a suite that goes red for that teaches people to ignore it.
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
sec "2. the label, on screen, by clicking"
# -------------------------------------------------------------------------
#   1. a company, created through the contact form
#   2. a person, attached to it through the form's own Company picker
#   3. the rental contract's Customer picker (GENERIC form renderer)
#   4. the picked value keeps the label, and the contract saves
#   5. reopening resolves the label by id — a different code path
#   6. the sales order form (hand-written markup) says the same thing
OUT=$(SHOTDIR=/tmp/display_name_test BASE="$BASE" DBN="$DBN" \
      timeout 420 node tests/lib/render_display_name.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "every picker on screen shows the composed label"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the same facts, checked independently of the driver"
# -------------------------------------------------------------------------
CO=$(pg "SELECT id FROM res_partner WHERE name='${PFX} Big Carrots'")
t_nonempty "$CO" "the company was created through the form"
PERSON=$(pg "SELECT id FROM res_partner WHERE name='${PFX} Carol'")
t_nonempty "$PERSON" "the individual was created through the form"

t_eq "$CO" "$(pg "SELECT parent_id FROM res_partner WHERE id=${PERSON:-0}")" \
     "picking the company in the form really set parent_id"

# pgv, not pg: pg() strips spaces and would turn the expected label into
# "Carol,BigCarrots", which matches nothing and reads as a product bug.
DN=$(pgv "SELECT display_name FROM res_partner WHERE id=${PERSON:-0}" | sed 's/^ *//;s/ *$//')
t_eq "$DN" "${PFX} Carol, ${PFX} Big Carrots" "the stored label backs what the screen showed"

DNCO=$(pgv "SELECT display_name FROM res_partner WHERE id=${CO:-0}" | sed 's/^ *//;s/ *$//')
t_eq "$DNCO" "${PFX} Big Carrots" "and the company is still displayed as itself"

t_eq "$PERSON" "$(pg "SELECT partner_id FROM rental_contract WHERE name='${PFX}-RC-UI'")" \
     "the contract saved from the screen carries the customer that was picked"

verdict

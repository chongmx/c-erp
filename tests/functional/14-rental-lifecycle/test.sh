#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# One tenancy, end to end, ENTIRELY BY CLICKING.
#
#   customer company -> a person who works there -> a unit -> a contract with
#   that unit on it -> run the billing -> the invoice -> pay it -> stop the
#   contract -> nothing bills again.
#
# Nothing in the journey is created over the API. The earlier version of this
# file did it all with `call rental.contract create` and friends; it proved the
# billing arithmetic and the money, and proved nothing whatever about the
# screens. "I cannot select this company in my new rental contract" was
# reported three times while this suite was green.
#
# Converting it to clicks found five things no API test could:
#
#   1. a unit could not be put on a contract AT ALL — no lines section, no
#      menu for rental.contract.line, and the unit grid's click was a no-op
#   2. a unit could not be created from the Units screen either
#   3. a line created through the form never billed: nothing set
#      next_period_start, which the billing run requires (migration 819)
#   4. the invoice's Register Payment dialog could not load its journals —
#      it called RpcService.searchRead(), which does not exist, and an empty
#      catch swallowed the TypeError. An invoice could not be paid from the
#      screen at all
#   5. the dialog's inputs were inline assignments in t-on-* handlers, which
#      OWL cannot compile — "v2 is not a function" on every keystroke, and the
#      field never updated
#
# Read the driver for the journey; this file owns fixtures, cleanup and the
# verdict, and re-checks the important facts against the database so a driver
# that stopped early cannot pass by omission.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
M=1000000                       # money is BIGINT micro-units (docs/047)

PFX='ZZLIFE'
cleanup() {
    pg "DELETE FROM account_move_line WHERE move_id IN
          (SELECT id FROM account_move WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE '${PFX}%'))" >/dev/null 2>&1
    pg "DELETE FROM account_payment WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM account_move WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_contract_line WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
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
# A functional test here is click-driven or it is nothing. Without a browser
# there is no reduced API version to fall back on — skip and say so.
if [ ! -x "$CHROME" ]; then
    echo "    NOTE  no Chrome at $CHROME — skipping (this test is click-driven only)"
    verdict; exit $?
fi
if [ ! -d node_modules/puppeteer-core ]; then
    echo "    NOTE  puppeteer-core is not installed — skipping"
    verdict; exit $?
fi
ok "Chrome and puppeteer-core are present"

# -------------------------------------------------------------------------
sec "2. the journey, by clicking"
# -------------------------------------------------------------------------
OUT=$(SHOTDIR=/tmp/lifecycle_test BASE="$BASE" DBN="$DBN" \
      timeout 420 node tests/lib/render_lifecycle.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "the whole tenancy works on screen"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the same facts, checked against the database"
# -------------------------------------------------------------------------
CO=$(pg "SELECT id FROM res_partner WHERE name='${PFX} Sunrise Traders Sdn Bhd'")
t_nonempty "$CO" "the customer company exists"
t_eq "t" "$(pg "SELECT is_company FROM res_partner WHERE id=${CO:-0}")" "it is a company"

WHO=$(pg "SELECT id FROM res_partner WHERE name='${PFX} Siti Rahman'")
t_eq "$CO" "$(pg "SELECT parent_id FROM res_partner WHERE id=${WHO:-0}")" \
     "the contact is under it"

RC_ID=$(pg "SELECT id FROM rental_contract WHERE name='${PFX}-RC-001'")
t_nonempty "$RC_ID" "the contract exists"
t_eq "$CO" "$(pg "SELECT partner_id FROM rental_contract WHERE id=${RC_ID:-0}")" \
     "billed to the COMPANY, not to the individual"
t_eq "closed" "$(pg "SELECT state FROM rental_contract WHERE id=${RC_ID:-0}")" \
     "and it ends closed"

t_eq "1" "$(pg "SELECT count(*) FROM rental_contract_line WHERE contract_id=${RC_ID:-0}")" \
     "with one unit on it"
t_eq "$CO" "$(pg "SELECT partner_id FROM rental_contract_line WHERE contract_id=${RC_ID:-0}")" \
     "the line inherited the contract's customer (migration 818)"
# The bug that made a form-created line silently unbillable.
t_ne "" "$(pg "SELECT COALESCE(next_period_start::text,'') FROM rental_contract_line
                 WHERE contract_id=${RC_ID:-0}")" \
     "and it was given a next period, so it can bill at all (migration 819)"

t_eq "available" "$(pg "SELECT state FROM rental_unit WHERE code='${PFX}-A1'")" \
     "the unit is released when the tenancy ends"

# -------------------------------------------------------------------------
sec "4. exactly one invoice, and it is paid"
# -------------------------------------------------------------------------
t_eq "1" "$(pg "SELECT count(*) FROM account_move
                 WHERE partner_id=${CO:-0} AND move_type='out_invoice'")" \
     "one invoice — three later billing runs raised nothing more"
INV=$(pg "SELECT id FROM account_move WHERE partner_id=${CO:-0} AND move_type='out_invoice'")
case "$(pg "SELECT payment_state FROM account_move WHERE id=${INV:-0}")" in
    paid|in_payment) ok "it is paid" ;;
    *) no "payment_state is '$(pg "SELECT payment_state FROM account_move WHERE id=${INV:-0}")'" ;;
esac
t_eq "0" "$(pg "SELECT amount_residual FROM account_move WHERE id=${INV:-0}")" \
     "nothing outstanding"
t_eq "$((1200 * M))" "$(pg "SELECT amount_total FROM account_move WHERE id=${INV:-0}")" \
     "for the rate that was typed on the line"
t_ge "$(pg "SELECT count(*) FROM account_payment WHERE partner_id=${CO:-0}")" "1" \
     "and a payment record exists against the company"

# -------------------------------------------------------------------------
sec "5. the customer now has history, so cannot be deleted"
# -------------------------------------------------------------------------
OUT=$(call res.partner unlink "[[${CO:-0}]]")
if has_error "$OUT"; then ok "the company cannot be deleted while it has documents"
else no "a company with an invoice and a contract was DELETED"; fi
t_contains "$OUT" "Archive it instead" "and is told what to do instead"

verdict

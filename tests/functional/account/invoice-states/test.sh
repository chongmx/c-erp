#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# FUNCTIONAL — WHERE AN INVOICE ENDS UP.  (CERP-6, with CERP-7)
#
# An invoice has four endings and the form showed one word for all of them.
# "Posted" is true of a paid invoice, of one reversed by a credit note and of
# one nobody has paid — and a cancelled invoice looked posted-then-something.
# The corner ribbon states the verdict; the status bar keeps showing the
# workflow. This test is the ribbon read off the screen.
#
# The journey is clicks only (tests/lib/render_invoice_states.mjs): the menu,
# the list, the form buttons, the payment dialog. Nothing here calls the API
# to move a document — if a button is missing or wired to nothing, the driver
# stops and this test fails.
#
# Three invoices are seeded as DRAFTS, because that is now the only way an
# invoice is born (CERP-7). The first is paid, the second cancelled, the third
# reversed by a credit note that the driver confirms itself. Afterwards this
# script re-reads the DATABASE, so a driver that quietly stopped early cannot
# pass by saying nothing.
# =============================================================
auth_or_die

M=1000000
PFX='ZZIS'

cleanup() {
    pg "DELETE FROM account_partial_reconcile WHERE payment_id IN
          (SELECT id FROM account_payment WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE '${PFX} %'))" >/dev/null
    pg "DELETE FROM account_payment    WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE '${PFX} %')" >/dev/null
    pg "DELETE FROM account_move_line  WHERE move_id IN
          (SELECT id FROM account_move WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE '${PFX} %'))" >/dev/null
    pg "DELETE FROM account_move       WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE '${PFX} %')" >/dev/null
    pg "DELETE FROM res_partner        WHERE name LIKE '${PFX} %'" >/dev/null
}
cleanup
trap cleanup EXIT

JRN_SALE=$(pg "SELECT id FROM account_journal WHERE type='sale' AND company_id=1 ORDER BY id LIMIT 1")
JRN_BANK=$(pg "SELECT id FROM account_journal WHERE type='bank' AND company_id=1 ORDER BY id LIMIT 1")
ARECV=$(pg "SELECT id FROM account_account WHERE account_type='asset_receivable' AND company_id=1 ORDER BY id LIMIT 1")
AINC=$(pg  "SELECT id FROM account_account WHERE account_type IN ('income','income_other') AND company_id=1 ORDER BY id LIMIT 1")

# -------------------------------------------------------------------------
sec "1. the fixtures, and the tooling to drive them"
# -------------------------------------------------------------------------
t_nonempty "$JRN_SALE" "a sale journal exists"
t_nonempty "$JRN_BANK" "a bank journal exists — Register Payment needs one"
t_nonempty "$ARECV"    "a receivable account exists"
t_nonempty "$AINC"     "an income account exists"
[ -z "$JRN_SALE" ] || [ -z "$JRN_BANK" ] || [ -z "$ARECV" ] && { verdict; exit 1; }

mkdraft() {   # mkdraft <partner-name> <total-majors> -> move id
    local p mv amt=$(( $2 * M ))
    p=$(pgid "INSERT INTO res_partner (name, active, company_id) VALUES ('$1', true, 1) RETURNING id")
    # name '/' and state 'draft': exactly what billing and every other
    # producer now writes. The number is assigned by posting, not before.
    mv=$(pgid "INSERT INTO account_move
        (name, move_type, state, date, invoice_date, due_date, journal_id, company_id,
         partner_id, amount_untaxed, amount_tax, amount_total, amount_residual, payment_state)
        VALUES ('/', 'out_invoice', 'draft', CURRENT_DATE, CURRENT_DATE, CURRENT_DATE,
                $JRN_SALE, 1, $p, $amt, 0, $amt, $amt, 'not_paid') RETURNING id")
    pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,partner_id,name,date,debit,credit)
        VALUES ($mv,$ARECV,$JRN_SALE,1,$p,'$1 receivable',CURRENT_DATE,$amt,0)" >/dev/null
    pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,partner_id,name,date,debit,credit,price_unit,quantity)
        VALUES ($mv,$AINC,$JRN_SALE,1,$p,'$1 service',CURRENT_DATE,0,$amt,$amt,1)" >/dev/null
    echo "$mv"
}

MV_PAY=$(mkdraft  "${PFX} Pay"     400)
MV_CAN=$(mkdraft  "${PFX} Cancel"  250)
MV_REV=$(mkdraft  "${PFX} Reverse" 180)
t_nonempty "$MV_PAY" "a draft invoice to pay"
t_nonempty "$MV_CAN" "a draft invoice to cancel"
t_nonempty "$MV_REV" "a draft invoice to reverse"
t_eq "3" "$(pg "SELECT count(*) FROM account_move WHERE id IN ($MV_PAY,$MV_CAN,$MV_REV) AND state='draft'")" \
     "all three start as drafts, unnumbered"

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
OUT=$(SHOTDIR=/tmp/invoice_states BASE="$BASE" DBN="$DBN" \
      timeout 340 node tests/lib/render_invoice_states.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "every ending is visible on the invoice itself"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the ledger, checked independently of the driver"
# -------------------------------------------------------------------------
t_eq "posted" "$(pg "SELECT state FROM account_move WHERE id=$MV_PAY")"        "the first invoice was posted by the Confirm button"
t_eq "paid"   "$(pg "SELECT payment_state FROM account_move WHERE id=$MV_PAY")" "and settled by the payment dialog"
t_eq "0"      "$(pg "SELECT amount_residual FROM account_move WHERE id=$MV_PAY")" "nothing is left owing on it"
t_eq "1"      "$(pg "SELECT count(*) FROM account_payment WHERE partner_id=(SELECT partner_id FROM account_move WHERE id=$MV_PAY)")" \
     "one payment was created, not two"
# Posting is where the number comes from. A draft that carried INV... would
# mean the sequence had been consumed before anyone confirmed anything.
t_eq "1" "$(pg "SELECT (name LIKE 'INV%')::int FROM account_move WHERE id=$MV_PAY")" \
     "posting is what numbered it"

t_eq "cancel" "$(pg "SELECT state FROM account_move WHERE id=$MV_CAN")" "the second invoice was cancelled"

t_eq "posted" "$(pg "SELECT state FROM account_move WHERE id=$MV_REV")" "the third invoice is posted"
CN=$(pg "SELECT id FROM account_move WHERE reversed_entry_id=$MV_REV")
t_nonempty "$CN" "a credit note points back at it"
t_eq "out_refund" "$(pg "SELECT move_type FROM account_move WHERE id=${CN:-0}")" "and it is a credit note, not a second invoice"
t_eq "posted"     "$(pg "SELECT state FROM account_move WHERE id=${CN:-0}")"     "the driver confirmed it on screen"
t_eq "1" "$(pg "SELECT (name LIKE 'RINV%')::int FROM account_move WHERE id=${CN:-0}")" \
     "credit notes number in their own RINV series, not the invoice one"

verdict

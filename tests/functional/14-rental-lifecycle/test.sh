#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# One tenancy, end to end — the journey a person actually takes.
#
#   customer company  ->  a person who works there  ->  a rental contract
#     ->  the billing run raises an invoice  ->  the invoice is paid
#     ->  the contract is stopped  ->  nothing bills again
#
# Every step goes through the HTTP API the screens use, in the order a user
# does them, so this fails if any link between them is broken — not just if a
# single model misbehaves.
#
# What it is really guarding:
#
#   * the invoice must be addressed to the COMPANY, not to the person, even
#     though the contract was signed with the company and the person is who you
#     talk to. Billing the individual is a real and expensive mistake.
#   * the money must survive the round trip: rate in, invoice total out, and
#     amount_residual back to zero on payment.
#   * stopping a contract must actually STOP it. A contract that keeps
#     invoicing after the tenant leaves is the worst outcome in this module,
#     which is why the last section runs the billing again and asserts silence.
#   * the unit must be released, or it can never be re-let.
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
    pg "DELETE FROM rental_contract WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_unit WHERE code LIKE '${PFX}-%'" >/dev/null 2>&1
    pg "UPDATE res_partner SET parent_id=NULL WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

# The /rental/ routes authenticate by cookie (docs/061); call_kw uses the
# context. Same session either way.
runbill() { curl -s -b "session_id=$SID" -X POST "$BASE/rental/billing/run?date=$1"; }

# -------------------------------------------------------------------------
sec "1. a customer company"
# -------------------------------------------------------------------------
CO=$(call res.partner create "[{\"name\":\"${PFX} Sunrise Traders Sdn Bhd\",
     \"is_company\":true,\"customer_rank\":1,\"email\":\"ap@sunrise.test\",
     \"street\":\"12 Jalan Satu\",\"city\":\"Penang\"}]" | rid)
t_nonempty "$CO" "the customer company is created"
t_eq "t" "$(pg "SELECT is_company FROM res_partner WHERE id=$CO")" "it is a company"

# -------------------------------------------------------------------------
sec "2. a person who works there"
# -------------------------------------------------------------------------
PERSON=$(call res.partner create "[{\"name\":\"${PFX} Siti Rahman\",\"parent_id\":$CO,
         \"email\":\"siti@sunrise.test\",\"job_position\":\"Office Manager\"}]" | rid)
t_nonempty "$PERSON" "the contact is created under the company"
t_eq "$CO" "$(pg "SELECT parent_id FROM res_partner WHERE id=$PERSON")" "she is linked to it"
t_eq "${PFX} Sunrise Traders Sdn Bhd" \
     "$(pgv "SELECT COALESCE(commercial_company_name,'') FROM res_partner WHERE id=$PERSON")" \
     "her row shows the company name, which is what the Contacts list renders"
# The address is inherited, so nobody retypes it (docs/130 §7).
t_eq "Penang" "$(pgv "SELECT COALESCE(city,'') FROM res_partner WHERE id=$PERSON")" \
     "she inherits the company address"

# -------------------------------------------------------------------------
sec "3. a rental contract for the company"
# -------------------------------------------------------------------------
UNIT=$(pgid "INSERT INTO rental_unit (code,name,state,company_id)
             VALUES ('${PFX}-A1','Unit A1','available',1) RETURNING id")
t_nonempty "$UNIT" "there is a unit to let"

RC=$(call rental.contract create "[{\"name\":\"${PFX} RC-001\",\"partner_id\":$CO,
     \"billing_period\":\"monthly\",\"date_start\":\"2026-03-01\",
     \"billing_lead_days\":7,\"state\":\"active\"}]" | rid)
t_nonempty "$RC" "the contract is created"
t_eq "$CO" "$(pg "SELECT partner_id FROM rental_contract WHERE id=${RC:-0}")" \
     "it is signed with the COMPANY, not with the person"
t_eq "1/month" \
     "$(pg "SELECT billing_interval||'/'||billing_unit FROM rental_contract WHERE id=${RC:-0}")" \
     "monthly resolves to every 1 month"

TAX=$(pg "SELECT id FROM account_tax WHERE type_tax_use='sale' AND active ORDER BY id LIMIT 1")
LINE=$(pgid "INSERT INTO rental_contract_line
             (contract_id,partner_id,unit_id,date_start,unit_price,tax_ids_json,
              state,billing_mode,billing_anchor_day,billing_lead_days,
              next_period_start,company_id)
             VALUES (${RC:-0},$CO,$UNIT,'2026-03-01',$((1200 * M)),'[${TAX:-}]',
                     'active','recurring',1,7,'2026-03-01',1) RETURNING id")
t_nonempty "$LINE" "the unit is put on the contract at RM1,200/month"

# Migration 811 derives unit state from its lines; an occupied unit must not
# be offered to the next customer.
t_eq "occupied" "$(pg "SELECT state FROM rental_unit WHERE id=$UNIT")" \
     "the unit is now occupied"

# -------------------------------------------------------------------------
sec "4. the billing run raises an invoice"
# -------------------------------------------------------------------------
# 22 Feb is inside the 7-day lead for a 1 March period, so this is the run
# that should bill — in advance, as a landlord does.
OUT=$(runbill 2026-02-22)
t_contains "$OUT" "invoice" "the billing run reports what it did"

INV=$(pg "SELECT id FROM account_move
           WHERE partner_id=$CO AND move_type='out_invoice' ORDER BY id DESC LIMIT 1")
t_nonempty "$INV" "an invoice exists"

# THE assertion of this file. The contract is with the company; the invoice
# must be too, however many people are attached to it.
t_eq "$CO" "$(pg "SELECT partner_id FROM account_move WHERE id=${INV:-0}")" \
     "the invoice is addressed to the COMPANY"
t_ne "$PERSON" "$(pg "SELECT partner_id FROM account_move WHERE id=${INV:-0}")" \
     "and NOT to the individual"

TOTAL=$(pg "SELECT amount_total FROM account_move WHERE id=${INV:-0}")
t_ge "${TOTAL:-0}" "$((1200 * M))" "the invoice is at least the monthly rate"
t_contains "$(call account.move read "[[${INV:-0}],[\"invoice_date\",\"due_date\"]]")" \
    "2026" "it carries dates"

# Billing in advance: the invoice must precede the period it covers.
IDATE=$(pgv "SELECT to_char(invoice_date,'YYYY-MM-DD') FROM account_move WHERE id=${INV:-0}")
case "$IDATE" in
    2026-02-*) ok "raised in February for the March period — billed in advance" ;;
    *)         no "invoice dated '$IDATE', which is not in advance of 1 March" ;;
esac

# Running the same date again must not bill twice.
runbill 2026-02-22 >/dev/null
t_eq "1" "$(pg "SELECT count(*) FROM account_move WHERE partner_id=$CO AND move_type='out_invoice'")" \
     "running the same date again does NOT double-bill"

# -------------------------------------------------------------------------
sec "5. the invoice is paid"
# -------------------------------------------------------------------------
BEFORE=$(pg "SELECT amount_residual FROM account_move WHERE id=${INV:-0}")
t_ge "${BEFORE:-0}" "1" "before payment there is something outstanding"

PAY=$(call account.move action_register_payment "[[${INV:-0}]]")
if has_error "$PAY"; then no "registering the payment failed: $(echo "$PAY" | head -c 160)"
else ok "the payment is registered"; fi

STATE=$(pg "SELECT payment_state FROM account_move WHERE id=${INV:-0}")
case "$STATE" in
    paid|in_payment) ok "the invoice reads as paid ($STATE)" ;;
    *)               no "payment_state is '$STATE', expected paid" ;;
esac
t_eq "0" "$(pg "SELECT amount_residual FROM account_move WHERE id=${INV:-0}")" \
     "nothing is left outstanding"

# The payment is a real record against the same customer, not a flag.
t_ge "$(pg "SELECT count(*) FROM account_payment WHERE partner_id=$CO")" "1" \
     "a payment record exists against the company"

# -------------------------------------------------------------------------
sec "6. the contract is stopped"
# -------------------------------------------------------------------------
# Ending a tenancy is two facts: the line stops on a date, and the contract is
# closed. Both go through the API the screen uses.
call rental.contract.line write "[[$LINE],{\"date_end\":\"2026-03-31\",\"state\":\"ended\"}]" >/dev/null
call rental.contract write "[[${RC:-0}],{\"state\":\"closed\"}]" >/dev/null

t_eq "closed" "$(pg "SELECT state FROM rental_contract WHERE id=${RC:-0}")" "the contract is closed"
t_eq "ended"  "$(pg "SELECT state FROM rental_contract_line WHERE id=$LINE")" "the line is ended"

# And the unit is available again — otherwise it can never be re-let.
t_eq "available" "$(pg "SELECT state FROM rental_unit WHERE id=$UNIT")" \
     "the unit is released back to available"

# -------------------------------------------------------------------------
sec "7. a stopped contract never bills again"
# -------------------------------------------------------------------------
# The one that matters. Run the billing well past the end date and assert
# nothing new appears — a closed tenancy that keeps invoicing is the failure
# this whole section exists to catch.
BEFORE_N=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$CO AND move_type='out_invoice'")
runbill 2026-04-15 >/dev/null
runbill 2026-05-15 >/dev/null
runbill 2026-06-15 >/dev/null
t_eq "$BEFORE_N" "$(pg "SELECT count(*) FROM account_move WHERE partner_id=$CO AND move_type='out_invoice'")" \
     "three more billing runs raised NO further invoice"

# The paid invoice is still there and still paid — stopping a contract must
# not disturb the history it already produced.
t_eq "1" "$(pg "SELECT count(*) FROM account_move WHERE id=${INV:-0}")" "the invoice survives the closure"
case "$(pg "SELECT payment_state FROM account_move WHERE id=${INV:-0}")" in
    paid|in_payment) ok "and is still marked paid" ;;
    *)               no "the closed contract disturbed its invoice's payment state" ;;
esac

# -------------------------------------------------------------------------
sec "8. the customer's history is intact, so they cannot be deleted"
# -------------------------------------------------------------------------
# The end of the journey: a customer with a year of history is not something
# to delete by accident. This is the rule tests/integration/core/contact-delete
# checks in detail; here it is checked in situ, on a real customer.
OUT=$(call res.partner unlink "[[$CO]]")
if has_error "$OUT"; then ok "the company cannot be deleted while it has documents"
else no "a company with an invoice and a contract was DELETED"; fi
t_contains "$OUT" "Archive it instead" "and the user is told what to do instead"

# -------------------------------------------------------------------------
sec "9. and the same first step THROUGH THE SCREEN"
# -------------------------------------------------------------------------
# Everything above went through the API, which proves the billing and the money
# and NOTHING about the form. A Customer combobox that cannot find the company,
# or that loses the selection on save, passes every check above — and that is
# exactly what was reported twice while this suite was green.
#
# So: open a new rental contract in a real browser, find THIS company in the
# combobox among all the others, save, and confirm the database got it.
CHROME=${CHROME_PATH:-/usr/bin/google-chrome}
if [ ! -x "$CHROME" ] || [ ! -d node_modules/puppeteer-core ]; then
    echo "    NOTE  no Chrome or puppeteer-core here — skipping the on-screen check"
else
    UIREF="${PFX}-UI-001"
    UIOUT=$(SHOT=/tmp/lifecycle_ui.png BASE="$BASE" DBN="$DBN" timeout 200 \
            node tests/lib/render_contract.mjs "${PFX} Sunrise Traders Sdn Bhd" "$UIREF" 2>&1)
    URC=$?
    echo "$UIOUT" | sed 's/^/      /'
    if [ "$URC" -eq 0 ]; then ok "a contract can be started from the screen for this customer"
    else no "starting a contract from the screen failed (see above)"; fi

    # Assert it independently of the driver's own reporting.
    t_eq "$CO" "$(pg "SELECT partner_id FROM rental_contract WHERE name='$UIREF'")" \
         "the contract saved from the SCREEN carries the right customer"
    t_eq "quarterly" "$(pg "SELECT billing_period FROM rental_contract WHERE name='$UIREF'")" \
         "and the billing period chosen in the combobox"
    pg "DELETE FROM rental_contract WHERE name='$UIREF'" >/dev/null 2>&1
fi

verdict

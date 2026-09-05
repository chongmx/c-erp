#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# FUNCTIONAL JOURNEY 07 — MONEY IN AND OUT.  (tests/docs/test-plan.md §4)
#
#   invoice -> PART payment -> the rest -> vendor bill -> paid ->
#   bank statement -> reconciled -> analytic items agree with the ledger
#
# The largest hole the coverage audit found. The other journeys settle invoices
# through `account.move.action_register_payment`, which means the PAYMENT MODEL
# ITSELF — the screen under Accounting → Customers → Payments — had never been
# exercised: not created, not posted, not allocated, never read back.
#
# Partial payment is the case that matters most and is least tested. A system
# that handles "pay it all" and mishandles "pay half" looks fine in every demo
# and is wrong on the first real Tuesday.
#
# Everything is prefixed MI- / 'MI ' and removed on the way out.
# =============================================================
auth_or_die

M=1000000
TOTAL=800            # invoice total, majors
PART=300             # first instalment

cleanup() {
    pg "DELETE FROM account_analytic_line WHERE name LIKE 'MI %'" >/dev/null
    pg "DELETE FROM account_analytic_account WHERE name LIKE 'MI %'" >/dev/null
    pg "DELETE FROM account_bank_statement_line WHERE payment_ref LIKE 'MI-%'" >/dev/null
    pg "DELETE FROM account_bank_statement WHERE name LIKE 'MI-%'" >/dev/null
    pg "DELETE FROM account_move_line WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'MI %')" >/dev/null
    pg "DELETE FROM account_payment   WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'MI %')" >/dev/null
    pg "DELETE FROM account_move      WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'MI %')" >/dev/null
    pg "DELETE FROM res_partner       WHERE name LIKE 'MI %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

JRN_SALE=$(pg "SELECT id FROM account_journal WHERE type='sale' AND company_id=1 ORDER BY id LIMIT 1")
JRN_BANK=$(pg "SELECT id FROM account_journal WHERE type='bank' AND company_id=1 ORDER BY id LIMIT 1")
ARECV=$(pg "SELECT id FROM account_account WHERE account_type='asset_receivable' AND company_id=1 ORDER BY id LIMIT 1")
APAY=$(pg  "SELECT id FROM account_account WHERE account_type='liability_payable' AND company_id=1 ORDER BY id LIMIT 1")
AINC=$(pg  "SELECT id FROM account_account WHERE account_type IN ('income','income_other') AND company_id=1 ORDER BY id LIMIT 1")
t_nonempty "$JRN_BANK" "a bank journal exists"
t_nonempty "$ARECV"    "a receivable account exists"
[ -z "$JRN_BANK" ] || [ -z "$ARECV" ] && { verdict; exit 1; }

CUST=$(pgid "INSERT INTO res_partner (name, active, company_id) VALUES ('MI Customer', true, 1) RETURNING id")
VEND=$(pgid "INSERT INTO res_partner (name, active, company_id) VALUES ('MI Vendor', true, 1) RETURNING id")

mkinvoice() {  # mkinvoice <partner> <type> <total-majors> <name> -> id
    local mv acc
    if [ "$2" = "out_invoice" ]; then acc=$ARECV; else acc=$APAY; fi
    mv=$(pgid "INSERT INTO account_move
        (name, move_type, state, date, invoice_date, due_date, journal_id, company_id,
         partner_id, amount_untaxed, amount_tax, amount_total, amount_residual, payment_state)
        VALUES ('$4','$2','draft',CURRENT_DATE,CURRENT_DATE,CURRENT_DATE,$JRN_SALE,1,$1,
                $(( $3 * M )),0,$(( $3 * M )),$(( $3 * M )),'not_paid') RETURNING id")
    if [ "$2" = "out_invoice" ]; then
        pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,partner_id,name,date,debit,credit)
            VALUES ($mv,$acc,$JRN_SALE,1,$1,'MI receivable',CURRENT_DATE,$(( $3 * M )),0)" >/dev/null
        pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,partner_id,name,date,debit,credit)
            VALUES ($mv,$AINC,$JRN_SALE,1,$1,'MI income',CURRENT_DATE,0,$(( $3 * M )))" >/dev/null
    else
        pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,partner_id,name,date,debit,credit)
            VALUES ($mv,$acc,$JRN_SALE,1,$1,'MI payable',CURRENT_DATE,0,$(( $3 * M )))" >/dev/null
        pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,partner_id,name,date,debit,credit)
            VALUES ($mv,$AINC,$JRN_SALE,1,$1,'MI expense',CURRENT_DATE,$(( $3 * M )),0)" >/dev/null
    fi
    pg "UPDATE account_move SET state='posted' WHERE id=$mv" >/dev/null
    echo "$mv"
}

# ------------------------------------------------------------------
sec "1. an invoice the customer will pay in two goes"
# ------------------------------------------------------------------
INV=$(mkinvoice "$CUST" out_invoice "$TOTAL" "MI-INV-1")
t_nonempty "$INV" "the invoice exists"
t_eq "posted" "$(pg "SELECT state FROM account_move WHERE id=$INV")" "and is posted"
t_eq "$((TOTAL * M))" "$(pg "SELECT amount_residual FROM account_move WHERE id=$INV")" \
     "the whole $TOTAL is outstanding"

# ------------------------------------------------------------------
sec "2. a PARTIAL payment"
# ------------------------------------------------------------------
# The case that separates a real implementation from a demo. Paying part of an
# invoice must reduce the residual by exactly that much and leave the invoice
# open — not round it to paid, and not leave it untouched.
P1=$(call_k account.move action_register_payment "[[$INV]]" "\"amount\":$PART")
has_error "$P1" && no "registering a partial payment failed: $(echo "$P1" | head -c 200)"

RESID=$(pg "SELECT amount_residual FROM account_move WHERE id=$INV")
STATE=$(pg "SELECT payment_state FROM account_move WHERE id=$INV")
echo "    residual after paying $PART of $TOTAL: $RESID  ($STATE)"
t_eq "$(( (TOTAL - PART) * M ))" "${RESID%%.*}" "the residual fell by exactly the amount paid"
case "$STATE" in
    partial|in_payment|not_paid) ok "the invoice is still open ($STATE)" ;;
    paid) no "a $PART payment against a $TOTAL invoice marked it fully PAID" ;;
    *)    no "unexpected payment_state '$STATE'" ;;
esac

# The payment must exist as a record of its own — this is the model the whole
# journey was written for.
PAY=$(pg "SELECT id FROM account_payment WHERE partner_id=$CUST ORDER BY id DESC LIMIT 1")
t_nonempty "$PAY" "a payment record was created"
if [ -n "$PAY" ]; then
    t_eq "$((PART * M))" "$(pg "SELECT amount FROM account_payment WHERE id=$PAY")" \
         "the payment is for the amount requested"
    t_eq "inbound" "$(pg "SELECT payment_type FROM account_payment WHERE id=$PAY")" \
         "it is inbound (money coming in)"
    t_eq "customer" "$(pg "SELECT partner_type FROM account_payment WHERE id=$PAY")" \
         "and against a customer"
    t_eq "$CUST" "$(pg "SELECT partner_id FROM account_payment WHERE id=$PAY")" "for the right partner"
    # It must be readable through the screen's own endpoint, not only in SQL.
    RD=$(call account.payment search_read "[[[\"partner_id\",\"=\",$CUST]],[\"amount\",\"payment_type\",\"state\"]]")
    has_error "$RD" && no "account.payment search_read failed: $(echo "$RD" | head -c 160)" \
                    || ok "the payment lists through account.payment"
fi

# ------------------------------------------------------------------
sec "3. paying the rest"
# ------------------------------------------------------------------
P2=$(call account.move action_register_payment "[[$INV]]")
has_error "$P2" && no "the closing payment failed: $(echo "$P2" | head -c 200)"
t_eq "0" "$(pg "SELECT amount_residual FROM account_move WHERE id=$INV")" "nothing is left outstanding"
case "$(pg "SELECT payment_state FROM account_move WHERE id=$INV")" in
    paid|in_payment) ok "the invoice now reads as paid" ;;
    *) no "the invoice is not paid after settling the balance" ;;
esac
t_eq "2" "$(pg "SELECT count(*) FROM account_payment WHERE partner_id=$CUST")" \
     "two payments are recorded against this customer"
t_eq "$((TOTAL * M))" "$(pg "SELECT COALESCE(SUM(amount),0) FROM account_payment WHERE partner_id=$CUST")" \
     "and together they add up to the invoice"

# Paying an already-settled invoice must be refused, not silently accepted.
AGAIN=$(call account.move action_register_payment "[[$INV]]")
if has_error "$AGAIN"; then ok "a fully paid invoice refuses another payment"
else no "a paid invoice accepted a further payment — residual now $(pg "SELECT amount_residual FROM account_move WHERE id=$INV")"; fi

# ------------------------------------------------------------------
sec "4. money going the other way"
# ------------------------------------------------------------------
BILL=$(mkinvoice "$VEND" in_invoice 450 "MI-BILL-1")
t_nonempty "$BILL" "a vendor bill exists"
PB=$(call account.move action_register_payment "[[$BILL]]")
has_error "$PB" && no "paying the vendor failed: $(echo "$PB" | head -c 200)"
t_eq "0" "$(pg "SELECT amount_residual FROM account_move WHERE id=$BILL")" "the bill is settled"

VPAY=$(pg "SELECT id FROM account_payment WHERE partner_id=$VEND ORDER BY id DESC LIMIT 1")
t_nonempty "$VPAY" "a vendor payment record was created"
# Direction is the assertion: an outbound payment recorded as inbound balances
# perfectly and reports the company's cash position backwards.
t_eq "outbound" "$(pg "SELECT payment_type FROM account_payment WHERE id=$VPAY")" \
     "it is OUTBOUND, not inbound"
t_eq "supplier" "$(pg "SELECT partner_type FROM account_payment WHERE id=$VPAY")" "and against a supplier"

# ------------------------------------------------------------------
sec "5. the bank statement"
# ------------------------------------------------------------------
ST=$(pgid "INSERT INTO account_bank_statement (name, date, journal_id, company_id)
           VALUES ('MI-ST-1', CURRENT_DATE, $JRN_BANK, 1) RETURNING id")
t_nonempty "$ST" "a bank statement was created"
INV2=$(mkinvoice "$CUST" out_invoice 275 "MI-INV-2")
LINE=$(pgid "INSERT INTO account_bank_statement_line
             (statement_id, date, name, payment_ref, partner_id, amount, is_reconciled, company_id)
             VALUES ($ST, CURRENT_DATE, 'MI line', 'MI-REF-1', $CUST, $((275 * M)), false, 1)
             RETURNING id")
t_nonempty "$LINE" "with a line to match"
# `boolean::text` gives 'true'/'false', not 't'/'f' — the psql *display* form is
# not the cast form. Compared as 0/1 to sidestep it entirely.
t_eq "0" "$(pg "SELECT is_reconciled::int FROM account_bank_statement_line WHERE id=$LINE")" \
     "which starts unreconciled"

REC=$(call account.bank.statement.line reconcile "[{\"line_id\":$LINE,\"move_id\":$INV2}]")
has_error "$REC" && no "reconcile failed: $(echo "$REC" | head -c 200)"
t_eq "1" "$(pg "SELECT is_reconciled::int FROM account_bank_statement_line WHERE id=$LINE")" \
     "the statement line is reconciled"
t_eq "0" "$(pg "SELECT amount_residual FROM account_move WHERE id=$INV2")" \
     "and reconciling cleared the invoice"

# Nothing may be left half-matched: a line marked reconciled that points at no
# move is the state that makes a bank reconciliation untrustworthy.
DANGLE=$(pg "SELECT count(*) FROM account_bank_statement_line l
              WHERE l.statement_id=$ST AND l.is_reconciled
                AND (l.reconciled_move_id IS NULL
                     OR NOT EXISTS (SELECT 1 FROM account_move m WHERE m.id=l.reconciled_move_id))")
t_eq "0" "${DANGLE:-0}" "no reconciled line points at a missing move"

# ------------------------------------------------------------------
sec "6. analytic accounting agrees with the ledger"
# ------------------------------------------------------------------
AA=$(call account.analytic.account create '[{"name":"MI Analytic"}]' | rid)
t_nonempty "$AA" "an analytic account was created"
if [ -n "$AA" ]; then
    ML=$(pg "SELECT id FROM account_move_line WHERE move_id=$INV AND debit > 0 ORDER BY id LIMIT 1")
    AL=$(call account.analytic.line create \
        "[{\"name\":\"MI item\",\"account_id\":$AA,\"amount\":$TOTAL,\"date\":\"$(date +%Y-%m-%d)\",\"move_line_id\":$ML}]" | rid)
    t_nonempty "$AL" "an analytic item was posted to it"
    t_eq "$((TOTAL * M))" "$(pg "SELECT COALESCE(SUM(amount),0) FROM account_analytic_line WHERE account_id=$AA")" \
         "the analytic account totals what was posted to it"
    # An analytic item must point at a journal item that exists, or the
    # analytic report and the ledger tell different stories.
    ORPH=$(pg "SELECT count(*) FROM account_analytic_line l
                WHERE l.account_id=$AA AND l.move_line_id IS NOT NULL
                  AND NOT EXISTS (SELECT 1 FROM account_move_line m WHERE m.id=l.move_line_id)")
    t_eq "0" "${ORPH:-0}" "no analytic item points at a missing journal item"
fi

# ------------------------------------------------------------------
sec "7. the invariants"
# ------------------------------------------------------------------
BAL=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0)
            FROM account_move_line l JOIN account_move m ON m.id=l.move_id
           WHERE m.state='posted'")
t_eq "0" "${BAL%%.*}" "the posted ledger balances"

OWED=$(pg "SELECT COALESCE(SUM(amount_residual),0) FROM account_move
            WHERE partner_id=$CUST AND state='posted' AND move_type='out_invoice'")
t_eq "0" "${OWED%%.*}" "the customer owes nothing"
DUE=$(pg "SELECT COALESCE(SUM(amount_residual),0) FROM account_move
           WHERE partner_id=$VEND AND state='posted' AND move_type='in_invoice'")
t_eq "0" "${DUE%%.*}" "and nothing is owed to the vendor"

# Every payment must have moved the ledger. A payment row with no journal entry
# behind it is money that exists on one screen and not in the accounts.
NOMOVE=$(pg "SELECT count(*) FROM account_payment p
              WHERE p.partner_id IN ($CUST,$VEND)
                AND (p.move_id IS NULL
                     OR NOT EXISTS (SELECT 1 FROM account_move m WHERE m.id=p.move_id))")
t_eq "0" "${NOMOVE:-0}" "every payment is backed by a journal entry"

verdict

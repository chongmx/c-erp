#!/bin/bash
# --- harness ---------------------------------------------------------------
# Walk up for CMakeLists.txt rather than counting `../`, so this test behaves
# the same whether the runner invoked it or you ran it directly, and so it can
# be nested a folder deeper without a preamble edit.
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Rental phase 5 — recurring billing (docs/054 §2 phase 5).
#
# The suite that matters most in this module. It asserts, in order:
#
#   1. period arithmetic does not DRIFT on a 31st anchor
#   2. billing IN ADVANCE — the invoice precedes the period
#   3. one customer, three start dates -> three invoices, three due dates
#   4. two units sharing a due date -> ONE invoice, two lines
#   5. running the same date twice does NOT double-bill
#   6. NEGATIVE CONTROL: without the UNIQUE constraint it DOES double-bill
#   7. tax reaches the invoice and the entry balances
#   8. a failure on one customer does not affect another
#   9. walk-ins (billing_mode='manual') are never auto-billed
#  10. advance credit is consumed by the generated invoice
#
# (6) is the point of the whole file. An idempotency test that has never
# seen the failure it prevents is asserting that today's code does what
# today's code does.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
M=1000000

pg()  { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
pgm() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null; }
ok()  { echo "    PASS  $1"; }
no()  { echo "    FAIL  $1"; FAILED=1; }

cleanup() {
    pg "DELETE FROM rental_invoice_link WHERE contract_line_id IN
          (SELECT id FROM rental_contract_line WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE 'BILLTEST%'))" >/dev/null
    pg "DELETE FROM account_move_line WHERE move_id IN
          (SELECT id FROM account_move WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE 'BILLTEST%'))" >/dev/null
    pg "DELETE FROM account_partial_reconcile WHERE move_id IN
          (SELECT id FROM account_move WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE 'BILLTEST%'))" >/dev/null
    pg "DELETE FROM account_payment WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'BILLTEST%')" >/dev/null
    pg "DELETE FROM account_move WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'BILLTEST%')" >/dev/null
    pg "DELETE FROM rental_contract_line WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'BILLTEST%')" >/dev/null
    pg "DELETE FROM rental_event WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'BILLTEST%')" >/dev/null
    pg "DELETE FROM rental_contract WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'BILLTEST%')" >/dev/null
    pg "DELETE FROM rental_unit WHERE code LIKE 'BT-%'" >/dev/null
    pg "DELETE FROM res_partner WHERE name LIKE 'BILLTEST%'" >/dev/null
}
cleanup

# The /rental/ routes authenticate (docs/061), so the cookie from this
# sign-in is presented on every call below.
CK=/tmp/vrb_cookie.txt
cat > /tmp/vrb_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/vrb_auth.json > /tmp/vrb_auth_out.json
grep -q '"session_id"' /tmp/vrb_auth_out.json || { echo "cannot authenticate"; exit 1; }

runbill() {   # $1 = as-of date -> echoes the JSON result
    curl -s -b "$CK" -X POST "$BASE/rental/billing/run?date=$1"
}

echo "############ 1. period arithmetic does not drift ############"
# date + interval '1 month' walks a 31st anchor backwards to the 28th and
# never returns. rental_next_period clamps instead.
D1=$(pg "SELECT rental_next_period('2026-01-31'::date, 31, 1)")
D2=$(pg "SELECT rental_next_period('$D1'::date, 31, 1)")
D3=$(pg "SELECT rental_next_period('$D2'::date, 31, 1)")
echo "    Jan 31 -> $D1 -> $D2 -> $D3"
[ "$D1" = "2026-02-28" ] && ok "clamps to the short month" || no "got $D1"
[ "$D2" = "2026-03-31" ] && ok "RECOVERS the 31st (no drift)" || no "drifted to $D2"
[ "$D3" = "2026-04-30" ] && ok "clamps again in April"       || no "got $D3"
NAIVE=$(pg "SELECT ('2026-01-31'::date + interval '1 month' + interval '1 month')::date")
echo "    naive date+interval would have given: $NAIVE"
[ "$NAIVE" != "$D2" ] && ok "the naive form really does drift — this guard earns its place" \
                      || no "no difference; the test proves nothing"

echo
echo "############ setup ############"
P1=$(pg "INSERT INTO res_partner (name,is_company,active) VALUES ('BILLTEST One',false,true) RETURNING id")
P2=$(pg "INSERT INTO res_partner (name,is_company,active) VALUES ('BILLTEST Two',false,true) RETURNING id")
TAX=$(pg "SELECT id FROM account_tax WHERE type_tax_use='sale' AND active ORDER BY id LIMIT 1")
mkunit() { pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('$1','probe','available',1) RETURNING id"; }
U1=$(mkunit BT-01); U2=$(mkunit BT-02); U3=$(mkunit BT-03); U4=$(mkunit BT-04); U5=$(mkunit BT-05)

# $1 partner $2 unit $3 period_start $4 rate $5 mode $6 anchor
mkline() {
    pg "INSERT INTO rental_contract_line
          (partner_id,unit_id,date_start,unit_price,tax_ids_json,state,
           billing_mode,billing_anchor_day,billing_months,billing_lead_days,
           next_period_start,company_id)
        VALUES ($1,$2,'$3',$(( $4 * M )),'[$TAX]','active','$5',$6,1,7,'$3',1) RETURNING id"
}
echo "    partners $P1 / $P2, tax $TAX"

echo
echo "############ 2-3. three start dates -> three invoices ############"
L1=$(mkline $P1 $U1 2026-09-01 100 recurring 1)
L2=$(mkline $P1 $U2 2026-09-10 200 recurring 10)
L3=$(mkline $P1 $U3 2026-09-20 300 recurring 20)
echo "    lines $L1 (Sep 1), $L2 (Sep 10), $L3 (Sep 20)"

# As-of Aug 26: only the Sep 1 period is within its 7-day lead.
R=$(runbill 2026-08-26)
echo "    run 2026-08-26 -> $R"
N=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$P1 AND move_type='out_invoice'")
[ "$N" = "1" ] && ok "only the unit due within the lead window was billed" || no "$N invoices"

DUE=$(pg "SELECT to_char(due_date,'YYYY-MM-DD') FROM account_move WHERE partner_id=$P1 LIMIT 1")
IDATE=$(pg "SELECT to_char(invoice_date,'YYYY-MM-DD') FROM account_move WHERE partner_id=$P1 LIMIT 1")
echo "    invoice dated $IDATE, due $DUE"
[ "$IDATE" \< "$DUE" ] && ok "billed IN ADVANCE — invoice precedes the period" || no "invoice not in advance"

R=$(runbill 2026-09-16)   # brings Sep 10 and Sep 20 into range
echo "    run 2026-09-16 -> $R"
N=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$P1 AND move_type='out_invoice'")
DUES=$(pgm "SELECT DISTINCT to_char(due_date,'MM-DD') FROM account_move WHERE partner_id=$P1 ORDER BY 1" | tr '\n' ' ')
echo "    invoices=$N  due dates: $DUES"
[ "$N" = "3" ] && ok "three start dates produced three invoices" || no "$N invoices, expected 3"

echo
echo "############ 4. two units sharing a due date -> ONE invoice ############"
L4=$(mkline $P2 $U4 2026-09-01 150 recurring 1)
L5=$(mkline $P2 $U5 2026-09-01 250 recurring 1)
R=$(runbill 2026-08-26)
N=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$P2 AND move_type='out_invoice'")
LN=$(pg "SELECT count(*) FROM account_move_line l JOIN account_move m ON m.id=l.move_id
          WHERE m.partner_id=$P2 AND l.credit>0 AND l.tax_line_id IS NULL AND l.display_type=''")
echo "    invoices=$N  product lines=$LN"
[ "$N" = "1" ]  && ok "combined onto one invoice" || no "$N invoices"
[ "$LN" = "2" ] && ok "two units, two lines"      || no "$LN lines"

echo
echo "############ 5. re-running the same date does NOT double-bill ############"
BEFORE=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$P2")
LINKS=$(pg "SELECT count(*) FROM rental_invoice_link WHERE contract_line_id IN ($L4,$L5)")
# Rewind so the same period comes due again — exactly what a second cron
# run in the same window does.
pg "UPDATE rental_contract_line SET next_period_start='2026-09-01' WHERE id IN ($L4,$L5)" >/dev/null
R=$(runbill 2026-08-26)
AFTER=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$P2")
echo "    invoices $BEFORE -> $AFTER   response: $R"
[ "$AFTER" = "$BEFORE" ] && ok "second run created no invoice" || no "double-billed: $BEFORE -> $AFTER"
printf '%s' "$R" | grep -q '"groups_skipped":1' && ok "reported as skipped, not failed" \
                                                || no "not reported as a skip"

echo
echo "############ 6. NEGATIVE CONTROL — without the constraint it double-bills ############"
# Proves the test above is testing the constraint and not a coincidence.
pg "ALTER TABLE rental_invoice_link DROP CONSTRAINT rental_invoice_link_uniq" >/dev/null
pg "UPDATE rental_contract_line SET next_period_start='2026-09-01' WHERE id IN ($L4,$L5)" >/dev/null
B2=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$P2")
runbill 2026-08-26 > /dev/null
A2=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$P2")
echo "    constraint dropped: invoices $B2 -> $A2"
[ "$A2" -gt "$B2" ] && ok "WITHOUT the constraint it double-bills — the guard is load-bearing" \
                    || no "no double-bill even without the constraint; check 5 proves nothing"
# Put it back, removing the duplicate rows first.
pg "DELETE FROM rental_invoice_link a USING rental_invoice_link b
     WHERE a.id > b.id AND a.contract_line_id = b.contract_line_id
       AND a.period_start = b.period_start" >/dev/null
pg "ALTER TABLE rental_invoice_link
      ADD CONSTRAINT rental_invoice_link_uniq UNIQUE (contract_line_id, period_start)" >/dev/null
RESTORED=$(pg "SELECT count(*) FROM pg_constraint WHERE conname='rental_invoice_link_uniq'")
[ "$RESTORED" = "1" ] && ok "constraint restored" || no "CONSTRAINT NOT RESTORED — fix before deploying"

echo
echo "############ 7. tax reaches the invoice and the entry balances ############"
MV=$(pg "SELECT id FROM account_move WHERE partner_id=$P1 ORDER BY id LIMIT 1")
UNT=$(pg "SELECT amount_untaxed FROM account_move WHERE id=$MV")
TXA=$(pg "SELECT amount_tax     FROM account_move WHERE id=$MV")
TOT=$(pg "SELECT amount_total   FROM account_move WHERE id=$MV")
BAL=$(pg "SELECT COALESCE(SUM(debit),0)-COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$MV")
TXL=$(pg "SELECT count(*) FROM account_move_line WHERE move_id=$MV AND tax_line_id IS NOT NULL")
echo "    untaxed=$UNT tax=$TXA total=$TOT  tax lines=$TXL  balance=$BAL"
[ "$UNT" = "$((100*M))" ]                && ok "untaxed 100.00"            || no "untaxed is $UNT"
[ "$TXA" = "$((15*M))" ]                 && ok "15% tax = 15.00 computed"  || no "tax is $TXA"
[ "$TOT" = "$((115*M))" ]                && ok "total 115.00"              || no "total is $TOT"
[ "$TXL" -ge 1 ]                         && ok "tax line posted to the ledger" || no "no tax line"
[ "$BAL" = "0" ]                         && ok "journal entry balances"     || no "out of balance by $BAL"

echo
echo "############ 8. a broken customer does not stop the others ############"
# P2's line is moved to a company that has no chart of accounts, so its
# group throws on the receivable lookup. P1 must still be billed in the
# same run, and the failure must be REPORTED rather than swallowed.
#
# An earlier version set unit_price=-1, which the engine happily billed —
# so the test passed while proving nothing, because no group had failed.
# The assertion below now requires groups_failed >= 1 as well.
pg "UPDATE rental_contract_line SET next_period_start='2026-10-01' WHERE id IN ($L1,$L4)" >/dev/null
pg "UPDATE rental_contract_line SET company_id=999, next_period_start='2026-10-01' WHERE id=$L4" >/dev/null
echo "    L4 now: company=$(pg "SELECT company_id FROM rental_contract_line WHERE id=$L4")" \
     "period=$(pg "SELECT to_char(next_period_start,'YYYY-MM-DD') FROM rental_contract_line WHERE id=$L4")" \
     "state=$(pg "SELECT state FROM rental_contract_line WHERE id=$L4")" \
     "mode=$(pg "SELECT billing_mode FROM rental_contract_line WHERE id=$L4")"
SEL=$(pg "SELECT count(*) FROM rental_contract_line l
           WHERE l.id=$L4 AND l.state='active' AND l.billing_mode='recurring'
             AND l.next_period_start IS NOT NULL
             AND l.next_period_start - l.billing_lead_days <= '2026-09-26'::date
             AND (l.date_end IS NULL OR l.next_period_start <= l.date_end)")
echo "    the engine's own predicate selects L4: $SEL"
BP1=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$P1")
R=$(runbill 2026-09-26)
AP1=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$P1")
echo "    P1 invoices $BP1 -> $AP1   response: $R"
printf '%s' "$R" | grep -qE '"groups_failed":[1-9]' \
    && ok "the broken group really did fail (premise holds)" \
    || no "no group failed — this test proves nothing"
[ "$AP1" -gt "$BP1" ] && ok "the healthy customer was still billed" || no "one bad group stopped the run"
BADINV=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$P2 AND company_id=999")
[ "$BADINV" = "0" ] && ok "the failed group left no half-written invoice" || no "$BADINV partial invoices"
pg "UPDATE rental_contract_line SET company_id=1 WHERE id=$L4" >/dev/null

echo
echo "############ 9. walk-ins are never auto-billed ############"
P3=$(pg "INSERT INTO res_partner (name,is_company,active) VALUES ('BILLTEST Walk',false,true) RETURNING id")
U6=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('BT-06','walkin','available',1) RETURNING id")
LW=$(mkline $P3 $U6 2026-09-01 99 manual 1)
runbill 2026-08-26 > /dev/null
NW=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$P3")
echo "    walk-in invoices: $NW"
[ "$NW" = "0" ] && ok "billing_mode='manual' is not picked up by the cron" || no "walk-in was auto-billed"

echo
echo "############ 10. advance credit is consumed by the new invoice ############"
JRN=$(pg "SELECT id FROM account_journal WHERE type IN ('bank','cash') ORDER BY id LIMIT 1")
P4=$(pg "INSERT INTO res_partner (name,is_company,active) VALUES ('BILLTEST Adv',false,true) RETURNING id")
U7=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('BT-07','adv','available',1) RETURNING id")
pg "INSERT INTO account_payment (date,journal_id,partner_id,company_id,amount,
                                 payment_type,partner_type,state,memo)
    VALUES ('2026-08-20',$JRN,$P4,1,$((500*M)),'inbound','customer','posted','advance')" >/dev/null
UNAL=$(pg "SELECT amount_unallocated FROM account_payment_unallocated
            WHERE partner_id=$P4 LIMIT 1")
echo "    advance on account: $UNAL"
LA=$(mkline $P4 $U7 2026-09-01 100 recurring 1)
runbill 2026-08-26 > /dev/null
RESID=$(pg "SELECT amount_residual FROM account_move WHERE partner_id=$P4 LIMIT 1")
PSTATE=$(pg "SELECT payment_state FROM account_move WHERE partner_id=$P4 LIMIT 1")
LEFT=$(pg "SELECT amount_unallocated FROM account_payment_unallocated WHERE partner_id=$P4 LIMIT 1")
echo "    invoice residual=$RESID state=$PSTATE  credit left=$LEFT"
[ "$RESID" = "0" ]     && ok "the advance settled the invoice automatically" || no "residual is $RESID"
[ "$PSTATE" = "paid" ] && ok "marked paid"                                    || no "state is $PSTATE"
[ "$LEFT" = "$((385*M))" ] && ok "credit drawn down by exactly 115.00" || no "credit left is $LEFT"

echo
echo "############ cleanup ############"
cleanup
LEFTOVER=$(pg "SELECT count(*) FROM res_partner WHERE name LIKE 'BILLTEST%'")
[ "$LEFTOVER" = "0" ] && ok "test data removed" || no "$LEFTOVER partners leaked"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

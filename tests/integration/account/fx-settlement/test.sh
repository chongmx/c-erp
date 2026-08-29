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
# The two loose ends, verified through the API the UI now calls:
#   1. FX settlement — action_register_payment with
#      amount_received_base derives the rate and posts to 7900.
#   2. Invoice tax picker — a line's tax_ids_json drives the
#      generated tax lines.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
M=1000000

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cat > /tmp/fx_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/fx_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }

call() {
    cat > /tmp/fx.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$1","method":"$2","args":$3,
 "kwargs":{$4"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/fx.json
}

PARTNER=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
JOURNAL=$(pg "SELECT id FROM account_journal WHERE type IN ('bank','cash') ORDER BY id LIMIT 1")
AR=$(pg "SELECT id FROM account_account WHERE code='1200' LIMIT 1")
REV=$(pg "SELECT id FROM account_account WHERE code='4000' LIMIT 1")
USD=$(pg "SELECT id FROM res_currency WHERE name='USD'")
pg "UPDATE res_currency SET rate=4700000, active=TRUE WHERE id=$USD" >/dev/null

echo "############ 1. FX settlement: 100 USD booked @4.70, bank pays 448.50 MYR ############"
INV=$(pg "INSERT INTO account_move (name,move_type,state,date,journal_id,partner_id,company_id,
                                    currency_id,currency_rate,amount_untaxed,amount_tax,
                                    amount_total,amount_residual,payment_state)
          VALUES ('FXTEST/'||nextval('account_move_id_seq')::text,'out_invoice','posted',CURRENT_DATE,
                  $JOURNAL,$PARTNER,1,$USD,4700000,$((100*M)),0,$((100*M)),$((100*M)),'not_paid')
          RETURNING id")
pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,debit,credit,display_type)
    VALUES ($INV,$AR,$JOURNAL,1,CURRENT_DATE,'AR',$((100*M)),0,'')" >/dev/null
# quantity/price_unit are set because the tax engine computes from them,
# not from the credit column — a line with only a credit yields zero tax.
pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,debit,credit,
                                   quantity,price_unit,display_type)
    VALUES ($INV,$REV,$JOURNAL,1,CURRENT_DATE,'Rev',0,$((100*M)),$M,$((100*M)),'')" >/dev/null
echo "    invoice $INV: 100.00 USD, booked rate 4.700000"

R=$(call account.move action_register_payment "[[$INV]]" \
     "\"payment_date\":\"$(date +%F)\",\"journal_id\":$JOURNAL,\"amount\":100,\"memo\":\"FX probe\",\"amount_received_base\":448.50,")
echo "    response: $(printf '%s' "$R" | head -c 200)"

ALLOC=$(pg "SELECT count(*) FROM account_partial_reconcile WHERE move_id=$INV")
FXD=$(pg "SELECT fx_diff FROM account_partial_reconcile WHERE move_id=$INV LIMIT 1")
BASEAMT=$(pg "SELECT amount_base FROM account_partial_reconcile WHERE move_id=$INV LIMIT 1")
echo "    allocations=$ALLOC  amount_base=$BASEAMT  fx_diff=$FXD"
[ "$ALLOC" = "1" ]              && ok "allocation row created" || no "expected 1 allocation, got $ALLOC"
[ "$BASEAMT" = "$((448*M+500000))" ] && ok "base amount 448.50 (derived rate 4.485)" || no "amount_base is $BASEAMT"
[ "$FXD" = "-21500000" ]        && ok "realised FX loss -21.50 recorded" || no "fx_diff is $FXD"

RESID=$(pg "SELECT amount_residual FROM account_move WHERE id=$INV")
PSTATE=$(pg "SELECT payment_state FROM account_move WHERE id=$INV")
echo "    residual=$RESID state=$PSTATE"
[ "$RESID" = "0" ]      && ok "invoice fully settled (exact zero, no epsilon)" || no "residual is $RESID"
[ "$PSTATE" = "paid" ]  && ok "payment_state = paid" || no "state is $PSTATE"

PMTMOVE=$(pg "SELECT move_id FROM account_payment WHERE id=(SELECT payment_id FROM account_partial_reconcile WHERE move_id=$INV LIMIT 1)")
FXLINE=$(pg "SELECT count(*) FROM account_move_line l JOIN account_account a ON a.id=l.account_id
              WHERE l.move_id=$PMTMOVE AND a.code='7900'")
echo "    FX lines on the PAYMENT entry $PMTMOVE: $FXLINE"
[ "$FXLINE" -ge 1 ] && ok "FX posted to 7900 on the payment entry" || no "no 7900 line found"

INVFX=$(pg "SELECT count(*) FROM account_move_line l JOIN account_account a ON a.id=l.account_id
             WHERE l.move_id=$INV AND a.code='7900'")
[ "$INVFX" = "0" ] && ok "customer invoice untouched by FX (they owe 100 USD regardless)" \
                   || no "FX line wrongly added to the invoice"

BAL=$(pg "SELECT COALESCE(SUM(debit),0)-COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$PMTMOVE")
[ "$BAL" = "0" ] && ok "payment entry balances with the FX line" || no "payment entry out by $BAL"

echo
echo "############ 2. invoice tax picker path ############"
TAX=$(pg "SELECT id FROM account_tax WHERE type_tax_use='sale' AND active ORDER BY id LIMIT 1")
ML=$(pg "SELECT id FROM account_move_line WHERE move_id=$INV AND credit>0 AND tax_line_id IS NULL LIMIT 1")
# Through the RPC the form uses — proves tax_ids_json is a writable field and
# not silently dropped by the field registry.
call account.move.line write "[[$ML],{\"tax_ids_json\":\"[$TAX]\"}]" >/dev/null
STORED=$(pg "SELECT tax_ids_json FROM account_move_line WHERE id=$ML")
echo "    tax_ids_json stored on line $ML: '$STORED'"
[ "$STORED" = "[$TAX]" ] && ok "picker value persisted through write()" || no "stored value is '$STORED'"
call account.move recompute_totals "[[$INV]]" >/dev/null
TAXLINES=$(pg "SELECT count(*) FROM account_move_line WHERE move_id=$INV AND tax_line_id IS NOT NULL")
AMT=$(pg "SELECT amount_tax FROM account_move WHERE id=$INV")
echo "    after setting tax on the line: tax lines=$TAXLINES amount_tax=$AMT"
[ "$TAXLINES" -ge 1 ] && ok "tax line generated from the picker value" || no "no tax line"
[ "$AMT" != "0" ]     && ok "header amount_tax populated" || no "amount_tax still zero"

echo
echo "############ 3. generated tax lines are hidden from the editable grid ############"
# The form loads lines with debit=0 AND tax_line_id IS NULL. Without that
# second clause the generated tax lines would appear as editable product
# lines and saving would double the invoice.
EDITABLE=$(pg "SELECT count(*) FROM account_move_line WHERE move_id=$INV AND debit=0 AND tax_line_id IS NULL")
ALLCREDIT=$(pg "SELECT count(*) FROM account_move_line WHERE move_id=$INV AND debit=0")
echo "    credit lines total=$ALLCREDIT  editable (tax_line_id IS NULL)=$EDITABLE"
[ "$EDITABLE" -lt "$ALLCREDIT" ] && ok "tax lines excluded from the editable set" \
                                 || no "filter is not excluding anything"

echo
echo "############ 4. the mirror case: a vendor BILL in USD ############"
# The legs are reversed, so the same 21.50 gap must land on the CREDIT side
# and read as a GAIN — we settled a 470.00 liability for 448.50.
AP=$(pg "SELECT id FROM account_account WHERE code='2100' LIMIT 1")
[ -z "$AP" ] && AP=$(pg "SELECT id FROM account_account WHERE account_type LIKE '%payable%' ORDER BY id LIMIT 1")
EXP=$(pg "SELECT id FROM account_account WHERE code='6000' LIMIT 1")
[ -z "$EXP" ] && EXP=$(pg "SELECT id FROM account_account WHERE account_type LIKE '%expense%' ORDER BY id LIMIT 1")
echo "    payable=$AP expense=$EXP"
BILL=$(pg "INSERT INTO account_move (name,move_type,state,date,journal_id,partner_id,company_id,
                                     currency_id,currency_rate,amount_untaxed,amount_tax,
                                     amount_total,amount_residual,payment_state)
           VALUES ('FXBILL/'||nextval('account_move_id_seq')::text,'in_invoice','posted',CURRENT_DATE,
                   $JOURNAL,$PARTNER,1,$USD,4700000,$((100*M)),0,$((100*M)),$((100*M)),'not_paid')
           RETURNING id")
pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,debit,credit,display_type)
    VALUES ($BILL,$EXP,$JOURNAL,1,CURRENT_DATE,'Exp',$((100*M)),0,'')" >/dev/null
pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,debit,credit,display_type)
    VALUES ($BILL,$AP,$JOURNAL,1,CURRENT_DATE,'AP',0,$((100*M)),'')" >/dev/null
BR=$(call account.move action_register_payment "[[$BILL]]" \
  "\"payment_date\":\"$(date +%F)\",\"journal_id\":$JOURNAL,\"amount\":100,\"memo\":\"FX bill\",\"amount_received_base\":448.50,")
echo "    response: $(printf '%s' "$BR" | head -c 220)"

BMOVE=$(pg "SELECT move_id FROM account_payment WHERE id=(SELECT payment_id FROM account_partial_reconcile WHERE move_id=$BILL LIMIT 1)")
BBAL=$(pg "SELECT COALESCE(SUM(debit),0)-COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$BMOVE")
BFXDR=$(pg "SELECT COALESCE(SUM(l.debit),0) FROM account_move_line l JOIN account_account a ON a.id=l.account_id
             WHERE l.move_id=$BMOVE AND a.code='7900'")
BFXCR=$(pg "SELECT COALESCE(SUM(l.credit),0) FROM account_move_line l JOIN account_account a ON a.id=l.account_id
             WHERE l.move_id=$BMOVE AND a.code='7900'")
echo "    bill payment entry $BMOVE: balance=$BBAL  FX debit=$BFXDR  FX credit=$BFXCR"
[ "$BBAL" = "0" ]           && ok "vendor payment entry balances" || no "out by $BBAL"
[ "$BFXCR" = "21500000" ]   && ok "FX lands on the CREDIT side (a gain) for an outbound payment" \
                            || no "FX credit is $BFXCR (debit $BFXDR)"
[ "$BFXDR" = "0" ]          && ok "no debit-side FX on the mirror case" || no "unexpected FX debit $BFXDR"

echo
echo "############ cleanup ############"
# Built as a list so an EMPTY id cannot produce `IN (12,)` — a syntax error
# that pg() swallows along with stderr, silently leaving the test rows
# behind. A failing assertion above is exactly when an id is empty, so the
# naive form leaked precisely when a run had already gone wrong.
IDS=$(printf '%s\n' "$INV" "$PMTMOVE" "$BILL" "$BMOVE" | grep -E '^[0-9]+$' | paste -sd, -)
if [ -n "$IDS" ]; then
    pg "DELETE FROM account_partial_reconcile WHERE move_id IN ($IDS)" >/dev/null
    pg "DELETE FROM account_payment          WHERE move_id IN ($IDS)" >/dev/null
    pg "DELETE FROM account_move_line        WHERE move_id IN ($IDS)" >/dev/null
    pg "DELETE FROM account_move             WHERE id      IN ($IDS)" >/dev/null
fi
# Belt and braces: anything this script has ever created is named by prefix,
# so a row orphaned by an interrupted run gets swept on the next one rather
# than accumulating until it trips the ledger-integrity check.
pg "DELETE FROM account_move_line WHERE move_id IN
      (SELECT id FROM account_move WHERE name LIKE 'FXTEST/%' OR name LIKE 'FXBILL/%')" >/dev/null
pg "DELETE FROM account_move WHERE name LIKE 'FXTEST/%' OR name LIKE 'FXBILL/%'" >/dev/null
pg "UPDATE res_currency SET rate=1000000 WHERE id=$USD" >/dev/null
echo "    test data removed (ids: ${IDS:-none})"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

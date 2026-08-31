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
# P3 — tax engine, end to end through the API.
#
# The unit tests (tests/test_tax.cpp) prove the arithmetic. This proves
# the wiring: that a line saved through the API gets the right tax, that
# an invoice generates real tax LINES in the ledger, and that the entry
# still balances afterwards.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cat > /tmp/vt_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/vt_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }
source scripts/seed_test_fixtures.sh; ensure_sale_fixture "$SID" >/dev/null 2>&1

call() {
    cat > /tmp/vt.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$1","method":"$2","args":$3,
 "kwargs":{"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/vt.json
}

TAX_ID=$(pg "SELECT id FROM account_tax WHERE type_tax_use='sale' AND active ORDER BY id LIMIT 1")
RATE=$(pg "SELECT amount FROM account_tax WHERE id=$TAX_ID")
echo "############ using tax id=$TAX_ID rate=$RATE% ############"

echo
echo "############ 1. sale line: exclusive tax ############"
SO=$(pg "SELECT id FROM sale_order ORDER BY id LIMIT 1")
PROD=$(pg "SELECT id FROM product_product ORDER BY id LIMIT 1")
R=$(call sale.order.line create "[{\"order_id\":$SO,\"product_id\":$PROD,\"name\":\"TaxProbe\",\"product_uom_qty\":2,\"price_unit\":100,\"tax_ids_json\":\"[$TAX_ID]\"}]")
LID=$(printf '%s' "$R" | sed -n 's/.*"result":\([0-9]*\).*/\1/p')
if [ -z "$LID" ]; then
    no "could not create line: $(printf '%s' "$R" | head -c 140)"
else
    SUB=$(pg "SELECT price_subtotal FROM sale_order_line WHERE id=$LID")
    TAXV=$(pg "SELECT price_tax FROM sale_order_line WHERE id=$LID")
    TOT=$(pg "SELECT price_total FROM sale_order_line WHERE id=$LID")
    echo "    2 x 100 @ $RATE%  ->  subtotal=$SUB  tax=$TAXV  total=$TOT  (micro-units)"
    [ "$SUB"  = "200000000" ] && ok "subtotal 200.00" || no "subtotal is $SUB, expected 200000000"
    [ "$TAXV" = "30000000"  ] && ok "tax 30.00 (15% of 200)" || no "tax is $TAXV, expected 30000000"
    [ "$TOT"  = "230000000" ] && ok "total 230.00" || no "total is $TOT"
    [ "$((SUB + TAXV))" = "$TOT" ] && ok "subtotal + tax == total exactly" || no "does not reconcile"
fi

echo
echo "############ 2. sale line: INCLUSIVE tax (was silently zero) ############"
pg "UPDATE account_tax SET price_include = TRUE WHERE id=$TAX_ID" >/dev/null
R=$(call sale.order.line create "[{\"order_id\":$SO,\"product_id\":$PROD,\"name\":\"TaxProbeIncl\",\"product_uom_qty\":1,\"price_unit\":115,\"tax_ids_json\":\"[$TAX_ID]\"}]")
LID2=$(printf '%s' "$R" | sed -n 's/.*"result":\([0-9]*\).*/\1/p')
if [ -n "$LID2" ]; then
    SUB=$(pg "SELECT price_subtotal FROM sale_order_line WHERE id=$LID2")
    TAXV=$(pg "SELECT price_tax FROM sale_order_line WHERE id=$LID2")
    TOT=$(pg "SELECT price_total FROM sale_order_line WHERE id=$LID2")
    echo "    115 gross incl $RATE%  ->  subtotal=$SUB  tax=$TAXV  total=$TOT"
    [ "$TAXV" != "0" ] && ok "tax is NOT zero (the old bug produced 0)" || no "tax is still zero"
    [ "$SUB" = "100000000" ] && ok "base backed out to 100.00" || no "base is $SUB, expected 100000000"
    [ "$TOT" = "115000000" ] && ok "total equals the quoted gross 115.00" || no "total is $TOT"
    [ "$((SUB + TAXV))" = "$TOT" ] && ok "base + tax == quoted gross exactly" || no "does not reconcile"
    call sale.order.line unlink "[[$LID2]]" >/dev/null
fi
pg "UPDATE account_tax SET price_include = FALSE WHERE id=$TAX_ID" >/dev/null
[ -n "$LID" ] && call sale.order.line unlink "[[$LID]]" >/dev/null

echo
echo "############ 3. invoice: tax LINES generated in the ledger ############"
MOVE=$(pg "SELECT id FROM account_move WHERE move_type='out_invoice' AND state='draft' ORDER BY id LIMIT 1")
if [ -z "$MOVE" ]; then
    MOVE=$(pg "SELECT id FROM account_move WHERE move_type='out_invoice' ORDER BY id LIMIT 1")
fi
if [ -z "$MOVE" ]; then
    echo "    SKIP  no customer invoice available"
else
    ML=$(pg "SELECT id FROM account_move_line WHERE move_id=$MOVE AND credit>0 AND tax_line_id IS NULL ORDER BY id LIMIT 1")
    if [ -n "$ML" ]; then
        pg "UPDATE account_move_line SET tax_ids_json='[$TAX_ID]' WHERE id=$ML" >/dev/null
        BEFORE=$(pg "SELECT count(*) FROM account_move_line WHERE move_id=$MOVE AND tax_line_id IS NOT NULL")
        call account.move recompute_totals "[[$MOVE]]" >/dev/null
        AFTER=$(pg "SELECT count(*) FROM account_move_line WHERE move_id=$MOVE AND tax_line_id IS NOT NULL")
        AMT=$(pg "SELECT amount_tax FROM account_move WHERE id=$MOVE")
        echo "    tax lines: $BEFORE -> $AFTER ; header amount_tax = $AMT"
        [ "$AFTER" -ge 1 ] && ok "tax line generated in the ledger" || no "no tax line created"
        [ "$AMT" != "0" ]  && ok "header amount_tax populated" || no "amount_tax still zero"

        # idempotency: recompute must not multiply the tax
        call account.move recompute_totals "[[$MOVE]]" >/dev/null
        AGAIN=$(pg "SELECT count(*) FROM account_move_line WHERE move_id=$MOVE AND tax_line_id IS NOT NULL")
        AMT2=$(pg "SELECT amount_tax FROM account_move WHERE id=$MOVE")
        echo "    after a second recompute: lines=$AGAIN amount_tax=$AMT2"
        [ "$AGAIN" = "$AFTER" ] && ok "recompute is idempotent (rebuild, not append)" || no "tax lines multiplied"
        [ "$AMT2" = "$AMT" ]    && ok "amount_tax stable across recomputes" || no "amount_tax drifted"

        # the entry must still balance
        DIFF=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$MOVE")
        echo "    SUM(debit) - SUM(credit) = $DIFF"
        [ "$DIFF" = "0" ] && ok "journal entry balances with the tax line" || no "entry is out of balance by $DIFF"

        pg "UPDATE account_move_line SET tax_ids_json='[]' WHERE id=$ML" >/dev/null
        call account.move recompute_totals "[[$MOVE]]" >/dev/null
    else
        echo "    SKIP  invoice has no product line"
    fi
fi

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

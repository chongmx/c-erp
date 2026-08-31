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
# Real-time inventory GL postings.
#
# Proves through the real HTTP path that each valuation layer posts a
# balanced journal entry in the stock journal, and that the Stock
# Valuation account ties out to the product's inventory value:
#   * receipt   -> Dr Stock Valuation / Cr Stock Interim (Received)
#   * delivery  -> Dr COGS            / Cr Stock Valuation
#   * every entry balances, is posted, and links back to its layer
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/gl_cookie.txt
cat > /tmp/gl_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/gl_auth.json > /tmp/gl_auth_out.json
grep -q '"session_id"' /tmp/gl_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
M=1000000
receive() {
    local pk=$(pg "INSERT INTO stock_picking (name,picking_type_id,state,location_id,location_dest_id,company_id,origin) VALUES ('GL-IN',1,'draft',5,4,1,'GLTEST') RETURNING id")
    pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id) VALUES ($pk,$1,'r',$(($2*M)),0,'draft',5,4,1)" >/dev/null
    callkw stock.picking action_confirm "[[$pk]]" >/dev/null
    callkw stock.picking button_validate "[[$pk]]" >/dev/null
}
deliver() {
    local pk=$(pg "INSERT INTO stock_picking (name,picking_type_id,state,location_id,location_dest_id,company_id,origin) VALUES ('GL-OUT',2,'draft',4,6,1,'GLTEST') RETURNING id")
    pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id) VALUES ($pk,$1,'d',$(($2*M)),0,'draft',4,6,1)" >/dev/null
    callkw stock.picking action_confirm "[[$pk]]" >/dev/null
    callkw stock.picking button_validate "[[$pk]]" >/dev/null
}

echo "############ 0. GL config seeded ############"
STJ=$(pg "SELECT id FROM account_journal WHERE code='STJ' AND company_id=1")
A1400=$(pg "SELECT id FROM account_account WHERE code='1400' AND company_id=1")
A1410=$(pg "SELECT id FROM account_account WHERE code='1410' AND company_id=1")
echo "    STJ journal=$STJ  1400=$A1400  1410=$A1410"
[ -n "$STJ" ]   && ok "Inventory journal (STJ) seeded"      || no "no STJ journal"
[ -n "$A1400" ] && ok "Stock Valuation account (1400) seeded" || no "no 1400 account"

echo
echo "############ 1. receipt posts Dr Valuation / Cr Interim ############"
PS=$(pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available,cost_method,standard_price) VALUES ('GLTEST','product',1,1,true,0,'standard',$((5*M))) RETURNING id")
receive "$PS" 10          # value 50 -> Dr 1400 50 / Cr 1410 50
DR1400=$(pg "SELECT COALESCE(SUM(debit),0) FROM account_move_line WHERE account_id=$A1400 AND move_id IN (SELECT account_move_id FROM stock_valuation_layer WHERE product_id=$PS)")
CR1410=$(pg "SELECT COALESCE(SUM(credit),0) FROM account_move_line WHERE account_id=$A1410 AND move_id IN (SELECT account_move_id FROM stock_valuation_layer WHERE product_id=$PS)")
echo "    Dr 1400=$DR1400  Cr 1410=$CR1410"
[ "$DR1400" = "50000000" ] && ok "Stock Valuation debited 50" || no "Dr 1400=$DR1400"
[ "$CR1410" = "50000000" ] && ok "Stock Interim credited 50"  || no "Cr 1410=$CR1410"

echo
echo "############ 2. delivery posts Dr COGS / Cr Valuation ############"
deliver "$PS" 4           # value 20 -> Dr 5000 20 / Cr 1400 20
A5000=$(pg "SELECT id FROM account_account WHERE code='5000' AND company_id=1")
DRCOGS=$(pg "SELECT COALESCE(SUM(debit),0) FROM account_move_line WHERE account_id=$A5000 AND move_id IN (SELECT account_move_id FROM stock_valuation_layer WHERE product_id=$PS)")
CR1400=$(pg "SELECT COALESCE(SUM(credit),0) FROM account_move_line WHERE account_id=$A1400 AND move_id IN (SELECT account_move_id FROM stock_valuation_layer WHERE product_id=$PS)")
echo "    Dr COGS(5000)=$DRCOGS  Cr 1400=$CR1400"
[ "$DRCOGS" = "20000000" ] && ok "COGS debited 20 on delivery" || no "Dr COGS=$DRCOGS"
[ "$CR1400" = "20000000" ] && ok "Stock Valuation credited 20" || no "Cr 1400=$CR1400"

echo
echo "############ 3. valuation ties to the ledger ############"
BAL1400=$(pg "SELECT COALESCE(SUM(debit)-SUM(credit),0) FROM account_move_line WHERE account_id=$A1400 AND move_id IN (SELECT account_move_id FROM stock_valuation_layer WHERE product_id=$PS)")
VSVL=$(pg "SELECT value_svl FROM product_product WHERE id=$PS")
echo "    Stock Valuation balance=$BAL1400  product value_svl=$VSVL"
[ "$BAL1400" = "30000000" ] && ok "Stock Valuation balance = 30" || no "1400 balance=$BAL1400"
[ "$BAL1400" = "$VSVL" ] && ok "GL Stock Valuation == product inventory value" || no "GL $BAL1400 != value_svl $VSVL"

echo
echo "############ 4. entries balanced, posted, linked ############"
UNBAL=$(pg "SELECT count(*) FROM (SELECT move_id FROM account_move_line WHERE move_id IN (SELECT account_move_id FROM stock_valuation_layer WHERE product_id=$PS) GROUP BY move_id HAVING SUM(debit)<>SUM(credit)) x")
DRAFT=$(pg "SELECT count(*) FROM account_move WHERE id IN (SELECT account_move_id FROM stock_valuation_layer WHERE product_id=$PS) AND state<>'posted'")
UNLINKED=$(pg "SELECT count(*) FROM stock_valuation_layer WHERE product_id=$PS AND account_move_id IS NULL")
echo "    unbalanced=$UNBAL  non-posted=$DRAFT  unlinked layers=$UNLINKED"
[ "$UNBAL" = "0" ]    && ok "every stock journal entry balances"      || no "$UNBAL unbalanced entries"
[ "$DRAFT" = "0" ]    && ok "entries are posted"                      || no "$DRAFT non-posted"
[ "$UNLINKED" = "0" ] && ok "each valuation layer links to its entry" || no "$UNLINKED unlinked layers"

echo
echo "############ cleanup ############"
MVS=$(pg "SELECT string_agg(account_move_id::text,',') FROM stock_valuation_layer WHERE product_id=$PS AND account_move_id IS NOT NULL")
[ -n "$MVS" ] && pg "DELETE FROM account_move_line WHERE move_id IN ($MVS)" >/dev/null
pg "DELETE FROM stock_valuation_layer WHERE product_id=$PS" >/dev/null
[ -n "$MVS" ] && pg "DELETE FROM account_move WHERE id IN ($MVS)" >/dev/null
pg "DELETE FROM stock_move WHERE origin='GLTEST' OR product_id=$PS" >/dev/null
pg "DELETE FROM stock_picking WHERE origin='GLTEST'" >/dev/null
pg "DELETE FROM stock_quant WHERE product_id=$PS" >/dev/null
pg "DELETE FROM product_product WHERE id=$PS" >/dev/null
rm -f "$CK" /tmp/gl_auth.json /tmp/gl_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

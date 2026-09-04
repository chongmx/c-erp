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
# Inventory valuation — the three cost methods (standard / average / FIFO).
#
# Proves through the real HTTP path that every move crossing the owned-stock
# boundary writes a valuation layer and maintains product value_svl:
#   * standard  — everything valued at the fixed standard cost
#   * average   — receipts blend into a running weighted average (AVCO),
#                 which becomes the issue cost
#   * FIFO      — issues consume the oldest cost layers first
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/val_cookie.txt
cat > /tmp/val_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/val_auth.json > /tmp/val_auth_out.json
grep -q '"session_id"' /tmp/val_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
M=1000000

mkprod() { pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available,cost_method,standard_price) VALUES ('$1','product',1,1,true,0,'$2',$(($3*M))) RETURNING id"; }
receive() { # $1 product $2 qty  — Vendors(5) -> Stock(4)
    local pk=$(pg "INSERT INTO stock_picking (name,picking_type_id,state,location_id,location_dest_id,company_id,origin) VALUES ('VAL-IN',1,'draft',5,4,1,'VALTEST') RETURNING id")
    pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id) VALUES ($pk,$1,'r',$(($2*M)),0,'draft',5,4,1)" >/dev/null
    callkw stock.picking action_confirm "[[$pk]]" >/dev/null
    callkw stock.picking button_validate "[[$pk]]" >/dev/null
}
deliver() { # $1 product $2 qty  — Stock(4) -> Customers(6)
    local pk=$(pg "INSERT INTO stock_picking (name,picking_type_id,state,location_id,location_dest_id,company_id,origin) VALUES ('VAL-OUT',2,'draft',4,6,1,'VALTEST') RETURNING id")
    pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id) VALUES ($pk,$1,'d',$(($2*M)),0,'draft',4,6,1)" >/dev/null
    callkw stock.picking action_confirm "[[$pk]]" >/dev/null
    callkw stock.picking button_validate "[[$pk]]" >/dev/null
}
setcost() { callkw product.product write "[[$1],{\"standard_price\":$2}]" >/dev/null; }

echo "############ 1. standard costing ############"
PS=$(mkprod 'VALTEST-STD' 'standard' 5)
receive "$PS" 10           # 10 @ 5 = 50
V1=$(pg "SELECT value_svl FROM product_product WHERE id=$PS")
Q1=$(pg "SELECT quantity_svl FROM product_product WHERE id=$PS")
echo "    after receive 10@5:  value_svl=$V1 quantity_svl=$Q1"
[ "$V1" = "50000000" ] && ok "inventory value = 50 (10 x 5)" || no "value_svl=$V1"
[ "$Q1" = "10000000" ] && ok "valued quantity = 10"          || no "quantity_svl=$Q1"
deliver "$PS" 4            # -4 @ 5 = -20
V2=$(pg "SELECT value_svl FROM product_product WHERE id=$PS")
Q2=$(pg "SELECT quantity_svl FROM product_product WHERE id=$PS")
echo "    after deliver 4@5:   value_svl=$V2 quantity_svl=$Q2"
[ "$V2" = "30000000" ] && ok "value = 30 (50 - 4 x 5)" || no "value_svl=$V2"
[ "$Q2" = "6000000" ]  && ok "quantity = 6"            || no "quantity_svl=$Q2"
LC=$(pg "SELECT count(*) FROM stock_valuation_layer WHERE product_id=$PS")
[ "$LC" = "2" ] && ok "two valuation layers written" || no "layer count=$LC"

echo
echo "############ 2. average costing (AVCO) ############"
PA=$(mkprod 'VALTEST-AVG' 'average' 5)
receive "$PA" 10           # 10 @ 5  -> avg 5, value 50
setcost "$PA" 7            # next receipt priced at 7
receive "$PA" 10           # 10 @ 7  -> total qty 20, value 120, avg 6
VA=$(pg "SELECT value_svl FROM product_product WHERE id=$PA")
QA=$(pg "SELECT quantity_svl FROM product_product WHERE id=$PA")
CA=$(pg "SELECT standard_price FROM product_product WHERE id=$PA")
echo "    after 10@5 then 10@7: value_svl=$VA quantity_svl=$QA avg_cost=$CA"
[ "$VA" = "120000000" ] && ok "value = 120 (50 + 70)"     || no "value_svl=$VA"
[ "$CA" = "6000000" ]   && ok "average cost blended to 6" || no "avg cost=$CA"
deliver "$PA" 5           # issue 5 @ avg 6 = 30
VA2=$(pg "SELECT value_svl FROM product_product WHERE id=$PA")
QA2=$(pg "SELECT quantity_svl FROM product_product WHERE id=$PA")
echo "    after issue 5@6:      value_svl=$VA2 quantity_svl=$QA2"
[ "$VA2" = "90000000" ] && ok "value = 90 (120 - 5 x 6)" || no "value_svl=$VA2"
[ "$QA2" = "15000000" ] && ok "quantity = 15"            || no "quantity_svl=$QA2"

echo
echo "############ 3. FIFO costing ############"
PF=$(mkprod 'VALTEST-FIFO' 'fifo' 5)
receive "$PF" 10           # layer A: 10 @ 5 (value 50)
setcost "$PF" 7
receive "$PF" 10           # layer B: 10 @ 7 (value 70)
deliver "$PF" 15           # consume 10@5 + 5@7 = 50 + 35 = 85
VF=$(pg "SELECT value_svl FROM product_product WHERE id=$PF")
QF=$(pg "SELECT quantity_svl FROM product_product WHERE id=$PF")
REM=$(pg "SELECT COALESCE(SUM(remaining_value),0) FROM stock_valuation_layer WHERE product_id=$PF")
echo "    after issue 15 (FIFO): value_svl=$VF quantity_svl=$QF remaining_layers=$REM"
[ "$VF" = "35000000" ] && ok "value = 35 (oldest-first: 120 - 85)" || no "value_svl=$VF"
[ "$QF" = "5000000" ]  && ok "quantity = 5"                        || no "quantity_svl=$QF"
[ "$REM" = "35000000" ] && ok "remaining FIFO layer value = 35 (5 @ 7)" || no "remaining=$REM"

echo
echo "############ 4. valuation ledger is queryable ############"
RD=$(callkw stock.valuation.layer search_read "[[[\"product_id\",\"=\",$PS]]]")
printf '%s' "$RD" | grep -q '"value"' && ok "valuation layer report returns rows" || no "report: $(printf '%s' "$RD" | head -c 100)"

echo
echo "############ cleanup ############"
pg "DELETE FROM stock_move WHERE origin='VALTEST' OR product_id IN ($PS,$PA,$PF)" >/dev/null
pg "DELETE FROM stock_picking WHERE origin='VALTEST'" >/dev/null
pg "DELETE FROM stock_valuation_layer WHERE product_id IN ($PS,$PA,$PF)" >/dev/null
pg "DELETE FROM stock_quant WHERE product_id IN ($PS,$PA,$PF)" >/dev/null
pg "DELETE FROM product_product WHERE id IN ($PS,$PA,$PF)" >/dev/null
rm -f "$CK" /tmp/val_auth.json /tmp/val_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

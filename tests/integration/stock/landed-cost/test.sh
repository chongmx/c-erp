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
# Landed costs — freight / duty / handling capitalised into stock value.
#
# Proves through the real HTTP path:
#   * a landed cost linked to a receipt distributes each cost line across the
#     received products by its split method (quantity / value / equal / weight)
#   * each product's inventory value (value_svl) rises by its allocated share
#   * the GL is posted: Dr Stock Valuation / Cr Landed Costs, tying to the layers
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/lc_cookie.txt
cat > /tmp/lc_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/lc_auth.json > /tmp/lc_auth_out.json
grep -q '"session_id"' /tmp/lc_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
rval() { python3 -c "import json,sys; print(json.load(sys.stdin).get('result',''))" 2>/dev/null; }
M=1000000

# A: cost 10, weight 2.  B: cost 20, weight 1.
A=$(pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available,cost_method,standard_price,weight,volume) VALUES ('LCTEST-A','product',1,1,true,0,'standard',$((10*M)),2,1) RETURNING id")
B=$(pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available,cost_method,standard_price,weight,volume) VALUES ('LCTEST-B','product',1,1,true,0,'standard',$((20*M)),1,2) RETURNING id")
echo "    A=$A (cost 10, wt 2)  B=$B (cost 20, wt 1)"

# One receipt: A x10, B x5.
PK=$(pg "INSERT INTO stock_picking (name,picking_type_id,state,location_id,location_dest_id,company_id,origin) VALUES ('LC-IN',1,'draft',5,4,1,'LCTEST') RETURNING id")
pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id) VALUES ($PK,$A,'a',$((10*M)),0,'draft',5,4,1)" >/dev/null
pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id) VALUES ($PK,$B,'b',$((5*M)),0,'draft',5,4,1)" >/dev/null
callkw stock.picking action_confirm "[[$PK]]" >/dev/null
callkw stock.picking button_validate "[[$PK]]" >/dev/null

echo "############ 1. base inventory value after receipt ############"
VA0=$(pg "SELECT value_svl FROM product_product WHERE id=$A")
VB0=$(pg "SELECT value_svl FROM product_product WHERE id=$B")
echo "    A value=$VA0  B value=$VB0"
[ "$VA0" = "100000000" ] && ok "A = 100 (10 x 10)" || no "A value=$VA0"
[ "$VB0" = "100000000" ] && ok "B = 100 (5 x 20)"  || no "B value=$VB0"

echo
echo "############ 2. landed cost: four split methods ############"
LC=$(callkw stock.landed.cost create "[{\"picking_id\":$PK,\"date\":\"2026-08-08\"}]" | rval)
callkw stock.landed.cost.line create "[{\"landed_cost_id\":$LC,\"name\":\"Freight\",\"price\":60,\"split_method\":\"by_quantity\"}]" >/dev/null
callkw stock.landed.cost.line create "[{\"landed_cost_id\":$LC,\"name\":\"Duty\",\"price\":30,\"split_method\":\"by_price\"}]"     >/dev/null
callkw stock.landed.cost.line create "[{\"landed_cost_id\":$LC,\"name\":\"Handling\",\"price\":20,\"split_method\":\"equal\"}]"     >/dev/null
callkw stock.landed.cost.line create "[{\"landed_cost_id\":$LC,\"name\":\"Insurance\",\"price\":30,\"split_method\":\"by_weight\"}]" >/dev/null
echo "    LC=$LC  lines: Freight 60/qty, Duty 30/value, Handling 20/equal, Insurance 30/weight"
callkw stock.landed.cost button_validate "[[$LC]]" >/dev/null

# Expected allocation:
#   Freight 60 by qty  (10:5)        -> A+40  B+20
#   Duty    30 by value (100:100)    -> A+15  B+15
#   Handling20 equal    (1:1)        -> A+10  B+10
#   Insurance30 by weight(20:5)      -> A+24  B+6
#   A += 89 -> 189 ;  B += 51 -> 151
VA=$(pg "SELECT value_svl FROM product_product WHERE id=$A")
VB=$(pg "SELECT value_svl FROM product_product WHERE id=$B")
ST=$(pg "SELECT state FROM stock_landed_cost WHERE id=$LC")
echo "    A value=$VA  B value=$VB  LC state=$ST"
[ "$VA" = "189000000" ] && ok "A capitalised to 189 (100 + 40+15+10+24)" || no "A value=$VA"
[ "$VB" = "151000000" ] && ok "B capitalised to 151 (100 + 20+15+10+6)"  || no "B value=$VB"
[ "$ST" = "done" ] && ok "landed cost validated" || no "state=$ST"

echo
echo "############ 3. GL: Dr Stock Valuation / Cr Landed Costs ############"
A1400=$(pg "SELECT id FROM account_account WHERE code='1400' AND company_id=1")
A5200=$(pg "SELECT id FROM account_account WHERE code='5200' AND company_id=1")
MVS="SELECT account_move_id FROM stock_valuation_layer WHERE product_id IN ($A,$B) AND counterpart_usage='revaluation' AND account_move_id IS NOT NULL"
DR=$(pg "SELECT COALESCE(SUM(debit),0) FROM account_move_line WHERE account_id=$A1400 AND move_id IN ($MVS)")
CR=$(pg "SELECT COALESCE(SUM(credit),0) FROM account_move_line WHERE account_id=$A5200 AND move_id IN ($MVS)")
echo "    Dr 1400=$DR  Cr 5200=$CR"
[ "$DR" = "140000000" ] && ok "Stock Valuation debited 140 (total landed)" || no "Dr 1400=$DR"
[ "$CR" = "140000000" ] && ok "Landed Costs credited 140"                  || no "Cr 5200=$CR"

echo
echo "############ cleanup ############"
MV=$(pg "SELECT string_agg(account_move_id::text,',') FROM stock_valuation_layer WHERE product_id IN ($A,$B) AND account_move_id IS NOT NULL")
[ -n "$MV" ] && pg "DELETE FROM account_move_line WHERE move_id IN ($MV)" >/dev/null
pg "DELETE FROM stock_landed_cost_line WHERE landed_cost_id=$LC" >/dev/null
pg "DELETE FROM stock_landed_cost WHERE id=$LC" >/dev/null
pg "DELETE FROM stock_valuation_layer WHERE product_id IN ($A,$B)" >/dev/null
[ -n "$MV" ] && pg "DELETE FROM account_move WHERE id IN ($MV)" >/dev/null
pg "DELETE FROM stock_move WHERE picking_id=$PK" >/dev/null
pg "DELETE FROM stock_picking WHERE origin='LCTEST'" >/dev/null
pg "DELETE FROM stock_quant WHERE product_id IN ($A,$B)" >/dev/null
pg "DELETE FROM product_product WHERE id IN ($A,$B)" >/dev/null
rm -f "$CK" /tmp/lc_auth.json /tmp/lc_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

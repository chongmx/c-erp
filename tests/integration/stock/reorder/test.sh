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
# Reordering rules (stock.warehouse.orderpoint).
#
# Proves through the real HTTP path:
#   * when forecasted stock falls below the minimum, the scheduler drafts a
#     replenishment up to the maximum, rounded to the order multiple
#   * route 'buy' drafts a purchase order to the vendor; 'manufacture' an MO
#   * an already-open replenishment counts as incoming, so the rule does not
#     re-order on the next run
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/ro_cookie.txt
cat > /tmp/ro_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/ro_auth.json > /tmp/ro_auth_out.json
grep -q '"session_id"' /tmp/ro_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
rval() { python3 -c "import json,sys; print(json.load(sys.stdin).get('result',''))" 2>/dev/null; }
M=1000000
mkprod() { pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available,cost_method,standard_price) VALUES ('$1','product',1,1,true,0,'standard',$(($2*M))) RETURNING id"; }

VENDOR=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
echo "    vendor=$VENDOR"

echo "############ 1. buy route drafts a purchase order ############"
PB=$(mkprod 'ROTEST-Buy' 8)
OP1=$(callkw stock.warehouse.orderpoint create "[{\"product_id\":$PB,\"location_id\":4,\"product_min_qty\":20,\"product_max_qty\":100,\"qty_multiple\":10,\"route\":\"buy\",\"supplier_id\":$VENDOR}]" | rval)
callkw stock.quant set_on_hand "[{\"product_id\":$PB,\"location_id\":4,\"quantity\":15}]" >/dev/null
echo "    PB=$PB  OP1=$OP1  (min 20, max 100, mult 10, on-hand 15)"
R1=$(callkw stock.warehouse.orderpoint run_scheduler "[]")
echo "    scheduler #1 -> $(printf '%s' "$R1" | python3 -c 'import json,sys;d=json.load(sys.stdin)["result"];print("checked",d["orderpoints_checked"],"created",d["replenishments_created"])' 2>/dev/null)"
POQTY=$(pg "SELECT pol.product_qty FROM purchase_order_line pol JOIN purchase_order po ON po.id=pol.order_id WHERE pol.product_id=$PB AND po.origin LIKE 'Reordering%'")
POCNT=$(pg "SELECT count(DISTINCT po.id) FROM purchase_order po JOIN purchase_order_line pol ON pol.order_id=po.id WHERE pol.product_id=$PB AND po.origin LIKE 'Reordering%'")
POST=$(pg "SELECT po.state FROM purchase_order po JOIN purchase_order_line pol ON pol.order_id=po.id WHERE pol.product_id=$PB AND po.origin LIKE 'Reordering%' LIMIT 1")
echo "    PO line qty=$POQTY  PO count=$POCNT  state=$POST"
# to_order = 100 - 15 = 85, rounded up to multiple 10 -> 90
[ "$POQTY" = "90000000" ] && ok "drafted PO for 90 (85 rounded up to x10)" || no "PO qty=$POQTY"
[ "$POCNT" = "1" ] && ok "exactly one purchase order drafted" || no "PO count=$POCNT"
[ "$POST" = "draft" ] && ok "PO is draft (buyer reviews before sending)" || no "PO state=$POST"

echo
echo "############ 2. manufacture route drafts an MO; buy rule doesn't re-order ############"
PM=$(mkprod 'ROTEST-Make' 5)
OP2=$(callkw stock.warehouse.orderpoint create "[{\"product_id\":$PM,\"location_id\":4,\"product_min_qty\":20,\"product_max_qty\":50,\"qty_multiple\":1,\"route\":\"manufacture\"}]" | rval)
callkw stock.quant set_on_hand "[{\"product_id\":$PM,\"location_id\":4,\"quantity\":5}]" >/dev/null
echo "    PM=$PM  OP2=$OP2  (min 20, max 50, make, on-hand 5)"
callkw stock.warehouse.orderpoint run_scheduler "[]" >/dev/null
MOQTY=$(pg "SELECT product_qty FROM mrp_production WHERE product_id=$PM AND origin LIKE 'Reordering%'")
MOST=$(pg "SELECT state FROM mrp_production WHERE product_id=$PM AND origin LIKE 'Reordering%' LIMIT 1")
POCNT2=$(pg "SELECT count(DISTINCT po.id) FROM purchase_order po JOIN purchase_order_line pol ON pol.order_id=po.id WHERE pol.product_id=$PB AND po.origin LIKE 'Reordering%'")
echo "    MO qty=$MOQTY  MO state=$MOST  PB purchase orders still=$POCNT2"
# to_order = 50 - 5 = 45
[ "$MOQTY" = "45000000" ] && ok "drafted MO for 45" || no "MO qty=$MOQTY"
[ "$MOST" = "draft" ] && ok "MO is draft" || no "MO state=$MOST"
[ "$POCNT2" = "1" ] && ok "buy rule did NOT re-order (open PO counts as incoming)" || no "PB PO count=$POCNT2"

echo
echo "############ cleanup ############"
pg "DELETE FROM purchase_order_line WHERE product_id IN ($PB,$PM)" >/dev/null
pg "DELETE FROM purchase_order WHERE origin LIKE 'Reordering%'" >/dev/null
pg "DELETE FROM mrp_production WHERE product_id IN ($PB,$PM)" >/dev/null
pg "DELETE FROM stock_warehouse_orderpoint WHERE id IN ($OP1,$OP2)" >/dev/null
pg "DELETE FROM stock_quant WHERE product_id IN ($PB,$PM)" >/dev/null
pg "DELETE FROM stock_valuation_layer WHERE product_id IN ($PB,$PM)" >/dev/null
pg "DELETE FROM product_product WHERE id IN ($PB,$PM)" >/dev/null
rm -f "$CK" /tmp/ro_auth.json /tmp/ro_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

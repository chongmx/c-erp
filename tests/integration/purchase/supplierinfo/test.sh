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
# product.supplierinfo — vendor pricelists, and reorder integration.
#
# Proves through the real HTTP path:
#   * a vendor pricelist line stores who sells a product, at what price/MOQ/lead
#   * the reordering "buy" route, with no vendor named on the rule, picks the
#     vendor AND price from product.supplierinfo (not standard_price)
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/si_cookie.txt
cat > /tmp/si_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/si_auth.json > /tmp/si_auth_out.json
grep -q '"session_id"' /tmp/si_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
rval() { python3 -c "import json,sys; print(json.load(sys.stdin).get('result',''))" 2>/dev/null; }
M=1000000

VENDOR=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
P=$(pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available,cost_method,standard_price) VALUES ('SITEST','product',1,1,true,0,'standard',$((8*M))) RETURNING id")
echo "    vendor=$VENDOR  product=$P (standard cost 8)"

echo "############ 1. vendor pricelist line ############"
SI=$(callkw product.supplierinfo create "[{\"product_id\":$P,\"partner_id\":$VENDOR,\"price\":12,\"min_qty\":5,\"delay\":3,\"product_code\":\"V-123\"}]" | rval)
echo "    supplierinfo id=$SI"
[ -n "$SI" ] && ok "vendor line created" || { no "create failed"; exit 1; }
RD=$(callkw product.supplierinfo search_read "[[[\"product_id\",\"=\",$P]]]")
echo "    search_read -> $(printf '%s' "$RD" | head -c 160)"
printf '%s' "$RD" | grep -q '"price":12' && ok "line reads back with price 12" || no "price not 12: $RD"
printf '%s' "$RD" | grep -q '"product_code":"V-123"' && ok "vendor code stored" || no "vendor code missing"

echo
echo "############ 2. reorder picks vendor + price from supplierinfo ############"
OP=$(callkw stock.warehouse.orderpoint create "[{\"product_id\":$P,\"location_id\":4,\"product_min_qty\":20,\"product_max_qty\":100,\"qty_multiple\":1,\"route\":\"buy\"}]" | rval)
callkw stock.quant set_on_hand "[{\"product_id\":$P,\"location_id\":4,\"quantity\":15}]" >/dev/null
echo "    orderpoint=$OP (min 20/max 100, NO vendor named), on-hand 15"
callkw stock.warehouse.orderpoint run_scheduler "[]" >/dev/null
POPART=$(pg "SELECT po.partner_id FROM purchase_order po JOIN purchase_order_line pol ON pol.order_id=po.id WHERE pol.product_id=$P AND po.origin LIKE 'Reordering%' LIMIT 1")
POPRICE=$(pg "SELECT pol.price_unit FROM purchase_order_line pol JOIN purchase_order po ON po.id=pol.order_id WHERE pol.product_id=$P AND po.origin LIKE 'Reordering%' LIMIT 1")
POQTY=$(pg "SELECT pol.product_qty FROM purchase_order_line pol JOIN purchase_order po ON po.id=pol.order_id WHERE pol.product_id=$P AND po.origin LIKE 'Reordering%' LIMIT 1")
echo "    drafted PO: partner=$POPART  price_unit=$POPRICE  qty=$POQTY"
[ "$POPART" = "$VENDOR" ] && ok "vendor auto-picked from supplierinfo" || no "PO partner=$POPART vs vendor $VENDOR"
[ "$POPRICE" = "12000000" ] && ok "price from supplierinfo (12), not standard cost (8)" || no "price_unit=$POPRICE"
[ "$POQTY" = "85000000" ] && ok "quantity to order = 85 (100 - 15)" || no "qty=$POQTY"

echo
echo "############ cleanup ############"
pg "DELETE FROM purchase_order_line WHERE product_id=$P" >/dev/null
pg "DELETE FROM purchase_order WHERE origin LIKE 'Reordering%' AND id NOT IN (SELECT order_id FROM purchase_order_line)" >/dev/null
pg "DELETE FROM stock_warehouse_orderpoint WHERE id=$OP" >/dev/null
pg "DELETE FROM product_supplierinfo WHERE product_id=$P" >/dev/null
pg "DELETE FROM stock_quant WHERE product_id=$P" >/dev/null
pg "DELETE FROM stock_valuation_layer WHERE product_id=$P" >/dev/null
pg "DELETE FROM product_product WHERE id=$P" >/dev/null
rm -f "$CK" /tmp/si_auth.json /tmp/si_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

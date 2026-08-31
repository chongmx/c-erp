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
# stock.quant — on-hand ledger, reservation, inventory adjustment.
#
# Proves the quant engine through the real HTTP path:
#   * a validated receipt raises on-hand and product.qty_available
#   * a delivery reserves available stock (assign) and releases it
#     (unreserve), then consumes it on validate
#   * inventory adjustment (set_on_hand) books the difference through
#     the Inventory Adjustments location
#   * allow-negative: a delivery beyond on-hand still validates and
#     drives the location negative (the reference ERP default)
#   * the On Hand report lists internal-location quants only
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/sq_cookie.txt
cat > /tmp/sq_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/sq_auth.json > /tmp/sq_auth_out.json
grep -q '"session_id"' /tmp/sq_auth_out.json || { echo "cannot authenticate"; exit 1; }

# call_kw helper — $1 model  $2 method  $3 args-json (default [])
callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}

M=1000000   # micro-units per whole unit

# Fresh product so on-hand starts from a known 0.
PRODUCT=$(pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available) VALUES ('SQTEST Widget','product',1,1,true,0) RETURNING id")
echo "    test product id=$PRODUCT"
[ -n "$PRODUCT" ] && ok "created test product" || { no "could not create product"; exit 1; }

mkpick() { # $1 name $2 type $3 src $4 dest -> picking id
    pg "INSERT INTO stock_picking (name,picking_type_id,state,location_id,location_dest_id,company_id,origin) VALUES ('$1',$2,'draft',$3,$4,1,'SQTEST') RETURNING id"
}
mkmove() { # $1 picking $2 src $3 dest $4 qty(units)
    pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id) VALUES ($1,$PRODUCT,'SQTEST move',$(($4*M)),0,'draft',$2,$3,1)"
}

echo "############ 1. receipt raises on-hand + qty_available ############"
P1=$(mkpick 'SQTEST-IN' 1 5 4); mkmove "$P1" 5 4 10 >/dev/null   # Vendors -> Stock, 10
callkw stock.picking action_confirm  "[[$P1]]" >/dev/null
callkw stock.picking button_validate "[[$P1]]" >/dev/null
Q4=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$PRODUCT AND location_id=4")
QA=$(pg "SELECT qty_available FROM product_product WHERE id=$PRODUCT")
Q5=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$PRODUCT AND location_id=5")
echo "    Stock quant=$Q4  qty_available=$QA  Vendors quant=$Q5"
[ "$Q4" = "10000000" ] && ok "on-hand at WH/Stock = 10"            || no "Stock quant=$Q4 (want 10000000)"
[ "$QA" = "10000000" ] && ok "product.qty_available = 10"          || no "qty_available=$QA"
[ "$Q5" = "-10000000" ] && ok "Vendors location went -10 (other end)" || no "Vendors quant=$Q5"

echo
echo "############ 2. delivery reserves, then unreserves ############"
P2=$(mkpick 'SQTEST-OUT' 2 4 6); mkmove "$P2" 4 6 4 >/dev/null    # Stock -> Customers, 4
callkw stock.picking action_confirm "[[$P2]]" >/dev/null
callkw stock.picking action_assign  "[[$P2]]" >/dev/null
RSV=$(pg "SELECT reserved_quantity FROM stock_quant WHERE product_id=$PRODUCT AND location_id=4")
MRSV=$(pg "SELECT reserved_qty FROM stock_move WHERE picking_id=$P2")
echo "    after assign: quant.reserved=$RSV  move.reserved_qty=$MRSV"
[ "$RSV" = "4000000" ] && ok "assign reserved 4 at WH/Stock"       || no "reserved=$RSV"
[ "$MRSV" = "4000000" ] && ok "move records its reservation"       || no "move reserved_qty=$MRSV"
callkw stock.picking button_unreserve "[[$P2]]" >/dev/null
RSV0=$(pg "SELECT reserved_quantity FROM stock_quant WHERE product_id=$PRODUCT AND location_id=4")
echo "    after unreserve: quant.reserved=$RSV0"
[ "$RSV0" = "0" ] && ok "unreserve released the reservation"        || no "reserved still $RSV0"

echo
echo "############ 3. delivery validate consumes on-hand ############"
callkw stock.picking action_assign  "[[$P2]]" >/dev/null
callkw stock.picking button_validate "[[$P2]]" >/dev/null
Q4b=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$PRODUCT AND location_id=4")
QAb=$(pg "SELECT qty_available FROM product_product WHERE id=$PRODUCT")
RSVb=$(pg "SELECT reserved_quantity FROM stock_quant WHERE product_id=$PRODUCT AND location_id=4")
echo "    Stock quant=$Q4b  qty_available=$QAb  reserved=$RSVb"
[ "$Q4b" = "6000000" ] && ok "on-hand now 6 (10 - 4)"              || no "Stock quant=$Q4b (want 6000000)"
[ "$QAb" = "6000000" ] && ok "qty_available follows to 6"          || no "qty_available=$QAb"
[ "$RSVb" = "0" ] && ok "reservation cleared on validate"          || no "reserved=$RSVb"

echo
echo "############ 4. inventory adjustment (set_on_hand) ############"
callkw stock.quant set_on_hand "[{\"product_id\":$PRODUCT,\"location_id\":4,\"quantity\":25}]" >/dev/null
Q4c=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$PRODUCT AND location_id=4")
QAc=$(pg "SELECT qty_available FROM product_product WHERE id=$PRODUCT")
Q7=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$PRODUCT AND location_id=7")
echo "    Stock quant=$Q4c  qty_available=$QAc  InventoryAdj quant=$Q7"
[ "$Q4c" = "25000000" ] && ok "counted qty applied: on-hand = 25"  || no "Stock quant=$Q4c (want 25000000)"
[ "$QAc" = "25000000" ] && ok "qty_available = 25"                 || no "qty_available=$QAc"
[ "$Q7" = "-19000000" ] && ok "difference booked via Inventory Adjustments (-19)" || no "InvAdj quant=$Q7 (want -19000000)"

echo
echo "############ 5. allow-negative: deliver beyond on-hand ############"
P3=$(mkpick 'SQTEST-NEG' 2 4 6); mkmove "$P3" 4 6 40 >/dev/null   # Stock -> Customers, 40 (> 25)
callkw stock.picking action_confirm  "[[$P3]]" >/dev/null
callkw stock.picking action_assign   "[[$P3]]" >/dev/null
callkw stock.picking button_validate "[[$P3]]" >/dev/null
STV=$(pg "SELECT state FROM stock_picking WHERE id=$P3")
Q4d=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$PRODUCT AND location_id=4")
echo "    picking state=$STV  Stock quant=$Q4d"
[ "$STV" = "done" ] && ok "over-delivery still validated (allow-negative)" || no "state=$STV"
[ "$Q4d" = "-15000000" ] && ok "on-hand went negative: 25 - 40 = -15" || no "Stock quant=$Q4d (want -15000000)"

echo
echo "############ 6. On Hand report lists internal quants only ############"
RD=$(callkw stock.quant search_read "[[[\"product_id\",\"=\",$PRODUCT]]]")
echo "    report -> $(printf '%s' "$RD" | head -c 200)"
printf '%s' "$RD" | grep -q '"location_id":\[4' && ok "WH/Stock row present in report" || no "no Stock row: $RD"
printf '%s' "$RD" | grep -q '"available_quantity"' && ok "report exposes available_quantity" || no "no available_quantity"
# Customers (6) and Inventory Adjustments (7) are virtual — must not appear.
printf '%s' "$RD" | grep -q '"location_id":\[6' && no "customer location leaked into On Hand" || ok "virtual locations excluded from On Hand"

echo
echo "############ cleanup ############"
pg "DELETE FROM stock_move   WHERE product_id=$PRODUCT" >/dev/null
pg "DELETE FROM stock_picking WHERE origin='SQTEST'"    >/dev/null
pg "DELETE FROM stock_quant   WHERE product_id=$PRODUCT" >/dev/null
pg "DELETE FROM product_product WHERE id=$PRODUCT"       >/dev/null
rm -f "$CK" /tmp/sq_auth.json /tmp/sq_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

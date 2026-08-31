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
# Subcontracting — the receipt IS the manufacturing event.
#
# Proves through the real HTTP path:
#   * a product with a subcontract BOM, received on a vendor receipt,
#     lands on-hand as normal
#   * validating that receipt backflushes the BOM components: they are
#     consumed from WH/Stock into the Subcontracting location
#   * a normal (non-subcontract) receipt does NOT backflush anything
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/sc_cookie.txt
cat > /tmp/sc_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/sc_auth.json > /tmp/sc_auth_out.json
grep -q '"session_id"' /tmp/sc_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
M=1000000

VENDOR=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
SP=$(pg  "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available) VALUES ('SCTEST-Finished','product',1,1,true,0) RETURNING id")
SC1=$(pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available) VALUES ('SCTEST-Comp','product',1,1,true,0) RETURNING id")
NP=$(pg  "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available) VALUES ('SCTEST-Normal','product',1,1,true,0) RETURNING id")
BOM=$(pg "INSERT INTO mrp_bom (product_id,code,bom_type,product_qty,product_uom_id,company_id,active,subcontractor_id) VALUES ($SP,'SCTEST-BOM','subcontract',$M,1,1,true,$VENDOR) RETURNING id")
pg "INSERT INTO mrp_bom_line (bom_id,product_id,product_qty,product_uom_id,sequence) VALUES ($BOM,$SC1,$((2*M)),1,10)" >/dev/null
callkw stock.quant set_on_hand "[{\"product_id\":$SC1,\"location_id\":4,\"quantity\":100}]" >/dev/null
echo "    vendor=$VENDOR SP=$SP SC1=$SC1 NP=$NP BOM=$BOM (subcontract)"
[ -n "$BOM" ] && ok "subcontract BOM created with a subcontractor" || { no "fixture setup failed"; exit 1; }

mkreceipt() { # $1 name $2 product $3 qty -> picking id
    local pk=$(pg "INSERT INTO stock_picking (name,picking_type_id,state,partner_id,location_id,location_dest_id,company_id,origin) VALUES ('$1',1,'draft',$VENDOR,5,4,1,'SCTEST') RETURNING id")
    pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id) VALUES ($pk,$2,'recv',$(($3*M)),0,'draft',5,4,1)" >/dev/null
    echo "$pk"
}

echo "############ 1. receive a subcontracted product ############"
SUBLOC=$(pg "SELECT id FROM stock_location WHERE usage='subcontract' ORDER BY id LIMIT 1")
echo "    subcontracting location id=$SUBLOC"
[ "$SUBLOC" = "9" ] && ok "Subcontracting location seeded" || no "subcontract loc=$SUBLOC"
PICK=$(mkreceipt 'SCTEST-IN' "$SP" 5)
callkw stock.picking action_confirm  "[[$PICK]]" >/dev/null
callkw stock.picking button_validate "[[$PICK]]" >/dev/null

ONSP=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$SP AND location_id=4")
ONSC=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$SC1 AND location_id=4")
ATSUB=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$SC1 AND location_id=$SUBLOC")
QASP=$(pg "SELECT qty_available FROM product_product WHERE id=$SP")
BFM=$(pg "SELECT count(*) FROM stock_move WHERE product_id=$SC1 AND origin='Subcontract'")
echo "    SP on-hand=$ONSP  SC1 on-hand=$ONSC  SC1@Subcontracting=$ATSUB  SP.qty_available=$QASP  backflush moves=$BFM"
[ "$ONSP" = "5000000" ]  && ok "finished good received: on-hand = 5"        || no "SP on-hand=$ONSP"
[ "$ONSC" = "90000000" ] && ok "components backflushed: 100 - (2 x 5) = 90" || no "SC1 on-hand=$ONSC"
[ "$ATSUB" = "10000000" ] && ok "consumed 10 into Subcontracting location"  || no "SC1@sub=$ATSUB"
[ "$QASP" = "5000000" ]  && ok "SP.qty_available = 5"                       || no "qty_available=$QASP"
[ "$BFM" -ge 1 ] 2>/dev/null && ok "a backflush component move was recorded" || no "backflush moves=$BFM"

echo
echo "############ 2. a normal receipt does NOT backflush ############"
PICK2=$(mkreceipt 'SCTEST-IN2' "$NP" 7)
callkw stock.picking action_confirm  "[[$PICK2]]" >/dev/null
callkw stock.picking button_validate "[[$PICK2]]" >/dev/null
ONNP=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$NP AND location_id=4")
# SC1 must be unchanged from the previous step (still 90) — no spurious backflush.
ONSC2=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$SC1 AND location_id=4")
echo "    NP on-hand=$ONNP  SC1 on-hand still=$ONSC2"
[ "$ONNP" = "7000000" ]  && ok "normal product received normally: 7"        || no "NP on-hand=$ONNP"
[ "$ONSC2" = "90000000" ] && ok "no backflush for a non-subcontract product" || no "SC1 changed to $ONSC2"

echo
echo "############ cleanup ############"
pg "DELETE FROM stock_move WHERE picking_id IN ($PICK,$PICK2) OR (product_id=$SC1 AND origin='Subcontract')" >/dev/null
pg "DELETE FROM stock_picking WHERE origin='SCTEST'" >/dev/null
pg "DELETE FROM mrp_bom_line WHERE bom_id=$BOM" >/dev/null
pg "DELETE FROM mrp_bom WHERE id=$BOM" >/dev/null
pg "DELETE FROM stock_quant WHERE product_id IN ($SP,$SC1,$NP)" >/dev/null
pg "DELETE FROM product_product WHERE id IN ($SP,$SC1,$NP)" >/dev/null
rm -f "$CK" /tmp/sc_auth.json /tmp/sc_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

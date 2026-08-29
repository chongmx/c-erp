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
# mrp.production — manufacturing orders, BOM explosion, work centers.
#
# Proves the manufacturing core through the real HTTP path:
#   * confirming an MO explodes its BOM into raw + finished stock moves,
#     scaled to the order quantity, and assigns an MO/##### number
#   * marking it done consumes components and produces the finished good
#     through the quant engine (on-hand + qty_available move correctly)
#   * work centers and BOM operations (routing) are CRUD-able
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/mo_cookie.txt
cat > /tmp/mo_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/mo_auth.json > /tmp/mo_auth_out.json
grep -q '"session_id"' /tmp/mo_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() { # $1 model $2 method $3 args
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
rval() { python3 -c "import json,sys; print(json.load(sys.stdin).get('result',''))" 2>/dev/null; }

M=1000000

# ---- fixtures: finished product P, components C1/C2, a BOM ----
P=$(pg  "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available) VALUES ('MOTEST-P','product',1,1,true,0) RETURNING id")
C1=$(pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available) VALUES ('MOTEST-C1','product',1,1,true,0) RETURNING id")
C2=$(pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available) VALUES ('MOTEST-C2','product',1,1,true,0) RETURNING id")
BOM=$(pg "INSERT INTO mrp_bom (product_id,code,bom_type,product_qty,product_uom_id,company_id,active) VALUES ($P,'MOTEST-BOM','normal',$M,1,1,true) RETURNING id")
pg "INSERT INTO mrp_bom_line (bom_id,product_id,product_qty,product_uom_id,sequence) VALUES ($BOM,$C1,$((2*M)),1,10)" >/dev/null
pg "INSERT INTO mrp_bom_line (bom_id,product_id,product_qty,product_uom_id,sequence) VALUES ($BOM,$C2,$((3*M)),1,20)" >/dev/null
echo "    P=$P C1=$C1 C2=$C2 BOM=$BOM"
[ -n "$P" ] && [ -n "$BOM" ] && ok "fixtures created" || { no "fixture setup failed"; exit 1; }

# Seed component stock: 100 each at WH/Stock via inventory adjustment.
callkw stock.quant set_on_hand "[{\"product_id\":$C1,\"location_id\":4,\"quantity\":100}]" >/dev/null
callkw stock.quant set_on_hand "[{\"product_id\":$C2,\"location_id\":4,\"quantity\":100}]" >/dev/null

echo "############ 1. create + confirm MO explodes the BOM ############"
MO=$(callkw mrp.production create "[{\"product_id\":$P,\"product_qty\":5,\"bom_id\":$BOM,\"location_src_id\":4,\"location_dest_id\":4}]" | rval)
echo "    MO id=$MO"
[ -n "$MO" ] && ok "MO created" || { no "MO create failed"; exit 1; }
callkw mrp.production action_confirm "[[$MO]]" >/dev/null
NAME=$(pg "SELECT name FROM mrp_production WHERE id=$MO")
STATE=$(pg "SELECT state FROM mrp_production WHERE id=$MO")
echo "    MO name=$NAME state=$STATE"
printf '%s' "$NAME" | grep -qE '^MO/[0-9]{5}$' && ok "MO numbered MO/##### ($NAME)" || no "MO name=$NAME"
[ "$STATE" = "confirmed" ] && ok "MO confirmed" || no "state=$STATE"

# Raw moves: C1 = 2*5 = 10, C2 = 3*5 = 15; finished = 5. Production loc = 8.
RAWN=$(pg "SELECT count(*) FROM stock_move WHERE production_id=$MO AND is_production_raw=TRUE")
FINN=$(pg "SELECT count(*) FROM stock_move WHERE production_id=$MO AND is_production_raw=FALSE")
QC1=$(pg "SELECT product_uom_qty FROM stock_move WHERE production_id=$MO AND product_id=$C1")
QC2=$(pg "SELECT product_uom_qty FROM stock_move WHERE production_id=$MO AND product_id=$C2")
QF=$(pg  "SELECT product_uom_qty FROM stock_move WHERE production_id=$MO AND product_id=$P")
DEST=$(pg "SELECT location_dest_id FROM stock_move WHERE production_id=$MO AND product_id=$C1")
echo "    raw moves=$RAWN finished=$FINN  C1=$QC1 C2=$QC2 finished_qty=$QF raw_dest=$DEST"
[ "$RAWN" = "2" ] && ok "two raw-material moves created" || no "raw move count=$RAWN"
[ "$FINN" = "1" ] && ok "one finished move created"      || no "finished move count=$FINN"
[ "$QC1" = "10000000" ] && ok "C1 scaled to 10 (2 x 5)" || no "C1 qty=$QC1"
[ "$QC2" = "15000000" ] && ok "C2 scaled to 15 (3 x 5)" || no "C2 qty=$QC2"
[ "$QF"  = "5000000" ]  && ok "finished qty = 5"        || no "finished qty=$QF"
[ "$DEST" = "8" ] && ok "raw moves flow into Production (loc 8)" || no "raw dest=$DEST"

echo
echo "############ 2. mark done consumes + produces via quant ############"
callkw mrp.production button_mark_done "[[$MO]]" >/dev/null
ST2=$(pg "SELECT state FROM mrp_production WHERE id=$MO")
ONC1=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$C1 AND location_id=4")
ONC2=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$C2 AND location_id=4")
ONP=$(pg  "SELECT quantity FROM stock_quant WHERE product_id=$P AND location_id=4")
QAP=$(pg  "SELECT qty_available FROM product_product WHERE id=$P")
echo "    MO state=$ST2  C1 on-hand=$ONC1  C2 on-hand=$ONC2  P on-hand=$ONP  P.qty_available=$QAP"
[ "$ST2" = "done" ] && ok "MO done"                           || no "state=$ST2"
[ "$ONC1" = "90000000" ] && ok "C1 consumed: 100 - 10 = 90"   || no "C1 on-hand=$ONC1"
[ "$ONC2" = "85000000" ] && ok "C2 consumed: 100 - 15 = 85"   || no "C2 on-hand=$ONC2"
[ "$ONP"  = "5000000" ]  && ok "P produced: on-hand = 5"      || no "P on-hand=$ONP"
[ "$QAP"  = "5000000" ]  && ok "P.qty_available = 5"          || no "qty_available=$QAP"

echo
echo "############ 3. work center + BOM operation (routing) CRUD ############"
WC=$(callkw mrp.workcenter create "[{\"name\":\"MOTEST Assembly\",\"code\":\"MOASM\",\"costs_hour\":50,\"time_efficiency\":100,\"capacity\":1}]" | rval)
echo "    work center id=$WC"
[ -n "$WC" ] && ok "work center created" || no "workcenter create failed"
OP=$(callkw mrp.routing.workcenter create "[{\"bom_id\":$BOM,\"workcenter_id\":$WC,\"name\":\"Assemble\",\"sequence\":10,\"time_cycle_manual\":30}]" | rval)
echo "    routing op id=$OP"
[ -n "$OP" ] && ok "BOM operation created" || no "routing create failed"
RD=$(callkw mrp.routing.workcenter search_read "[[[\"bom_id\",\"=\",$BOM]]]")
printf '%s' "$RD" | grep -q '"name":"Assemble"' && ok "operation reads back on the BOM" || no "routing read: $(printf '%s' "$RD" | head -c 120)"
printf '%s' "$RD" | grep -q 'MOTEST Assembly' && ok "operation shows its work center" || no "workcenter name missing"

echo
echo "############ cleanup ############"
pg "DELETE FROM stock_move   WHERE production_id=$MO" >/dev/null
pg "DELETE FROM mrp_production WHERE id=$MO" >/dev/null
pg "DELETE FROM mrp_routing_workcenter WHERE bom_id=$BOM" >/dev/null
pg "DELETE FROM mrp_bom_line WHERE bom_id=$BOM" >/dev/null
pg "DELETE FROM mrp_bom WHERE id=$BOM" >/dev/null
pg "DELETE FROM mrp_workcenter WHERE id=$WC" >/dev/null
pg "DELETE FROM stock_quant WHERE product_id IN ($P,$C1,$C2)" >/dev/null
pg "DELETE FROM product_product WHERE id IN ($P,$C1,$C2)" >/dev/null
rm -f "$CK" /tmp/mo_auth.json /tmp/mo_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

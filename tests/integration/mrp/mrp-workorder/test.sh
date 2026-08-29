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
# mrp.workorder — per-operation execution, and the MO close-gate.
#
# Proves through the real HTTP path:
#   * confirming an MO with a BOM operation creates a work order, with
#     an expected duration scaled to the order quantity
#   * the MO cannot be closed while a work order is still open
#   * starting then finishing the work order moves the MO to 'to_close'
#   * only then does mark-done consume/produce through the quant engine
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/wo_cookie.txt
cat > /tmp/wo_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/wo_auth.json > /tmp/wo_auth_out.json
grep -q '"session_id"' /tmp/wo_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
rval() { python3 -c "import json,sys; print(json.load(sys.stdin).get('result',''))" 2>/dev/null; }

M=1000000

P=$(pg  "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available) VALUES ('WOTEST-P','product',1,1,true,0) RETURNING id")
C1=$(pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available) VALUES ('WOTEST-C1','product',1,1,true,0) RETURNING id")
BOM=$(pg "INSERT INTO mrp_bom (product_id,code,bom_type,product_qty,product_uom_id,company_id,active) VALUES ($P,'WOTEST-BOM','normal',$M,1,1,true) RETURNING id")
pg "INSERT INTO mrp_bom_line (bom_id,product_id,product_qty,product_uom_id,sequence) VALUES ($BOM,$C1,$((2*M)),1,10)" >/dev/null
WC=$(callkw mrp.workcenter create "[{\"name\":\"WOTEST WC\",\"code\":\"WOWC\",\"costs_hour\":40}]" | rval)
OP=$(callkw mrp.routing.workcenter create "[{\"bom_id\":$BOM,\"workcenter_id\":$WC,\"name\":\"Assemble\",\"sequence\":10,\"time_cycle_manual\":30}]" | rval)
callkw stock.quant set_on_hand "[{\"product_id\":$C1,\"location_id\":4,\"quantity\":100}]" >/dev/null
echo "    P=$P C1=$C1 BOM=$BOM WC=$WC OP=$OP"
[ -n "$OP" ] && ok "fixtures + BOM operation created" || { no "fixture setup failed"; exit 1; }

echo "############ 1. confirm MO creates a work order ############"
MO=$(callkw mrp.production create "[{\"product_id\":$P,\"product_qty\":4,\"bom_id\":$BOM,\"location_src_id\":4,\"location_dest_id\":4}]" | rval)
callkw mrp.production action_confirm "[[$MO]]" >/dev/null
WON=$(pg "SELECT count(*) FROM mrp_workorder WHERE production_id=$MO")
WO=$(pg  "SELECT id FROM mrp_workorder WHERE production_id=$MO ORDER BY id LIMIT 1")
WOSTATE=$(pg "SELECT state FROM mrp_workorder WHERE id=$WO")
DE=$(pg "SELECT duration_expected FROM mrp_workorder WHERE id=$WO")
echo "    MO=$MO  work orders=$WON  WO=$WO state=$WOSTATE duration_expected=$DE"
[ "$WON" = "1" ] && ok "one work order created from the BOM operation" || no "WO count=$WON"
[ "$WOSTATE" = "ready" ] && ok "work order starts 'ready'" || no "WO state=$WOSTATE"
[ "${DE%.*}" = "120" ] && ok "expected duration = 30 min x 4 = 120" || no "duration_expected=$DE"

echo
echo "############ 2. MO cannot close while a work order is open ############"
callkw mrp.production button_mark_done "[[$MO]]" > /tmp/wo_blocked.json
STB=$(pg "SELECT state FROM mrp_production WHERE id=$MO")
echo "    MO state after blocked mark_done=$STB"
[ "$STB" != "done" ] && ok "mark_done refused while WO open (MO still $STB)" || no "MO closed despite open WO"

echo
echo "############ 3. start + finish the work order ############"
callkw mrp.workorder button_start "[[$WO]]" >/dev/null
WS1=$(pg "SELECT state FROM mrp_workorder WHERE id=$WO")
MS1=$(pg "SELECT state FROM mrp_production WHERE id=$MO")
echo "    after start: WO=$WS1  MO=$MS1"
[ "$WS1" = "progress" ] && ok "work order in progress" || no "WO state=$WS1"
[ "$MS1" = "progress" ] && ok "MO moved to progress"   || no "MO state=$MS1"
callkw mrp.workorder button_finish "[[$WO]]" >/dev/null
WS2=$(pg "SELECT state FROM mrp_workorder WHERE id=$WO")
MS2=$(pg "SELECT state FROM mrp_production WHERE id=$MO")
DUR=$(pg "SELECT duration FROM mrp_workorder WHERE id=$WO")
echo "    after finish: WO=$WS2  MO=$MS2  logged duration=$DUR"
[ "$WS2" = "done" ] && ok "work order done" || no "WO state=$WS2"
[ "$MS2" = "to_close" ] && ok "MO ready to close (to_close)" || no "MO state=$MS2"
[ "${DUR%.*}" = "120" ] && ok "duration defaulted from expected (120)" || no "duration=$DUR"

echo
echo "############ 4. now mark done consumes + produces ############"
callkw mrp.production button_mark_done "[[$MO]]" >/dev/null
MS3=$(pg "SELECT state FROM mrp_production WHERE id=$MO")
ONC=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$C1 AND location_id=4")
ONP=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$P AND location_id=4")
echo "    MO=$MS3  C1 on-hand=$ONC  P on-hand=$ONP"
[ "$MS3" = "done" ] && ok "MO closed once all work orders finished" || no "MO state=$MS3"
[ "$ONC" = "92000000" ] && ok "C1 consumed: 100 - 8 = 92" || no "C1 on-hand=$ONC"
[ "$ONP" = "4000000" ]  && ok "P produced: on-hand = 4"    || no "P on-hand=$ONP"

echo
echo "############ cleanup ############"
pg "DELETE FROM mrp_workorder WHERE production_id=$MO" >/dev/null
pg "DELETE FROM stock_move WHERE production_id=$MO" >/dev/null
pg "DELETE FROM mrp_production WHERE id=$MO" >/dev/null
pg "DELETE FROM mrp_routing_workcenter WHERE bom_id=$BOM" >/dev/null
pg "DELETE FROM mrp_bom_line WHERE bom_id=$BOM" >/dev/null
pg "DELETE FROM mrp_bom WHERE id=$BOM" >/dev/null
pg "DELETE FROM mrp_workcenter WHERE id=$WC" >/dev/null
pg "DELETE FROM stock_quant WHERE product_id IN ($P,$C1)" >/dev/null
pg "DELETE FROM product_product WHERE id IN ($P,$C1)" >/dev/null
rm -f "$CK" /tmp/wo_auth.json /tmp/wo_auth_out.json /tmp/wo_blocked.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

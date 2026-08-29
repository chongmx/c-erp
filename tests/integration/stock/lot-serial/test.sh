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
# Lots & serial numbers.
#
# Proves through the real HTTP path:
#   * on-hand is tracked per (product, location, lot)
#   * receipts/deliveries carry a lot, and reservation/validation follow it
#   * a lot's traceability = current on-hand + full move history
#   * a serial is one unit; a tracked product refuses validation without a lot
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/lot_cookie.txt
cat > /tmp/lot_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/lot_auth.json > /tmp/lot_auth_out.json
grep -q '"session_id"' /tmp/lot_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
rval() { python3 -c "import json,sys; print(json.load(sys.stdin).get('result',''))" 2>/dev/null; }
M=1000000

mkprod() { pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available,tracking) VALUES ('$1','product',1,1,true,0,'$2') RETURNING id"; }
mklot()  { callkw stock.production.lot create "[{\"name\":\"$1\",\"product_id\":$2}]" | rval; }
mkrecv() { # product qty lot(0=none) -> picking id
    local pk=$(pg "INSERT INTO stock_picking (name,picking_type_id,state,location_id,location_dest_id,company_id,origin) VALUES ('LOT-IN',1,'draft',5,4,1,'LOTTEST') RETURNING id")
    if [ "$3" = "0" ]; then
        pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id) VALUES ($pk,$1,'r',$(($2*M)),0,'draft',5,4,1)" >/dev/null
    else
        pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id,lot_id) VALUES ($pk,$1,'r',$(($2*M)),0,'draft',5,4,1,$3)" >/dev/null
    fi
    echo "$pk"
}
mkdel() { # product qty lot -> picking id
    local pk=$(pg "INSERT INTO stock_picking (name,picking_type_id,state,location_id,location_dest_id,company_id,origin) VALUES ('LOT-OUT',2,'draft',4,6,1,'LOTTEST') RETURNING id")
    pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id,lot_id) VALUES ($pk,$1,'d',$(($2*M)),0,'draft',4,6,1,$3)" >/dev/null
    echo "$pk"
}
val() { callkw stock.picking action_confirm "[[$1]]" >/dev/null; callkw stock.picking button_validate "[[$1]]" >/dev/null; }

echo "############ 1. on-hand tracked per lot ############"
PL=$(mkprod 'LOTTEST-PL' 'lot')
LA=$(mklot 'LOT-A' "$PL"); LB=$(mklot 'LOT-B' "$PL")
echo "    PL=$PL  LOT-A=$LA  LOT-B=$LB"
[ -n "$LA" ] && [ -n "$LB" ] && ok "two lots created" || { no "lot creation failed"; exit 1; }
val "$(mkrecv "$PL" 10 "$LA")"   # 10 into LOT-A
val "$(mkrecv "$PL" 5  "$LB")"   # 5  into LOT-B
QA=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$PL AND location_id=4 AND lot_id=$LA")
QB=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$PL AND location_id=4 AND lot_id=$LB")
TOT=$(pg "SELECT qty_available FROM product_product WHERE id=$PL")
echo "    LOT-A on-hand=$QA  LOT-B on-hand=$QB  total qty_available=$TOT"
[ "$QA" = "10000000" ] && ok "LOT-A holds 10" || no "LOT-A=$QA"
[ "$QB" = "5000000" ]  && ok "LOT-B holds 5"  || no "LOT-B=$QB"
[ "$TOT" = "15000000" ] && ok "qty_available sums lots = 15" || no "total=$TOT"

echo
echo "############ 2. deliver consumes the named lot ############"
val "$(mkdel "$PL" 4 "$LA")"     # ship 4 from LOT-A
QA2=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$PL AND location_id=4 AND lot_id=$LA")
QB2=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$PL AND location_id=4 AND lot_id=$LB")
echo "    LOT-A on-hand=$QA2  LOT-B on-hand=$QB2"
[ "$QA2" = "6000000" ] && ok "LOT-A drawn down to 6" || no "LOT-A=$QA2"
[ "$QB2" = "5000000" ] && ok "LOT-B untouched at 5"  || no "LOT-B=$QB2"

echo
echo "############ 3. lot traceability ############"
TR=$(callkw stock.production.lot traceability "[{\"lot_id\":$LA}]")
OH=$(printf '%s' "$TR" | python3 -c "import json,sys;print(json.load(sys.stdin)['result']['on_hand'])" 2>/dev/null)
NM=$(printf '%s' "$TR" | python3 -c "import json,sys;print(len(json.load(sys.stdin)['result']['moves']))" 2>/dev/null)
echo "    LOT-A traceability: on_hand=$OH  moves=$NM"
awk -v a="$OH" 'BEGIN{exit !(a+0==6)}' && ok "traceability on-hand = 6" || no "on_hand=$OH"
[ "${NM:-0}" -ge 2 ] 2>/dev/null && ok "move history shows receipt + delivery ($NM moves)" || no "moves=$NM"

echo
echo "############ 4. serial = one unit ############"
PSER=$(mkprod 'LOTTEST-SN' 'serial')
SN1=$(mklot 'SN-1' "$PSER")
val "$(mkrecv "$PSER" 1 "$SN1")"
QS=$(pg "SELECT quantity FROM stock_quant WHERE product_id=$PSER AND location_id=4 AND lot_id=$SN1")
echo "    serial SN-1 on-hand=$QS"
[ "$QS" = "1000000" ] && ok "serial SN-1 holds exactly 1" || no "SN-1=$QS"

echo
echo "############ 5. enforcement ############"
# a tracked product refuses validation without a lot
P_NOLOT=$(mkrecv "$PL" 3 0)
val "$P_NOLOT"
ST1=$(pg "SELECT state FROM stock_picking WHERE id=$P_NOLOT")
echo "    tracked receipt without a lot -> state=$ST1"
[ "$ST1" != "done" ] && ok "validation refused (no lot on a tracked product)" || no "validated without a lot"
# a serial move must be one unit
SN2=$(mklot 'SN-2' "$PSER")
P_SER2=$(mkrecv "$PSER" 2 "$SN2")
val "$P_SER2"
ST2=$(pg "SELECT state FROM stock_picking WHERE id=$P_SER2")
echo "    serial receipt of 2 units -> state=$ST2"
[ "$ST2" != "done" ] && ok "validation refused (serial must be one unit)" || no "validated 2 units on a serial"

echo
echo "############ cleanup ############"
pg "DELETE FROM stock_move WHERE origin='LOTTEST' OR product_id IN ($PL,$PSER)" >/dev/null
pg "DELETE FROM stock_picking WHERE origin='LOTTEST'" >/dev/null
pg "DELETE FROM stock_quant WHERE product_id IN ($PL,$PSER)" >/dev/null
pg "DELETE FROM stock_valuation_layer WHERE product_id IN ($PL,$PSER)" >/dev/null
pg "DELETE FROM stock_production_lot WHERE product_id IN ($PL,$PSER)" >/dev/null
pg "DELETE FROM product_product WHERE id IN ($PL,$PSER)" >/dev/null
rm -f "$CK" /tmp/lot_auth.json /tmp/lot_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

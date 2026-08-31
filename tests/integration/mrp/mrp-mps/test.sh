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
# MPS — master production schedule: forecast → projected stock →
# suggested replenishment → draft MO.
#
# Proves through the real HTTP path:
#   * get_mps_grid runs the time-phased projection: on-hand carries
#     forward, demand is subtracted, replenishment is suggested to hold
#     the projected stock at/above the safety level
#   * action_replenish turns a suggested quantity into a draft MO
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }
# numeric equality (tolerant of 25 vs 25.0)
eqnum() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a+0==b+0)}'; }

CK=/tmp/mps_cookie.txt
cat > /tmp/mps_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/mps_auth.json > /tmp/mps_auth_out.json
grep -q '"session_id"' /tmp/mps_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}

MP=$(pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available) VALUES ('MPSTEST','product',1,1,true,0) RETURNING id")
echo "    product id=$MP"
[ -n "$MP" ] && ok "product created" || { no "setup failed"; exit 1; }

# On-hand 20; safety stock 10.
callkw stock.quant set_on_hand "[{\"product_id\":$MP,\"location_id\":4,\"quantity\":20}]" >/dev/null
callkw mrp.production.schedule create "[{\"product_id\":$MP,\"min_to_replenish\":10}]" >/dev/null

# Forecast demand: Sep 5, Oct 30, Nov 5.
callkw mrp.production.schedule set_forecast "[{\"product_id\":$MP,\"date\":\"2026-09-01\",\"quantity\":5}]"  >/dev/null
callkw mrp.production.schedule set_forecast "[{\"product_id\":$MP,\"date\":\"2026-10-01\",\"quantity\":30}]" >/dev/null
callkw mrp.production.schedule set_forecast "[{\"product_id\":$MP,\"date\":\"2026-11-01\",\"quantity\":5}]"  >/dev/null

echo "############ 1. time-phased projection ############"
GRID=$(callkw mrp.production.schedule get_mps_grid "[{\"product_id\":$MP,\"periods\":3,\"start\":\"2026-09-01\"}]")
eval "$(printf '%s' "$GRID" | python3 -c "
import json,sys
d=json.load(sys.stdin)['result']; p=d['periods']
print('OH=%s'%d['on_hand'])
for i,x in enumerate(p):
    print('F%d=%s'%(i,x['forecast']))
    print('R%d=%s'%(i,x['to_replenish']))
    print('S%d=%s'%(i,x['forecast_stock']))
" 2>/dev/null)"
echo "    on_hand=$OH  Sep(F/R/S)=$F0/$R0/$S0  Oct=$F1/$R1/$S1  Nov=$F2/$R2/$S2"
eqnum "$OH" 20 && ok "starting on-hand = 20" || no "on_hand=$OH"
# Sep: 20 - 5 = 15 (>= 10) → no replenishment, stock 15
eqnum "$R0" 0 && eqnum "$S0" 15 && ok "Sep: demand 5, no replenish, stock 15" || no "Sep R=$R0 S=$S0"
# Oct: 15 - 30 = -15 (< 10) → replenish 25 to reach 10
eqnum "$R1" 25 && eqnum "$S1" 10 && ok "Oct: demand 30, replenish 25 to hit safety 10" || no "Oct R=$R1 S=$S1"
# Nov: 10 - 5 = 5 (< 10) → replenish 5 to reach 10
eqnum "$R2" 5 && eqnum "$S2" 10 && ok "Nov: demand 5, replenish 5 to hit safety 10" || no "Nov R=$R2 S=$S2"

echo
echo "############ 2. replenish creates a draft MO ############"
MOID=$(callkw mrp.production.schedule action_replenish "[{\"product_id\":$MP,\"quantity\":25,\"date\":\"2026-10-01\"}]" \
        | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['production_id'])" 2>/dev/null)
echo "    replenishment MO id=$MOID"
[ -n "$MOID" ] && ok "action_replenish returned an MO" || no "no MO id"
MOQ=$(pg "SELECT product_qty FROM mrp_production WHERE id=$MOID")
MOS=$(pg "SELECT state FROM mrp_production WHERE id=$MOID")
MOO=$(pg "SELECT origin FROM mrp_production WHERE id=$MOID")
echo "    MO product_qty=$MOQ state=$MOS origin=$MOO"
[ "$MOQ" = "25000000" ] && ok "draft MO for 25 units" || no "MO qty=$MOQ"
[ "$MOS" = "draft" ] && ok "MO is draft (planner reviews before launch)" || no "MO state=$MOS"
[ "$MOO" = "MPS" ] && ok "MO traced to MPS origin" || no "MO origin=$MOO"

echo
echo "############ cleanup ############"
pg "DELETE FROM mrp_production WHERE id=$MOID" >/dev/null
pg "DELETE FROM mrp_forecast WHERE product_id=$MP" >/dev/null
pg "DELETE FROM mrp_production_schedule WHERE product_id=$MP" >/dev/null
pg "DELETE FROM stock_quant WHERE product_id=$MP" >/dev/null
pg "DELETE FROM product_product WHERE id=$MP" >/dev/null
rm -f "$CK" /tmp/mps_auth.json /tmp/mps_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

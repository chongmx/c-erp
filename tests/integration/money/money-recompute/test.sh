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
# P2 Phase 4 probe: do the module recompute paths (custom SQL +
# C++ arithmetic) still produce correct totals after the columns
# became BIGINT micro-units?
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

FAILED=
pg() { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cat > /tmp/vr_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/vr_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }
source scripts/seed_test_fixtures.sh; ensure_sale_fixture "$SID" >/dev/null 2>&1

LID=$(pg "SELECT id FROM sale_order_line ORDER BY id LIMIT 1")
OID=$(pg "SELECT order_id FROM sale_order_line WHERE id=$LID")
echo "sale_order_line id=$LID (order $OID)"

echo "--- before ---"
pg "SELECT price_unit||' | '||product_uom_qty||' | '||price_subtotal||' | '||price_total FROM sale_order_line WHERE id=$LID"

cat > /tmp/vr_w.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"sale.order.line","method":"write",
 "args":[[$LID],{"price_unit":10.00,"product_uom_qty":3}],
 "kwargs":{"context":{"session_id":"$SID"}}}}
EOF
curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
     --data @/tmp/vr_w.json | head -c 140
echo

echo "--- after: DB (micro-units) ---"
DBPU=$(pg "SELECT price_unit      FROM sale_order_line WHERE id=$LID")
DBQT=$(pg "SELECT product_uom_qty FROM sale_order_line WHERE id=$LID")
DBST=$(pg "SELECT price_subtotal  FROM sale_order_line WHERE id=$LID")
DBTT=$(pg "SELECT price_total     FROM sale_order_line WHERE id=$LID")
echo "  price_unit=$DBPU  qty=$DBQT  subtotal=$DBST  total=$DBTT"
[ "$DBPU" = "10000000" ] && ok "price_unit stored as 10.00 in micro-units" || no "price_unit is $DBPU, expected 10000000"
[ "$DBQT" = "3000000" ]  && ok "quantity stored as 3 in micro-units"       || no "quantity is $DBQT, expected 3000000"
[ "$DBST" = "30000000" ] && ok "subtotal recomputed to 30.00"              || no "subtotal is $DBST, expected 30000000"
[ "$DBTT" = "30000000" ] && ok "total recomputed to 30.00"                 || no "total is $DBTT, expected 30000000"

echo "--- after: what the API reports ---"
# The DB check above proves the arithmetic; this proves the micro->major
# conversion on the way out. A recompute path that treated micro-units as
# major units would show 10000000.0 here rather than 10.0.
cat > /tmp/vr_r.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"sale.order.line","method":"search_read",
 "args":[[["id","=",$LID]]],
 "kwargs":{"fields":["price_unit","product_uom_qty","price_subtotal","price_total"],
           "context":{"session_id":"$SID"}}}}
EOF
API=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/vr_r.json)
echo "  $API"
echo "$API" | grep -q '"price_unit":10.0'      && ok "API reports price_unit 10.0"  || no "API price_unit wrong"
echo "$API" | grep -q '"product_uom_qty":3.0'  && ok "API reports quantity 3.0"     || no "API quantity wrong"
echo "$API" | grep -q '"price_subtotal":30.0'  && ok "API reports subtotal 30.0"    || no "API subtotal wrong"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

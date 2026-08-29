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
# Barcode — fields on product/location/lot + a scan resolver.
#
# Proves through the real HTTP path: resolve_barcode turns a scanned code
# into what it is (product / location / lot), and 'unknown' otherwise.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/bc_cookie.txt
cat > /tmp/bc_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/bc_auth.json > /tmp/bc_auth_out.json
grep -q '"session_id"' /tmp/bc_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
rtype() { python3 -c "import json,sys; print(json.load(sys.stdin)['result']['type'])" 2>/dev/null; }

P=$(pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available,barcode) VALUES ('BCTEST-Prod','product',1,1,true,0,'PROD-123') RETURNING id")
L=$(pg "INSERT INTO stock_location (name,complete_name,location_id,usage,company_id,barcode) VALUES ('BCTEST-Loc','WH/Stock/BC',4,'internal',1,'LOC-456') RETURNING id")
LOT=$(pg "INSERT INTO stock_production_lot (name,product_id,barcode,company_id) VALUES ('BCTEST-Lot',$P,'LOT-789',1) RETURNING id")
echo "    product=$P (PROD-123)  location=$L (LOC-456)  lot=$LOT (LOT-789)"

echo "############ scan resolves to the right record ############"
T1=$(callkw stock.quant resolve_barcode "[{\"barcode\":\"PROD-123\"}]" | rtype)
T2=$(callkw stock.quant resolve_barcode "[{\"barcode\":\"LOC-456\"}]"  | rtype)
T3=$(callkw stock.quant resolve_barcode "[{\"barcode\":\"LOT-789\"}]"  | rtype)
T4=$(callkw stock.quant resolve_barcode "[{\"barcode\":\"NOPE-000\"}]" | rtype)
echo "    PROD-123 -> $T1 ; LOC-456 -> $T2 ; LOT-789 -> $T3 ; NOPE-000 -> $T4"
[ "$T1" = "product" ]  && ok "product barcode resolves to a product"   || no "PROD-123 -> $T1"
[ "$T2" = "location" ] && ok "location barcode resolves to a location" || no "LOC-456 -> $T2"
[ "$T3" = "lot" ]      && ok "lot barcode resolves to a lot"           || no "LOT-789 -> $T3"
[ "$T4" = "unknown" ]  && ok "an unknown code resolves to 'unknown'"   || no "NOPE-000 -> $T4"

# confirm the resolved id is correct for the product
PID=$(callkw stock.quant resolve_barcode "[{\"barcode\":\"PROD-123\"}]" | python3 -c "import json,sys;print(json.load(sys.stdin)['result']['id'])" 2>/dev/null)
[ "$PID" = "$P" ] && ok "resolved id matches the product" || no "resolved id=$PID vs $P"

echo
echo "############ cleanup ############"
pg "DELETE FROM stock_production_lot WHERE id=$LOT" >/dev/null
pg "DELETE FROM product_product WHERE id=$P" >/dev/null
pg "DELETE FROM stock_location WHERE id=$L" >/dev/null
rm -f "$CK" /tmp/bc_auth.json /tmp/bc_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

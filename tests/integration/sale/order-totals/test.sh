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
# Does writing a sale.order.line actually refresh the parent order's
# header totals?
#
# The ledger-integrity SQL flagged order 2 with a header of 375.00 against
# a single line of 30.00. Either the header is not recomputed on a line
# write — a real bug — or the discrepancy is stale data left by lines that
# were removed with raw SQL, which no recompute would ever see.
#
# This decides which, by writing through the API and re-reading.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cat > /tmp/vot_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/vot_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }

OID=$(pg "SELECT order_id FROM sale_order_line ORDER BY id LIMIT 1")
LID=$(pg "SELECT id FROM sale_order_line ORDER BY id LIMIT 1")
echo "############ order $OID, line $LID ############"
echo "    header before: $(pg "SELECT amount_total FROM sale_order WHERE id=$OID")"
echo "    lines  before: $(pg "SELECT COALESCE(SUM(price_total),0) FROM sale_order_line WHERE order_id=$OID")"

cat > /tmp/vot_w.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"sale.order.line","method":"write",
 "args":[[$LID],{"price_unit":10.00,"product_uom_qty":3}],
 "kwargs":{"context":{"session_id":"$SID"}}}}
EOF
curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
     --data @/tmp/vot_w.json > /dev/null

HDR=$(pg "SELECT amount_total FROM sale_order WHERE id=$OID")
SUM=$(pg "SELECT COALESCE(SUM(price_total),0) FROM sale_order_line WHERE order_id=$OID")
echo "    header after:  $HDR"
echo "    lines  after:  $SUM"

[ "$HDR" = "$SUM" ] && ok "header equals the sum of its lines after a line write" \
                    || no "header $HDR != lines $SUM — updateOrderTotals_ did not run"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

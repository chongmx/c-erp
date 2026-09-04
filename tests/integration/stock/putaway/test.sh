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
# Putaway rules (stock.putaway.rule).
#
# Proves through the real HTTP path: validating a receipt routes each
# product to its designated sub-location —
#   * a product-specific rule wins
#   * a category rule catches products with no product rule
#   * a product rule beats a category rule for the same product
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/pa_cookie.txt
cat > /tmp/pa_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/pa_auth.json > /tmp/pa_auth_out.json
grep -q '"session_id"' /tmp/pa_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
M=1000000

# Sub-locations under WH/Stock (4).
SA=$(pg "INSERT INTO stock_location (name,complete_name,location_id,usage,company_id) VALUES ('Shelf A','WH/Stock/Shelf A',4,'internal',1) RETURNING id")
SB=$(pg "INSERT INTO stock_location (name,complete_name,location_id,usage,company_id) VALUES ('Shelf B','WH/Stock/Shelf B',4,'internal',1) RETURNING id")
SC=$(pg "INSERT INTO stock_location (name,complete_name,location_id,usage,company_id) VALUES ('Shelf C','WH/Stock/Shelf C',4,'internal',1) RETURNING id")
CAT=$(pg "INSERT INTO product_category (name) VALUES ('PUTAWAY-CAT') RETURNING id")
mkprod() { pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active,qty_available,categ_id) VALUES ('$1','product',1,1,true,0,$CAT) RETURNING id"; }
PP=$(mkprod 'PATEST-PP'); PC=$(mkprod 'PATEST-PC'); PP2=$(mkprod 'PATEST-PP2')
echo "    Shelf A=$SA B=$SB C=$SC  CAT=$CAT  PP=$PP PC=$PC PP2=$PP2"

# Rules: product PP -> A ; category CAT -> B ; product PP2 -> C
pg "INSERT INTO stock_putaway_rule (product_id,location_in_id,location_out_id,sequence) VALUES ($PP,4,$SA,10)" >/dev/null
pg "INSERT INTO stock_putaway_rule (category_id,location_in_id,location_out_id,sequence) VALUES ($CAT,4,$SB,10)" >/dev/null
pg "INSERT INTO stock_putaway_rule (product_id,location_in_id,location_out_id,sequence) VALUES ($PP2,4,$SC,10)" >/dev/null

recv() { # product qty
    local pk=$(pg "INSERT INTO stock_picking (name,picking_type_id,state,location_id,location_dest_id,company_id,origin) VALUES ('PA-IN',1,'draft',5,4,1,'PATEST') RETURNING id")
    pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id) VALUES ($pk,$1,'r',$(($2*M)),0,'draft',5,4,1)" >/dev/null
    callkw stock.picking action_confirm "[[$pk]]" >/dev/null
    callkw stock.picking button_validate "[[$pk]]" >/dev/null
}

echo "############ 1. product rule routes to its shelf ############"
recv "$PP" 10
QA=$(pg "SELECT COALESCE((SELECT quantity FROM stock_quant WHERE product_id=$PP AND location_id=$SA),0)")
Q4=$(pg "SELECT COALESCE((SELECT quantity FROM stock_quant WHERE product_id=$PP AND location_id=4),0)")
echo "    PP @ Shelf A=$QA  @ WH/Stock=$Q4"
[ "$QA" = "10000000" ] && ok "PP routed to Shelf A (product rule)" || no "PP @ ShelfA=$QA"
[ "$Q4" = "0" ] && ok "nothing left at WH/Stock" || no "PP @ Stock=$Q4"

echo
echo "############ 2. category rule catches PC ############"
recv "$PC" 7
QB=$(pg "SELECT COALESCE((SELECT quantity FROM stock_quant WHERE product_id=$PC AND location_id=$SB),0)")
echo "    PC @ Shelf B=$QB"
[ "$QB" = "7000000" ] && ok "PC routed to Shelf B (category rule)" || no "PC @ ShelfB=$QB"

echo
echo "############ 3. product rule beats category rule ############"
recv "$PP2" 3
QC=$(pg "SELECT COALESCE((SELECT quantity FROM stock_quant WHERE product_id=$PP2 AND location_id=$SC),0)")
QB2=$(pg "SELECT COALESCE((SELECT quantity FROM stock_quant WHERE product_id=$PP2 AND location_id=$SB),0)")
echo "    PP2 @ Shelf C=$QC  @ Shelf B=$QB2"
[ "$QC" = "3000000" ] && ok "PP2 routed to Shelf C (product beats category)" || no "PP2 @ ShelfC=$QC"
[ "$QB2" = "0" ] && ok "category rule did not apply to PP2" || no "PP2 @ ShelfB=$QB2"

echo
echo "############ cleanup ############"
pg "DELETE FROM stock_move WHERE origin='PATEST' OR product_id IN ($PP,$PC,$PP2)" >/dev/null
pg "DELETE FROM stock_picking WHERE origin='PATEST'" >/dev/null
pg "DELETE FROM stock_putaway_rule WHERE location_out_id IN ($SA,$SB,$SC)" >/dev/null
pg "DELETE FROM stock_quant WHERE product_id IN ($PP,$PC,$PP2)" >/dev/null
pg "DELETE FROM stock_valuation_layer WHERE product_id IN ($PP,$PC,$PP2)" >/dev/null
pg "DELETE FROM product_product WHERE id IN ($PP,$PC,$PP2)" >/dev/null
pg "DELETE FROM product_category WHERE id=$CAT" >/dev/null
pg "DELETE FROM stock_location WHERE id IN ($SA,$SB,$SC)" >/dev/null
rm -f "$CK" /tmp/pa_auth.json /tmp/pa_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

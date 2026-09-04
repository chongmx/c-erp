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
# Product form inventory actions (docs/090).
#
# The Product form's stat row used to be inert placeholders labelled
# "coming soon" while stock.quant, stock.warehouse.orderpoint and
# stock.putaway.rule had all been built. This drives the endpoints those
# widgets now call.
#
# The load-bearing assertion is that the figures come from the REAL ledger:
# an inventory adjustment must move on-hand by exactly the counted difference
# and leave a stock move behind, and the forecast must be on-hand plus pending
# incoming minus pending outgoing — not a stored number that can drift.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }
CTX="\"context\":{\"session_id\":\"$SID\"}"
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":$4}}"; }
rid(){ sed -n 's/.*"result":\([0-9]*\).*/\1/p'; }
num(){ python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('result',{}).get('$1','ERR'))" 2>/dev/null; }

echo "############ fixture ############"
pg "DELETE FROM stock_warehouse_orderpoint WHERE product_id IN (SELECT id FROM product_product WHERE default_code='QA-INV-1')" >/dev/null
pg "DELETE FROM stock_putaway_rule         WHERE product_id IN (SELECT id FROM product_product WHERE default_code='QA-INV-1')" >/dev/null
PID=$(pg "SELECT id FROM product_product WHERE default_code='QA-INV-1' LIMIT 1")
if [ -z "$PID" ]; then
    PID=$(call product.product create "[{\"name\":\"QA Inventory Widget\",\"default_code\":\"QA-INV-1\",\"type\":\"storable\",\"list_price\":10,\"standard_price\":6}]" "{$CTX}" | rid)
fi
[ -n "$PID" ] && ok "test product ready ($PID)" || { no "product create failed"; echo "*** FAILURES ***"; exit 1; }
# Stock location: any internal one.
LOC=$(pg "SELECT id FROM stock_location WHERE usage='internal' ORDER BY id LIMIT 1")
[ -n "$LOC" ] && ok "internal location found ($LOC)" || { no "no internal location"; echo "*** FAILURES ***"; exit 1; }
# Start from a known on-hand so the run is repeatable.
call stock.quant set_on_hand "[{\"product_id\":$PID,\"location_id\":$LOC,\"quantity\":0}]" "{$CTX}" >/dev/null

echo "############ product_summary reads the ledger ############"
SUM=$(call stock.quant product_summary "[$PID]" "{$CTX}")
echo "$SUM" | grep -q '"qty_available"'     && ok "summary returns qty_available"     || no "qty_available missing"
echo "$SUM" | grep -q '"virtual_available"' && ok "summary returns virtual_available" || no "virtual_available missing"
echo "$SUM" | grep -q '"orderpoint_count"'  && ok "summary returns orderpoint_count"  || no "orderpoint_count missing"
echo "$SUM" | grep -q '"putaway_count"'     && ok "summary returns putaway_count"     || no "putaway_count missing"
Q0=$(echo "$SUM" | num qty_available)
[ "$Q0" = "0.0" ] && ok "on hand starts at 0" || no "on hand should start at 0, got $Q0"

echo "############ Update Quantity moves the real ledger ############"
# An adjustment is booked against the Inventory Adjustments location, which is
# virtual, so it crosses the owned-stock boundary and must leave a valuation
# layer. (It leaves no stock_move: in this schema a move belongs to a picking,
# and an adjustment has none — the valuation layer is its audit trail.)
VL0=$(pg "SELECT count(*) FROM stock_valuation_layer WHERE product_id=$PID")
call stock.quant set_on_hand "[{\"product_id\":$PID,\"location_id\":$LOC,\"quantity\":25}]" "{$CTX}" >/dev/null
Q1=$(call stock.quant product_summary "[$PID]" "{$CTX}" | num qty_available)
[ "$Q1" = "25.0" ] && ok "counting 25 sets on hand to 25" || no "on hand = $Q1, expected 25.0"
LEDGER=$(pg "SELECT COALESCE(SUM(q.quantity),0) FROM stock_quant q JOIN stock_location sl ON sl.id=q.location_id WHERE q.product_id=$PID AND sl.usage='internal'")
[ "$LEDGER" = "25000000" ] && ok "the quant ledger agrees (25.000000)" || no "ledger = $LEDGER, expected 25000000"
VL1=$(pg "SELECT count(*) FROM stock_valuation_layer WHERE product_id=$PID")
[ "${VL1:-0}" -gt "${VL0:-0}" ] && ok "the adjustment left a valuation layer behind" || no "no valuation layer recorded ($VL0 -> $VL1)"
[ "$(pg "SELECT quantity FROM stock_valuation_layer WHERE product_id=$PID ORDER BY id DESC LIMIT 1")" = "25000000" ] \
    && ok "the layer records the +25 that was counted in" || no "the valuation layer does not match the adjustment"
# Counting down must work too.
call stock.quant set_on_hand "[{\"product_id\":$PID,\"location_id\":$LOC,\"quantity\":18}]" "{$CTX}" >/dev/null
Q2=$(call stock.quant product_summary "[$PID]" "{$CTX}" | num qty_available)
[ "$Q2" = "18.0" ] && ok "counting down to 18 works" || no "on hand = $Q2, expected 18.0"
[ "$(pg "SELECT quantity FROM stock_valuation_layer WHERE product_id=$PID ORDER BY id DESC LIMIT 1")" = "-7000000" ] \
    && ok "counting down books the -7 difference, not the new total" || no "the count-down layer is wrong"

echo "############ forecast = on hand + incoming - outgoing ############"
# A pending outgoing move must lower the forecast without touching on-hand.
CUST=$(pg "SELECT id FROM stock_location WHERE usage='customer' ORDER BY id LIMIT 1")
PT=$(pg "SELECT id FROM stock_picking_type ORDER BY id LIMIT 1")
PICK=$(pg "INSERT INTO stock_picking (name,picking_type_id,location_id,location_dest_id,state,company_id) VALUES ('QA-INV-OUT',$PT,$LOC,$CUST,'assigned',1) RETURNING id" | head -1)
pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,state,location_id,location_dest_id,company_id) VALUES ($PICK,$PID,'QA outgoing',5,'assigned',$LOC,$CUST,1)" >/dev/null
SUM2=$(call stock.quant product_summary "[$PID]" "{$CTX}")
[ "$(echo "$SUM2" | num outgoing_qty)"      = "5.0"  ] && ok "the pending delivery counts as outgoing" || no "outgoing = $(echo "$SUM2" | num outgoing_qty), expected 5.0"
[ "$(echo "$SUM2" | num qty_available)"     = "18.0" ] && ok "on hand is unchanged by a pending move"  || no "on hand moved on a pending delivery"
[ "$(echo "$SUM2" | num virtual_available)" = "13.0" ] && ok "forecast drops to 13 (18 - 5)"           || no "forecast = $(echo "$SUM2" | num virtual_available), expected 13.0"
# A done move must NOT be double-counted: it is already in the quants.
pg "UPDATE stock_move SET state='done' WHERE picking_id=$PICK" >/dev/null
[ "$(call stock.quant product_summary "[$PID]" "{$CTX}" | num outgoing_qty)" = "0.0" ] \
    && ok "a completed move stops counting as outgoing" || no "a done move is still counted as outgoing"

echo "############ reordering rules + Replenish ############"
VEND=$(pg "SELECT id FROM res_partner WHERE supplier_rank > 0 ORDER BY id LIMIT 1")
[ -z "$VEND" ] && VEND=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
OP=$(call stock.warehouse.orderpoint create "[{\"product_id\":$PID,\"location_id\":$LOC,\"product_min_qty\":50,\"product_max_qty\":80,\"qty_multiple\":1,\"route\":\"buy\",\"supplier_id\":$VEND}]" "{$CTX}" | rid)
[ -n "$OP" ] && ok "reordering rule created ($OP)" || no "orderpoint create failed"
[ "$(call stock.quant product_summary "[$PID]" "{$CTX}" | num orderpoint_count)" = "1" ] \
    && ok "the form's Reordering Rules counter sees it" || no "orderpoint_count did not update"
PO0=$(pg "SELECT count(*) FROM purchase_order_line WHERE product_id=$PID")
call stock.warehouse.orderpoint run_scheduler "[{\"product_id\":$PID}]" "{$CTX}" >/dev/null
PO1=$(pg "SELECT count(*) FROM purchase_order_line WHERE product_id=$PID")
[ "${PO1:-0}" -gt "${PO0:-0}" ] && ok "Replenish drafts a purchase for the shortfall" || no "no replenishment created ($PO0 -> $PO1)"
# 80 max - 18 on hand = 62 to order.
[ "$(pg "SELECT product_qty FROM purchase_order_line WHERE product_id=$PID ORDER BY id DESC LIMIT 1")" = "62000000" ] \
    && ok "it orders up to the maximum (62 = 80 - 18)" || no "wrong replenishment quantity"
# Running again changes nothing: the open purchase counts toward the forecast,
# so the rule is satisfied and must not order a second time.
call stock.warehouse.orderpoint run_scheduler "[{\"product_id\":$PID}]" "{$CTX}" >/dev/null
[ "$(pg "SELECT count(*) FROM purchase_order_line WHERE product_id=$PID")" = "$PO1" ] \
    && ok "an open purchase order suppresses a second replenishment" || no "Replenish double-ordered"
# A rule with no vendor is reported, not silently skipped — otherwise the
# Replenish button says "nothing to do" when the truth is "nobody to buy from".
# The minimum is raised past what is already on order so the rule really fires.
pg "UPDATE stock_warehouse_orderpoint SET supplier_id=NULL, product_min_qty=500000000, product_max_qty=600000000 WHERE id=$OP" >/dev/null
pg "DELETE FROM product_supplierinfo WHERE product_id=$PID" >/dev/null
NV=$(call stock.warehouse.orderpoint run_scheduler "[{\"product_id\":$PID}]" "{$CTX}")
echo "$NV" | grep -q '"skipped_no_vendor":1' && ok "a vendorless rule is reported back to the user" \
                                             || no "vendorless rule was skipped silently: $(echo "$NV" | head -c 160)"
# Scoped run: another product's rule must not fire.
OTHER=$(pg "SELECT id FROM product_product WHERE id<>$PID ORDER BY id LIMIT 1")
OP2=$(call stock.warehouse.orderpoint create "[{\"product_id\":$OTHER,\"location_id\":$LOC,\"product_min_qty\":9999,\"product_max_qty\":9999,\"qty_multiple\":1,\"route\":\"buy\"}]" "{$CTX}" | rid)
OTH0=$(pg "SELECT count(*) FROM purchase_order_line WHERE product_id=$OTHER")
call stock.warehouse.orderpoint run_scheduler "[{\"product_id\":$PID}]" "{$CTX}" >/dev/null
OTH1=$(pg "SELECT count(*) FROM purchase_order_line WHERE product_id=$OTHER")
[ "$OTH0" = "$OTH1" ] && ok "Replenish is scoped to its own product" || no "a scoped run fired another product's rule"

echo "############ putaway rules ############"
DEST=$(pg "SELECT id FROM stock_location WHERE usage='internal' AND id<>$LOC ORDER BY id LIMIT 1")
[ -z "$DEST" ] && DEST=$LOC
PW=$(call stock.putaway.rule create "[{\"product_id\":$PID,\"location_in_id\":$LOC,\"location_out_id\":$DEST}]" "{$CTX}" | rid)
[ -n "$PW" ] && ok "putaway rule created ($PW)" || no "putaway create failed"
[ "$(call stock.quant product_summary "[$PID]" "{$CTX}" | num putaway_count)" = "1" ] \
    && ok "the form's Putaway Rules counter sees it" || no "putaway_count did not update"

echo "############ housekeeping ############"
pg "DELETE FROM stock_putaway_rule WHERE product_id=$PID" >/dev/null
pg "DELETE FROM stock_warehouse_orderpoint WHERE product_id IN ($PID,$OTHER)" >/dev/null
# Drop the drafted replenishment orders whole, not just their lines, so a rerun
# does not find an open purchase suppressing the replenishment assertions.
pg "DELETE FROM purchase_order_line WHERE order_id IN (SELECT id FROM purchase_order WHERE origin LIKE 'Reordering: OP/%')" >/dev/null
pg "DELETE FROM purchase_order WHERE origin LIKE 'Reordering: OP/%'" >/dev/null
pg "DELETE FROM stock_move WHERE picking_id=$PICK" >/dev/null
pg "DELETE FROM stock_picking WHERE id=$PICK" >/dev/null
call stock.quant set_on_hand "[{\"product_id\":$PID,\"location_id\":$LOC,\"quantity\":0}]" "{$CTX}" >/dev/null
# The test product goes too. It used to be left behind and reused on the next
# run — stable, but it still sat in the user's product list forever (docs/092).
# Stock rows first: quant and valuation layer both reference the product.
pg "DELETE FROM stock_quant           WHERE product_id=$PID" >/dev/null
pg "DELETE FROM stock_valuation_layer WHERE product_id=$PID" >/dev/null
pg "DELETE FROM stock_move            WHERE product_id=$PID" >/dev/null
pg "DELETE FROM product_product       WHERE id=$PID" >/dev/null
[ "$(pg "SELECT count(*) FROM product_product WHERE default_code='QA-INV-1'")" = "0" ] \
    && ok "fixtures cleaned up" || no "the test product was left behind"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

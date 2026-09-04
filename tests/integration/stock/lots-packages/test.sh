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
# Lot/serial tracking and packages on transfers (docs/090).
#
# Lot tracking was already enforced in the engine but had no UI; packages are
# new. The two halves have different guarantees and this script asserts both:
#
#   * LOTS are a ledger fact — a tracked receipt must land the stock on the
#     right lot's quant, a tracked move without a lot must be REFUSED, and a
#     serial move must be exactly one unit.
#   * PACKAGES are a logistics label, deliberately NOT a quant dimension. So
#     the assertion is that packing groups the operations and follows the
#     goods to their destination — and that it does not disturb the quants.
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

echo "############ menu ############"
[ "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Packages' LIMIT 1")" = "stock.quant.package" ] \
    && ok "Inventory -> Products -> Packages wired" || no "Packages menu missing"
[ "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Lots/Serial Numbers' LIMIT 1")" = "stock.production.lot" ] \
    && ok "Lots/Serial Numbers menu still wired" || no "Lots menu missing"

echo "############ fixture ############"
pg "DELETE FROM stock_move WHERE name LIKE 'QA-LOT%'" >/dev/null
pg "DELETE FROM stock_picking WHERE name LIKE 'QA-LOTPICK%'" >/dev/null
PID=$(pg "SELECT id FROM product_product WHERE default_code='QA-LOT-1' LIMIT 1")
if [ -z "$PID" ]; then
    PID=$(call product.product create "[{\"name\":\"QA Tracked Widget\",\"default_code\":\"QA-LOT-1\",\"type\":\"storable\",\"standard_price\":4}]" "{$CTX}" | rid)
fi
[ -n "$PID" ] && ok "tracked product ready ($PID)" || { no "product create failed"; echo "*** FAILURES ***"; exit 1; }
pg "UPDATE product_product SET tracking='lot' WHERE id=$PID" >/dev/null
[ "$(pg "SELECT tracking FROM product_product WHERE id=$PID")" = "lot" ] && ok "product is lot-tracked" || no "tracking not set"
SRC=$(pg "SELECT id FROM stock_location WHERE usage='supplier' ORDER BY id LIMIT 1")
DST=$(pg "SELECT id FROM stock_location WHERE usage='internal' ORDER BY id LIMIT 1")
PT=$(pg "SELECT id FROM stock_picking_type WHERE code='incoming' ORDER BY id LIMIT 1")

echo "############ a tracked move without a lot is refused ############"
PK=$(call stock.picking create "[{\"name\":\"QA-LOTPICK-1\",\"picking_type_id\":$PT,\"location_id\":$SRC,\"location_dest_id\":$DST}]" "{$CTX}" | rid)
[ -n "$PK" ] && ok "transfer created ($PK)" || { no "picking create failed"; echo "*** FAILURES ***"; exit 1; }
MV=$(call stock.move create "[{\"picking_id\":$PK,\"product_id\":$PID,\"name\":\"QA-LOT receipt\",\"product_uom_qty\":10,\"location_id\":$SRC,\"location_dest_id\":$DST}]" "{$CTX}" | rid)
[ -n "$MV" ] && ok "operation added ($MV)" || no "move create failed"
call stock.picking action_confirm "[[$PK]]" "{$CTX}" >/dev/null
R=$(call stock.picking button_validate "[[$PK]]" "{$CTX}")
echo "$R" | grep -qi 'lot/serial number is required' && ok "validating without a lot is refused" \
    || no "a tracked move validated with no lot: $(echo "$R" | head -c 140)"
[ "$(pg "SELECT state FROM stock_picking WHERE id=$PK")" != "done" ] && ok "the transfer stayed open" || no "the transfer completed anyway"

echo "############ the move accepts a lot over RPC ############"
LOT=$(call stock.production.lot create "[{\"name\":\"QA-LOT-A\",\"product_id\":$PID}]" "{$CTX}" | rid)
[ -n "$LOT" ] && ok "lot created ($LOT)" || no "lot create failed"
call stock.move write "[[$MV],{\"lot_id\":$LOT}]" "{$CTX}" >/dev/null
[ "$(pg "SELECT lot_id FROM stock_move WHERE id=$MV")" = "$LOT" ] && ok "the lot is stored on the operation" || no "lot_id was not written"
# and it comes back on the read the transfer form uses
S=$(call stock.move search_read "[[[\"picking_id\",\"=\",$PK]]]" "{$CTX}")
echo "$S" | grep -q '"tracking":"lot"' && ok "search_read reports the product's tracking mode" || no "tracking missing from search_read"
echo "$S" | grep -q "\"lot_id\":\[$LOT" && ok "search_read returns the lot with its name" || no "lot_id missing from search_read"

echo "############ packing ############"
P=$(call stock.quant.package put_in_pack "[{\"picking_id\":$PK}]" "{$CTX}")
PKG=$(echo "$P" | sed -n 's/.*"id":\([0-9]*\).*/\1/p')
[ -n "$PKG" ] && ok "put_in_pack created a package ($PKG)" || { no "put_in_pack failed: $(echo "$P" | head -c 140)"; }
echo "$P" | grep -q '"name":"PACK' && ok "the package is numbered from a sequence (PACK…)" || no "package name is not sequenced"
[ "$(pg "SELECT result_package_id FROM stock_move WHERE id=$MV")" = "$PKG" ] && ok "the operation is in the package" || no "the move was not packed"
# Packing twice must not silently create an empty second parcel.
R=$(call stock.quant.package put_in_pack "[{\"picking_id\":$PK}]" "{$CTX}")
echo "$R" | grep -qi 'nothing left to pack' && ok "re-packing an already-packed transfer is refused" \
    || no "a second, empty package was created"
C=$(call stock.quant.package contents "[$PKG]" "{$CTX}")
echo "$C" | grep -q "\"move_id\":$MV" && ok "the package lists its contents" || no "contents did not list the operation"

echo "############ validate: stock lands on the lot's quant ############"
BEFORE=$(pg "SELECT COALESCE(quantity,0) FROM stock_quant WHERE product_id=$PID AND location_id=$DST AND lot_id=$LOT")
call stock.picking button_validate "[[$PK]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT state FROM stock_picking WHERE id=$PK")" = "done" ] && ok "the transfer validates once the lot is set" || no "validate still failed"
AFTER=$(pg "SELECT COALESCE(quantity,0) FROM stock_quant WHERE product_id=$PID AND location_id=$DST AND lot_id=$LOT")
[ "$(( ${AFTER:-0} - ${BEFORE:-0} ))" = "10000000" ] && ok "10 units landed on lot QA-LOT-A's quant" \
    || no "lot quant moved by $(( ${AFTER:-0} - ${BEFORE:-0} )), expected 10000000"
# The stock must NOT have gone to the untracked (lot_id 0) bucket. SUM, not a
# bare column: with no such quant row at all — the ideal outcome — a plain
# SELECT returns no row and the comparison would fail against its own success.
[ "$(pg "SELECT COALESCE(SUM(quantity),0) FROM stock_quant WHERE product_id=$PID AND location_id=$DST AND lot_id=0")" = "0" ] \
    && ok "nothing leaked into the untracked bucket" || no "some stock landed with no lot"

echo "############ the package followed the goods ############"
[ "$(pg "SELECT location_id FROM stock_quant_package WHERE id=$PKG")" = "$DST" ] \
    && ok "the package is now at the destination location" || no "the package location did not follow"
[ "$(pg "SELECT count(*) FROM stock_quant_package WHERE id=$PKG")" = "1" ] \
    && ok "the package survives validation" || no "the package disappeared"
R=$(call stock.quant.package unpack "[[$PKG]]" "{$CTX}")
echo "$R" | grep -qi 'already shipped' && ok "a shipped package cannot be unpacked" || no "a shipped package was unpacked"

echo "############ serial tracking is one unit per move ############"
pg "UPDATE product_product SET tracking='serial' WHERE id=$PID" >/dev/null
PK2=$(call stock.picking create "[{\"name\":\"QA-LOTPICK-2\",\"picking_type_id\":$PT,\"location_id\":$SRC,\"location_dest_id\":$DST}]" "{$CTX}" | rid)
SER=$(call stock.production.lot create "[{\"name\":\"QA-SERIAL-1\",\"product_id\":$PID}]" "{$CTX}" | rid)
MV2=$(call stock.move create "[{\"picking_id\":$PK2,\"product_id\":$PID,\"name\":\"QA-LOT serial receipt\",\"product_uom_qty\":3,\"location_id\":$SRC,\"location_dest_id\":$DST,\"lot_id\":$SER}]" "{$CTX}" | rid)
call stock.picking action_confirm "[[$PK2]]" "{$CTX}" >/dev/null
R=$(call stock.picking button_validate "[[$PK2]]" "{$CTX}")
echo "$R" | grep -qi 'exactly one unit' && ok "a 3-unit serial move is refused" || no "a serial move of 3 was accepted"
call stock.move write "[[$MV2],{\"product_uom_qty\":1}]" "{$CTX}" >/dev/null
call stock.picking button_validate "[[$PK2]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT state FROM stock_picking WHERE id=$PK2")" = "done" ] && ok "a 1-unit serial move validates" || no "the serial receipt did not validate"

echo "############ housekeeping ############"
pg "UPDATE product_product SET tracking='none' WHERE id=$PID" >/dev/null
pg "UPDATE stock_move SET result_package_id=NULL WHERE picking_id IN ($PK,$PK2)" >/dev/null
pg "DELETE FROM stock_quant_package WHERE picking_id IN ($PK,$PK2)" >/dev/null
pg "DELETE FROM stock_move WHERE picking_id IN ($PK,$PK2)" >/dev/null
pg "DELETE FROM stock_picking WHERE id IN ($PK,$PK2)" >/dev/null
pg "DELETE FROM stock_quant WHERE product_id=$PID" >/dev/null
pg "DELETE FROM stock_production_lot WHERE product_id=$PID" >/dev/null
# And the product itself — it was previously left behind and reused, which kept
# a QA row permanently in the user's product list (docs/092).
pg "DELETE FROM stock_valuation_layer WHERE product_id=$PID" >/dev/null
pg "DELETE FROM stock_move            WHERE product_id=$PID" >/dev/null
pg "DELETE FROM product_product       WHERE id=$PID" >/dev/null
[ "$(pg "SELECT count(*) FROM product_product WHERE default_code='QA-LOT-1'")" = "0" ] \
    && ok "fixtures cleaned up" || no "the test product was left behind"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

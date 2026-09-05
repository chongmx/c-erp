#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# FUNCTIONAL JOURNEY 03 — MAKE.  (docs/109 §3)
#
#   BOM -> manufacturing order -> confirm -> reserve -> produce -> stock moved
#
# The journey a workshop actually runs, and the one where the arithmetic is
# easiest to get wrong: making 5 of something that takes 2 resistors and 3
# capacitors each must consume exactly 10 and 15, not 2 and 3, and not 10 and
# 15 of the wrong component.
#
# The invariant at the end is conservation: every unit that left the component
# stock is accounted for by a unit of finished product, at the BOM's ratio.
# Nothing may quietly evaporate between the two. A per-module test checks that
# an MO reaches state 'done'; only the arithmetic across the whole run tells
# you whether the warehouse still adds up.
#
# Everything is prefixed MK- / 'MK ' and removed on the way out.
# =============================================================
auth_or_die

M=1000000
BUILD=5              # finished units to make
PER_R=2              # resistors per unit
PER_C=3              # capacitors per unit
STOCK=100            # starting on-hand of each component

cleanup() {
    pg "DELETE FROM stock_move_line WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'MK-%')" >/dev/null
    pg "DELETE FROM stock_move      WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'MK-%')" >/dev/null
    pg "DELETE FROM stock_quant     WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'MK-%')" >/dev/null
    pg "DELETE FROM mrp_production  WHERE name LIKE 'MK%' OR product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'MK-%')" >/dev/null
    pg "DELETE FROM mrp_bom_line    WHERE bom_id IN (SELECT id FROM mrp_bom WHERE code LIKE 'MK-%')" >/dev/null
    pg "DELETE FROM mrp_bom         WHERE code LIKE 'MK-%'" >/dev/null
    pg "DELETE FROM product_product WHERE default_code LIKE 'MK-%'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

mkproduct() {  # mkproduct <code> <name> <cost-majors>
    local uom cat
    uom=$(pg "SELECT id FROM uom_uom ORDER BY id LIMIT 1")
    cat=$(pg "SELECT id FROM product_category ORDER BY id LIMIT 1")
    pgid "INSERT INTO product_product
        (name, default_code, type, categ_id, uom_id, uom_po_id, list_price, standard_price,
         qty_available, active, sale_ok, purchase_ok, company_id)
        VALUES ('$2','$1','product',$cat,$uom,$uom,0,$(( $3 * M )),0,true,true,true,1)
        RETURNING id"
}

onhand() { local q; q=$(pg "SELECT COALESCE(quantity,0) FROM stock_quant WHERE product_id=$1 AND location_id=4"); echo "${q:-0}"; }

# ------------------------------------------------------------------
sec "0. the parts and the thing they make"
# ------------------------------------------------------------------
FINISHED=$(mkproduct "MK-BOARD-1" "MK Assembled Board" 50)
COMP_R=$(mkproduct   "MK-RES-1"   "MK Resistor"         1)
COMP_C=$(mkproduct   "MK-CAP-1"   "MK Capacitor"        2)
t_nonempty "$FINISHED" "the finished product exists"
t_nonempty "$COMP_R"   "the resistor exists"
t_nonempty "$COMP_C"   "the capacitor exists"
[ -z "$FINISHED" ] || [ -z "$COMP_R" ] || [ -z "$COMP_C" ] && { verdict; exit 1; }

call stock.quant set_on_hand "[{\"product_id\":$COMP_R,\"location_id\":4,\"quantity\":$STOCK}]" >/dev/null
call stock.quant set_on_hand "[{\"product_id\":$COMP_C,\"location_id\":4,\"quantity\":$STOCK}]" >/dev/null
R0=$(onhand "$COMP_R"); C0=$(onhand "$COMP_C"); F0=$(onhand "$FINISHED")
t_eq "$((STOCK * M))" "${R0%%.*}" "resistors are in stock"
t_eq "$((STOCK * M))" "${C0%%.*}" "capacitors are in stock"

# ------------------------------------------------------------------
sec "1. a bill of materials"
# ------------------------------------------------------------------
UOM=$(pg "SELECT id FROM uom_uom ORDER BY id LIMIT 1")
BOM=$(pgid "INSERT INTO mrp_bom (product_id, code, bom_type, product_qty, product_uom_id, company_id, active)
            VALUES ($FINISHED,'MK-BOM-1','normal',$M,$UOM,1,true) RETURNING id")
t_nonempty "$BOM" "a BOM was created"
pg "INSERT INTO mrp_bom_line (bom_id, product_id, product_qty, product_uom_id, sequence)
    VALUES ($BOM,$COMP_R,$((PER_R * M)),$UOM,10)" >/dev/null
pg "INSERT INTO mrp_bom_line (bom_id, product_id, product_qty, product_uom_id, sequence)
    VALUES ($BOM,$COMP_C,$((PER_C * M)),$UOM,20)" >/dev/null
t_eq "2" "$(pg "SELECT count(*) FROM mrp_bom_line WHERE bom_id=$BOM")" "it has two component lines"
# 'normal', not 'phantom'. A kit is a phantom BOM — a collection put in a pack,
# never manufactured. This one is a PCBA and must produce a manufacturing order.
t_eq "normal" "$(pg "SELECT bom_type FROM mrp_bom WHERE id=$BOM")" "it is a manufacturing BOM, not a kit"

# ------------------------------------------------------------------
sec "2. a manufacturing order"
# ------------------------------------------------------------------
MO=$(call mrp.production create \
    "[{\"product_id\":$FINISHED,\"product_qty\":$BUILD,\"bom_id\":$BOM,\"location_src_id\":4,\"location_dest_id\":4}]" | rid)
t_nonempty "$MO" "a manufacturing order was created"
[ -z "$MO" ] && { verdict; exit 1; }

CRES=$(call mrp.production action_confirm "[[$MO]]")
has_error "$CRES" && no "confirming the MO failed: $(echo "$CRES" | head -c 200)"
STATE=$(pg "SELECT state FROM mrp_production WHERE id=$MO")
NAME=$(pg "SELECT name FROM mrp_production WHERE id=$MO")
echo "    $NAME is '$STATE'"
t_ne "draft" "$STATE" "confirming moved it out of draft"

# Exploding the BOM is the step that decides what the workshop is told to pick.
# Off-by-one here means the wrong parts leave the shelf.
NEED_R=$(pg "SELECT COALESCE(SUM(product_uom_qty),0) FROM stock_move WHERE production_id=$MO AND is_production_raw AND product_id=$COMP_R")
NEED_C=$(pg "SELECT COALESCE(SUM(product_uom_qty),0) FROM stock_move WHERE production_id=$MO AND is_production_raw AND product_id=$COMP_C")
echo "    exploded demand: resistors=$NEED_R capacitors=$NEED_C (micro-units)"
t_eq "$((BUILD * PER_R * M))" "${NEED_R%%.*}" "it needs $((BUILD * PER_R)) resistors for $BUILD units"
t_eq "$((BUILD * PER_C * M))" "${NEED_C%%.*}" "it needs $((BUILD * PER_C)) capacitors for $BUILD units"

# ------------------------------------------------------------------
sec "3. producing"
# ------------------------------------------------------------------
DRES=$(call mrp.production button_mark_done "[[$MO]]")
has_error "$DRES" && no "marking the MO done failed: $(echo "$DRES" | head -c 200)"
t_eq "done" "$(pg "SELECT state FROM mrp_production WHERE id=$MO")" "the order is finished"

R1=$(onhand "$COMP_R"); C1=$(onhand "$COMP_C"); F1=$(onhand "$FINISHED")
echo "    resistors  $R0 -> $R1"
echo "    capacitors $C0 -> $C1"
echo "    finished   $F0 -> $F1"

# ------------------------------------------------------------------
sec "4. conservation — the whole point"
# ------------------------------------------------------------------
USED_R=$(pg "SELECT (${R0%%.*} - ${R1%%.*})")
USED_C=$(pg "SELECT (${C0%%.*} - ${C1%%.*})")
MADE=$(pg   "SELECT (${F1%%.*} - ${F0%%.*})")

t_eq "$((BUILD * M))"          "${MADE%%.*}"   "$BUILD finished units appeared"
t_eq "$((BUILD * PER_R * M))"  "${USED_R%%.*}" "exactly $((BUILD * PER_R)) resistors were consumed"
t_eq "$((BUILD * PER_C * M))"  "${USED_C%%.*}" "exactly $((BUILD * PER_C)) capacitors were consumed"

# And the ratio holds, stated independently of the numbers above so that a
# consistent-but-wrong pair cannot pass both.
if [ "${MADE%%.*}" -gt 0 ] 2>/dev/null; then
    t_eq "$PER_R" "$(( ${USED_R%%.*} / ${MADE%%.*} ))" "the resistor ratio matches the BOM"
    t_eq "$PER_C" "$(( ${USED_C%%.*} / ${MADE%%.*} ))" "the capacitor ratio matches the BOM"
fi

# Nothing may go negative: consuming more than was on the shelf means the
# reservation did not hold.
t_ge "${R1%%.*}" 0 "resistor stock did not go negative"
t_ge "${C1%%.*}" 0 "capacitor stock did not go negative"

# ==================================================================
# THE CANCEL PATH
#
# A workshop cancels far more orders than it likes to admit: the customer
# pulls out, a component is unobtainable, the revision changes. What must be
# true afterwards is simple and easy to get wrong — the parts go back on the
# shelf.
#
# A cancel that consumes anything is the expensive failure here, because the
# loss is silent. Nothing errors; the stock figure is just quietly wrong, and
# it stays wrong until someone counts the shelf by hand.
# ==================================================================
sec "5. an order that gets cancelled before it is made"
R_BEFORE=$(onhand "$COMP_R"); C_BEFORE=$(onhand "$COMP_C"); F_BEFORE=$(onhand "$FINISHED")

MO2=$(call mrp.production create \
    "[{\"product_id\":$FINISHED,\"product_qty\":3,\"bom_id\":$BOM,\"location_src_id\":4,\"location_dest_id\":4}]" | rid)
t_nonempty "$MO2" "a second manufacturing order was created"
if [ -n "$MO2" ]; then
    call mrp.production action_confirm "[[$MO2]]" >/dev/null
    NEED2=$(pg "SELECT COALESCE(SUM(product_uom_qty),0) FROM stock_move
                 WHERE production_id=$MO2 AND is_production_raw AND product_id=$COMP_R")
    t_eq "$((3 * PER_R * M))" "${NEED2%%.*}" "it demands $((3 * PER_R)) resistors"

    # Reserving first is the case that actually bites: a cancel has to release
    # the reservation as well as skip the consumption, or the parts stay
    # invisible to every later order while sitting on the shelf.
    call mrp.production action_assign "[[$MO2]]" >/dev/null

    XRES=$(call mrp.production action_cancel "[[$MO2]]")
    has_error "$XRES" && no "cancelling the order failed: $(echo "$XRES" | head -c 200)"
    t_eq "cancel" "$(pg "SELECT state FROM mrp_production WHERE id=$MO2")" "the order reads as cancelled"
fi

sec "6. the parts went back on the shelf"
R_AFTER=$(onhand "$COMP_R"); C_AFTER=$(onhand "$COMP_C"); F_AFTER=$(onhand "$FINISHED")
echo "    resistors  $R_BEFORE -> $R_AFTER"
echo "    capacitors $C_BEFORE -> $C_AFTER"
echo "    finished   $F_BEFORE -> $F_AFTER"
t_eq "${R_BEFORE%%.*}" "${R_AFTER%%.*}" "no resistors were consumed by the cancelled order"
t_eq "${C_BEFORE%%.*}" "${C_AFTER%%.*}" "no capacitors were consumed by the cancelled order"
t_eq "${F_BEFORE%%.*}" "${F_AFTER%%.*}" "and nothing was produced"

# Nothing may be left reserved against an order that no longer exists as work.
RSV=$(pg "SELECT COALESCE(SUM(reserved_quantity),0) FROM stock_quant
           WHERE product_id IN ($COMP_R,$COMP_C) AND location_id=4")
t_eq "0" "${RSV%%.*}" "the cancelled order left nothing reserved"

# And its moves must not still read as pending work.
LIVE=$(pg "SELECT count(*) FROM stock_move
            WHERE production_id=$MO2 AND state NOT IN ('cancel','draft')")
t_eq "0" "${LIVE:-0}" "no component move survived the cancellation"

verdict

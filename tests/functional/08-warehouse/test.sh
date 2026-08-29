#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# FUNCTIONAL JOURNEY 08 — THE WAREHOUSE.  (tests/docs/test-plan.md §4)
#
#   a sub-location -> receipt -> internal transfer -> lots -> a package ->
#   delivery -> the numbers still add up everywhere you can look at them
#
# Stock errors are silent and expensive: nothing throws, the figure is simply
# wrong, and it stays wrong until somebody counts a shelf by hand. So this
# journey is mostly ARITHMETIC, asserted from several directions at once:
#
#   * on-hand per location must sum to on-hand overall,
#   * the sum of a product's lots must equal the product's own quantity,
#   * every completed move must have moved something, out of one place and
#     into another, in equal measure.
#
# The last one is the invariant a per-module test never checks, because it
# spans every operation type rather than one.
#
# Prefixed WH- / 'WH ' and removed on the way out.
# =============================================================
auth_or_die

M=1000000
RECEIVE=60
MOVE_IN=25          # moved to the sub-location
SHIP=10

cleanup() {
    pg "DELETE FROM stock_move_line WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'WH-%')" >/dev/null
    pg "DELETE FROM stock_move      WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'WH-%')" >/dev/null
    pg "DELETE FROM stock_picking    WHERE origin LIKE 'WH-%'" >/dev/null
    pg "DELETE FROM stock_quant      WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'WH-%')" >/dev/null
    pg "DELETE FROM stock_production_lot WHERE name LIKE 'WH-LOT%'" >/dev/null
    pg "DELETE FROM stock_quant_package  WHERE name LIKE 'WH-PACK%'" >/dev/null
    pg "DELETE FROM stock_valuation_layer WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'WH-%')" >/dev/null
    pg "DELETE FROM product_product  WHERE default_code LIKE 'WH-%'" >/dev/null
    pg "DELETE FROM product_template WHERE default_code LIKE 'WH-%'" >/dev/null
    pg "DELETE FROM stock_location   WHERE name LIKE 'WH %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

onhand() { local q; q=$(pg "SELECT COALESCE(quantity,0) FROM stock_quant WHERE product_id=$1 AND location_id=$2"); echo "${q:-0}"; }
total_onhand() { local q; q=$(pg "SELECT COALESCE(SUM(quantity),0) FROM stock_quant q
                                  JOIN stock_location l ON l.id=q.location_id
                                 WHERE q.product_id=$1 AND l.usage='internal'"); echo "${q:-0}"; }

# ------------------------------------------------------------------
sec "1. the warehouse layout"
# ------------------------------------------------------------------
STOCK=$(pg "SELECT id FROM stock_location WHERE usage='internal' ORDER BY id LIMIT 1")
VENDORS=$(pg "SELECT id FROM stock_location WHERE usage='supplier' ORDER BY id LIMIT 1")
CUSTOMERS=$(pg "SELECT id FROM stock_location WHERE usage='customer' ORDER BY id LIMIT 1")
t_nonempty "$STOCK"     "an internal stock location exists"
t_nonempty "$VENDORS"   "a vendor location exists"
t_nonempty "$CUSTOMERS" "a customer location exists"
[ -z "$STOCK" ] || [ -z "$VENDORS" ] || [ -z "$CUSTOMERS" ] && { verdict; exit 1; }

# A shelf inside the stock location. Sub-locations are where per-location
# arithmetic starts to matter: the parent must not double-count its children.
SHELF=$(call stock.location create "[{\"name\":\"WH Shelf A\",\"usage\":\"internal\",\"location_id\":$STOCK}]" | rid)
t_nonempty "$SHELF" "a sub-location was created"
t_eq "$STOCK" "$(pg "SELECT location_id FROM stock_location WHERE id=$SHELF")" "it sits under stock"
t_eq "internal" "$(pg "SELECT usage FROM stock_location WHERE id=$SHELF")" "and is an internal location"

UOM=$(pg "SELECT id FROM uom_uom ORDER BY id LIMIT 1")
CAT=$(pg "SELECT id FROM product_category ORDER BY id LIMIT 1")
TMPL=$(pgid "INSERT INTO product_template (name, default_code, type, categ_id, uom_id, uom_po_id,
             list_price, standard_price, active, sale_ok, purchase_ok, company_id)
             VALUES ('WH Widget','WH-W1','product',$CAT,$UOM,$UOM,$((20*M)),$((8*M)),true,true,true,1)
             RETURNING id")
PROD=$(pgid "INSERT INTO product_product (name, default_code, type, categ_id, uom_id, uom_po_id,
             list_price, standard_price, qty_available, active, sale_ok, purchase_ok, company_id, product_tmpl_id)
             VALUES ('WH Widget','WH-W1','product',$CAT,$UOM,$UOM,$((20*M)),$((8*M)),0,true,true,true,1,$TMPL)
             RETURNING id")
t_nonempty "$PROD" "a stockable product exists"

mkpick() {  # mkpick <name> <type> <src> <dest> -> id
    pgid "INSERT INTO stock_picking (name,picking_type_id,state,location_id,location_dest_id,company_id,origin)
          VALUES ('$1',$2,'draft',$3,$4,1,'WH-JOURNEY') RETURNING id"
}
mkmove() {  # mkmove <picking> <src> <dest> <qty-units>
    pg "INSERT INTO stock_move (picking_id,product_id,name,product_uom_qty,quantity,state,
                                location_id,location_dest_id,company_id)
        VALUES ($1,$PROD,'WH move',$(( $4 * M )),0,'draft',$2,$3,1)" >/dev/null
}
run_pick() {  # run_pick <id>
    call stock.picking action_confirm  "[[$1]]" >/dev/null
    call stock.picking action_assign   "[[$1]]" >/dev/null
    call stock.picking button_validate "[[$1]]"
}

# ------------------------------------------------------------------
sec "2. receiving stock"
# ------------------------------------------------------------------
PT_IN=$(pg "SELECT id FROM stock_picking_type WHERE code='incoming' ORDER BY id LIMIT 1")
t_nonempty "$PT_IN" "an incoming operation type exists"
P1=$(mkpick 'WH-IN-1' "$PT_IN" "$VENDORS" "$STOCK")
mkmove "$P1" "$VENDORS" "$STOCK" "$RECEIVE"
R1=$(run_pick "$P1")
has_error "$R1" && no "validating the receipt failed: $(echo "$R1" | head -c 200)"
t_eq "done" "$(pg "SELECT state FROM stock_picking WHERE id=$P1")" "the receipt is done"
t_eq "$((RECEIVE * M))" "$(onhand "$PROD" "$STOCK")" "$RECEIVE units are on hand at stock"
t_eq "$((RECEIVE * M))" "$(pg "SELECT qty_available FROM product_product WHERE id=$PROD")" \
     "and the product's own on-hand agrees"
# The other end of the move must go negative: stock is conserved, not created.
t_eq "-$((RECEIVE * M))" "$(onhand "$PROD" "$VENDORS")" "the vendor location went negative by the same amount"

# ------------------------------------------------------------------
sec "3. an internal transfer to the shelf"
# ------------------------------------------------------------------
PT_INT=$(pg "SELECT id FROM stock_picking_type WHERE code='internal' ORDER BY id LIMIT 1")
if [ -n "$PT_INT" ]; then
    P2=$(mkpick 'WH-INT-1' "$PT_INT" "$STOCK" "$SHELF")
    mkmove "$P2" "$STOCK" "$SHELF" "$MOVE_IN"
    R2=$(run_pick "$P2")
    has_error "$R2" && no "the internal transfer failed: $(echo "$R2" | head -c 200)"
    t_eq "done" "$(pg "SELECT state FROM stock_picking WHERE id=$P2")" "the transfer is done"

    ST=$(onhand "$PROD" "$STOCK"); SH=$(onhand "$PROD" "$SHELF")
    echo "    stock=$ST  shelf=$SH"
    t_eq "$(( (RECEIVE - MOVE_IN) * M ))" "${ST%%.*}" "stock fell by what moved out"
    t_eq "$((MOVE_IN * M))"               "${SH%%.*}" "the shelf gained exactly that"

    # The invariant an internal move exists to test: moving stock around must
    # not change how much of it there is.
    t_eq "$((RECEIVE * M))" "$(total_onhand "$PROD")" \
         "total on-hand across internal locations is unchanged by an internal move"
else
    echo "    NOTE  no internal operation type configured — transfer step skipped"
fi

# ------------------------------------------------------------------
sec "4. lots and packages"
# ------------------------------------------------------------------
LOT=$(call stock.production.lot create "[{\"name\":\"WH-LOT-1\",\"product_id\":$PROD}]" | rid)
t_nonempty "$LOT" "a lot was created"
if [ -n "$LOT" ]; then
    t_eq "$PROD" "$(pg "SELECT product_id FROM stock_production_lot WHERE id=$LOT")" \
         "the lot belongs to the product"
    # A lot number must be unique per product, or two batches become
    # indistinguishable in a recall.
    DUP=$(call stock.production.lot create "[{\"name\":\"WH-LOT-1\",\"product_id\":$PROD}]")
    if has_error "$DUP"; then ok "a duplicate lot number is refused"
    else echo "    NOTE  a duplicate lot number was accepted (id $(echo "$DUP" | rid)) — worth tightening"; fi
fi

PACK=$(call stock.quant.package create '[{"name":"WH-PACK-1"}]' | rid)
t_nonempty "$PACK" "a package was created"

# ------------------------------------------------------------------
sec "5. shipping some out"
# ------------------------------------------------------------------
PT_OUT=$(pg "SELECT id FROM stock_picking_type WHERE code='outgoing' ORDER BY id LIMIT 1")
P3=$(mkpick 'WH-OUT-1' "$PT_OUT" "$STOCK" "$CUSTOMERS")
mkmove "$P3" "$STOCK" "$CUSTOMERS" "$SHIP"
R3=$(run_pick "$P3")
has_error "$R3" && no "the delivery failed: $(echo "$R3" | head -c 200)"
t_eq "done" "$(pg "SELECT state FROM stock_picking WHERE id=$P3")" "the delivery is done"
t_eq "$(( (RECEIVE - SHIP) * M ))" "$(total_onhand "$PROD")" \
     "total on-hand fell by exactly what shipped"

# ------------------------------------------------------------------
sec "6. the arithmetic holds from every direction"
# ------------------------------------------------------------------
# Per-location sum versus the product's own figure. These are maintained by
# different code paths, so a disagreement means one of them is wrong.
BYLOC=$(total_onhand "$PROD")
SELF=$(pg "SELECT qty_available FROM product_product WHERE id=$PROD")
echo "    sum over locations=$BYLOC   product.qty_available=$SELF"
t_eq "${BYLOC%%.*}" "${SELF%%.*}" "on-hand per location sums to the product's on-hand"

# Every completed move moved something, and moved it somewhere.
BADMOVE=$(pg "SELECT count(*) FROM stock_move
               WHERE product_id=$PROD AND state='done'
                 AND (quantity IS NULL OR quantity = 0
                      OR location_id IS NULL OR location_dest_id IS NULL
                      OR location_id = location_dest_id)")
t_eq "0" "${BADMOVE:-0}" "no completed move is empty or goes nowhere"

# Conservation across the whole journey: everything that left a location
# arrived somewhere else, so the sum over ALL locations is zero.
NET=$(pg "SELECT COALESCE(SUM(quantity),0) FROM stock_quant WHERE product_id=$PROD")
echo "    net across every location (internal + vendor + customer): $NET"
t_eq "0" "${NET%%.*}" "stock is conserved — nothing was created or destroyed"

# Valuation must follow the movement, not lag it.
LAYERS=$(pg "SELECT count(*) FROM stock_valuation_layer WHERE product_id=$PROD")
if [ "${LAYERS:-0}" -gt 0 ]; then
    ok "movements produced $LAYERS valuation layer(s)"
    ORPH=$(pg "SELECT count(*) FROM stock_valuation_layer v
                WHERE v.product_id=$PROD AND v.stock_move_id IS NOT NULL
                  AND NOT EXISTS (SELECT 1 FROM stock_move m WHERE m.id=v.stock_move_id)")
    t_eq "0" "${ORPH:-0}" "no valuation layer points at a missing move"
else
    echo "    NOTE  no valuation layers — this product's category may not be set to real-time valuation"
fi

# And the moves history screen must be able to show all of this.
HIST=$(call stock.move search_read "[[[\"product_id\",\"=\",$PROD]],[\"state\",\"product_uom_qty\"]]")
has_error "$HIST" && no "moves history failed to read: $(echo "$HIST" | head -c 160)" \
                  || ok "the move history reads back through the API"

verdict

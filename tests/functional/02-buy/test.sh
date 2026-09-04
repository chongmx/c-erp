#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# FUNCTIONAL JOURNEY 02 — BUY.  (docs/109 §3)
#
#   purchase order -> confirm -> receive -> vendor bill -> post -> pay
#
# The mirror of 01-sell, and not a redundant one: the money moves the other
# way (payable, not receivable), stock moves IN rather than out, and the
# quantity that has to survive the round trip is `qty_received` rather than
# `qty_delivered`.
#
# That last one is why this journey exists as its own file. The bug 01-sell
# found — a double bound to a BIGINT micro-unit column, serialising as
# "4e+06" — was present TWICE, once in each direction, and only the sale side
# was reachable from the sell journey. A single journey would have left the
# purchase half broken and looking covered.
#
# Ends on the same two invariants, read from the buying side: the books
# balance, and stock ties out.
#
# Everything is prefixed BUY- / 'BUY ' and removed on the way out.
# =============================================================
auth_or_die

QTY=6
COST=125             # majors; the API scales on the way in
EXPECT=$((QTY * COST))
M=1000000

cleanup() {
    pg "DELETE FROM account_move_line WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'BUY %')" >/dev/null
    pg "DELETE FROM account_move      WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'BUY %')" >/dev/null
    pg "DELETE FROM account_payment   WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'BUY %')" >/dev/null
    pg "DELETE FROM stock_move_line   WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'BUY-%')" >/dev/null
    pg "DELETE FROM stock_move        WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'BUY-%')" >/dev/null
    pg "DELETE FROM stock_picking     WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'BUY %')" >/dev/null
    pg "DELETE FROM stock_quant       WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'BUY-%')" >/dev/null
    pg "DELETE FROM purchase_order_line WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'BUY-%')" >/dev/null
    pg "DELETE FROM purchase_order    WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'BUY %')" >/dev/null
    pg "DELETE FROM product_product   WHERE default_code LIKE 'BUY-%'" >/dev/null
    pg "DELETE FROM res_partner       WHERE name LIKE 'BUY %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

# ------------------------------------------------------------------
sec "0. a vendor and something to buy"
# ------------------------------------------------------------------
UOM=$(pg "SELECT id FROM uom_uom ORDER BY id LIMIT 1")
CAT=$(pg "SELECT id FROM product_category ORDER BY id LIMIT 1")
VENDOR=$(pgid "INSERT INTO res_partner (name, active, company_id) VALUES ('BUY Vendor', true, 1) RETURNING id")
PRODUCT=$(pgid "INSERT INTO product_product
    (name, default_code, type, categ_id, uom_id, uom_po_id, list_price, standard_price,
     qty_available, active, sale_ok, purchase_ok, company_id)
    VALUES ('BUY Component','BUY-COMP-1','product',$CAT,$UOM,$UOM,
            200000000, $((COST * M)), 0, true, true, true, 1) RETURNING id")
t_nonempty "$VENDOR"  "a vendor exists"
t_nonempty "$PRODUCT" "a purchasable product exists"
[ -z "$VENDOR" ] || [ -z "$PRODUCT" ] && { verdict; exit 1; }

ONHAND0=$(pg "SELECT COALESCE(quantity,0) FROM stock_quant WHERE product_id=$PRODUCT AND location_id=4")
ONHAND0=${ONHAND0:-0}
echo "    on hand before receiving: ${ONHAND0} micro-units"

# ------------------------------------------------------------------
sec "1. a purchase order"
# ------------------------------------------------------------------
PO=$(call purchase.order create "[{\"partner_id\":$VENDOR}]" | rid)
t_nonempty "$PO" "purchase order created"
[ -z "$PO" ] && { verdict; exit 1; }

LINE=$(call purchase.order.line create \
    "[{\"order_id\":$PO,\"product_id\":$PRODUCT,\"name\":\"BUY line\",\"product_qty\":$QTY,\"price_unit\":$COST}]" | rid)
t_nonempty "$LINE" "a line was added"
t_eq "draft" "$(pg "SELECT state FROM purchase_order WHERE id=$PO")" "the order starts as a draft"

HDR=$(pg "SELECT amount_total FROM purchase_order WHERE id=$PO")
LINES=$(pg "SELECT COALESCE(SUM(price_total),0) FROM purchase_order_line WHERE order_id=$PO")
t_eq "$LINES" "$HDR" "the header equals the sum of its lines"
t_eq "$((EXPECT * M))" "$HDR" "the order totals $EXPECT"
t_eq "0" "$(pg "SELECT COALESCE(qty_received,0) FROM purchase_order_line WHERE id=$LINE")" \
     "nothing is received yet"

# ------------------------------------------------------------------
sec "2. confirming it"
# ------------------------------------------------------------------
call purchase.order action_confirm "[[$PO]]" >/dev/null
t_eq "purchase" "$(pg "SELECT state FROM purchase_order WHERE id=$PO")" "the order is confirmed"
NAME=$(pg "SELECT name FROM purchase_order WHERE id=$PO")
case "$NAME" in P*/*) ok "it took a sequence number ($NAME)" ;; *) no "unexpected order name '$NAME'" ;; esac

PICK=""
for _ in 1 2 3 4 5 6; do
    PICK=$(pg "SELECT id FROM stock_picking WHERE origin='$NAME' ORDER BY id LIMIT 1")
    [ -n "$PICK" ] && break
    sleep 0.5
done
t_nonempty "$PICK" "confirming created a receipt"

# ------------------------------------------------------------------
sec "3. receiving the goods"
# ------------------------------------------------------------------
if [ -n "$PICK" ]; then
    MOVEQ=$(pg "SELECT COALESCE(SUM(product_uom_qty),0) FROM stock_move WHERE picking_id=$PICK AND product_id=$PRODUCT")
    t_eq "$((QTY * M))" "${MOVEQ%%.*}" "the receipt is for the quantity ordered"

    call stock.picking action_confirm  "[[$PICK]]" >/dev/null
    call stock.picking action_assign   "[[$PICK]]" >/dev/null
    VRES=$(call stock.picking button_validate "[[$PICK]]")
    has_error "$VRES" && no "validating the receipt failed: $(echo "$VRES" | head -c 200)"
    t_eq "done" "$(pg "SELECT state FROM stock_picking WHERE id=$PICK")" "the receipt is validated"

    ONHAND1=$(pg "SELECT COALESCE(quantity,0) FROM stock_quant WHERE product_id=$PRODUCT AND location_id=4")
    RECEIVED=$(pg "SELECT COALESCE(SUM(quantity),0) FROM stock_move WHERE picking_id=$PICK AND product_id=$PRODUCT")
    echo "    on hand ${ONHAND0} -> ${ONHAND1:-0}, received $RECEIVED (micro-units)"
    DELTA=$(pg "SELECT (${ONHAND1:-0} - ${ONHAND0})")
    t_eq "${RECEIVED%%.*}" "${DELTA%%.*}" "on-hand rose by exactly what was received"

    # The line the fix was for. qty_received is a BIGINT micro-unit column, and
    # binding a double to it wrote "6e+06", which PostgreSQL rejected — so the
    # whole validate failed and nothing was received at all.
    QR=$(pg "SELECT COALESCE(qty_received,0) FROM purchase_order_line WHERE id=$LINE")
    t_eq "$((QTY * M))" "${QR%%.*}" "the order line records what was received"
fi

# ------------------------------------------------------------------
sec "4. the vendor bill"
# ------------------------------------------------------------------
BRES=$(call purchase.order action_create_bills "[[$PO]]")
has_error "$BRES" && no "creating the bill failed: $(echo "$BRES" | head -c 250)"
echo "    order state at billing time: $(pg "SELECT state FROM purchase_order WHERE id=$PO"), invoice_status: $(pg "SELECT invoice_status FROM purchase_order WHERE id=$PO")"
BILL=$(pg "SELECT id FROM account_move WHERE partner_id=$VENDOR AND move_type='in_invoice' ORDER BY id DESC LIMIT 1")
t_nonempty "$BILL" "a bill was raised from the order"
if [ -n "$BILL" ]; then
    t_eq "draft" "$(pg "SELECT state FROM account_move WHERE id=$BILL")" "it starts as a draft"
    t_eq "$((EXPECT * M))" "$(pg "SELECT amount_total FROM account_move WHERE id=$BILL")" \
         "the bill is for what was ordered ($EXPECT)"
fi

# ------------------------------------------------------------------
sec "5. posting it"
# ------------------------------------------------------------------
if [ -n "$BILL" ]; then
    PRES=$(call account.move action_post "[[$BILL]]")
    has_error "$PRES" && no "posting the bill failed: $(echo "$PRES" | head -c 200)"
    t_eq "posted" "$(pg "SELECT state FROM account_move WHERE id=$BILL")" "the bill is posted"
    BAL=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$BILL")
    t_eq "0" "${BAL%%.*}" "the bill's journal items balance"
fi

# ------------------------------------------------------------------
sec "6. paying the vendor"
# ------------------------------------------------------------------
if [ -n "$BILL" ]; then
    RESID0=$(pg "SELECT amount_residual FROM account_move WHERE id=$BILL")
    t_eq "$((EXPECT * M))" "$RESID0" "the whole amount is outstanding before payment"

    PAYRES=$(call account.move action_register_payment "[[$BILL]]")
    has_error "$PAYRES" && no "registering the payment failed: $(echo "$PAYRES" | head -c 200)"

    RESID1=$(pg "SELECT amount_residual FROM account_move WHERE id=$BILL")
    PSTATE=$(pg "SELECT payment_state FROM account_move WHERE id=$BILL")
    echo "    residual $RESID0 -> $RESID1, payment_state=$PSTATE"
    t_eq "0" "${RESID1%%.*}" "nothing is left outstanding"
    case "$PSTATE" in
        paid|in_payment) ok "the bill reads as paid ($PSTATE)" ;;
        *)               no "payment_state is '$PSTATE' after a full payment" ;;
    esac
fi

# ------------------------------------------------------------------
sec "7. the invariants"
# ------------------------------------------------------------------
JBAL=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0)
             FROM account_move_line WHERE partner_id=$VENDOR")
t_eq "0" "${JBAL%%.*}" "the books balance across everything this journey posted"

OWED=$(pg "SELECT COALESCE(SUM(amount_residual),0) FROM account_move
            WHERE partner_id=$VENDOR AND move_type='in_invoice' AND state='posted'")
t_eq "0" "${OWED%%.*}" "nothing is owed to the vendor"

# ==================================================================
# THE CANCEL PATH — the buying side of the same story.
#
# A second order is confirmed and billed, and then cancelled. The unwind is a
# vendor credit note: the bill stays in the ledger and is neutralised by its
# opposite, rather than being deleted.
#
# The asymmetry worth checking is the direction. A refund on the buying side
# must move the PAYABLE, not the receivable; a reversal posted against the
# wrong account still balances internally and still looks right on the bill.
# ==================================================================
sec "8. a second order, billed, then cancelled"
PO2=$(call purchase.order create "[{\"partner_id\":$VENDOR}]" | rid)
t_nonempty "$PO2" "a second purchase order was raised"
if [ -n "$PO2" ]; then
    call purchase.order.line create \
        "[{\"order_id\":$PO2,\"product_id\":$PRODUCT,\"name\":\"BUY line 2\",\"product_qty\":3,\"price_unit\":$COST}]" >/dev/null
    call purchase.order action_confirm "[[$PO2]]" >/dev/null
    t_eq "purchase" "$(pg "SELECT state FROM purchase_order WHERE id=$PO2")" "it is confirmed"

    call purchase.order action_create_bills "[[$PO2]]" >/dev/null
    BILL2=$(pg "SELECT id FROM account_move WHERE partner_id=$VENDOR AND move_type='in_invoice' ORDER BY id DESC LIMIT 1")
    t_nonempty "$BILL2" "and billed"
    [ -n "$BILL2" ] && call account.move action_post "[[$BILL2]]" >/dev/null
    t_eq "posted" "$(pg "SELECT state FROM account_move WHERE id=$BILL2")" "the bill is posted and now owed"
    t_eq "$((3 * COST * M))" "$(pg "SELECT COALESCE(SUM(amount_residual),0) FROM account_move
                                     WHERE partner_id=$VENDOR AND move_type='in_invoice' AND state='posted'")" \
         "the second bill is what is outstanding"
fi

sec "9. reversing the bill"
RRES=$(call account.move action_reverse "[[$BILL2]]")
CN=$(echo "$RRES" | rid)
t_nonempty "$CN" "action_reverse produced a vendor credit note"
if [ -n "$CN" ]; then
    t_eq "in_refund" "$(pg "SELECT move_type FROM account_move WHERE id=$CN")" \
         "it is a VENDOR refund (in_refund), not a customer one"
    PRES=$(call account.move action_post "[[$CN]]")
    has_error "$PRES" && no "posting the credit note failed: $(echo "$PRES" | head -c 200)"
    t_eq "posted" "$(pg "SELECT state FROM account_move WHERE id=$CN")" "the credit note is posted"
    t_eq "$((3 * COST * M))" "$(pg "SELECT amount_total FROM account_move WHERE id=$CN")" \
         "it is for the full amount of the bill"

    PAIR=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0)
                 FROM account_move_line WHERE move_id IN ($BILL2,$CN)")
    t_eq "0" "${PAIR%%.*}" "bill and credit note cancel each other exactly"

    # The direction check: the payable must be the account that moved back.
    PAY=$(pg "SELECT COALESCE(SUM(l.credit) - SUM(l.debit),0)
                FROM account_move_line l
                JOIN account_account a ON a.id=l.account_id
               WHERE l.move_id IN ($BILL2,$CN) AND a.account_type='liability_payable'")
    t_eq "0" "${PAY%%.*}" "the payable nets back to zero (the reversal hit the right side)"
fi

sec "10. cancelling the order itself"
CRES=$(call purchase.order action_cancel "[[$PO2]]")
has_error "$CRES" && no "cancelling the order failed: $(echo "$CRES" | head -c 200)"
t_eq "cancel" "$(pg "SELECT state FROM purchase_order WHERE id=$PO2")" "the order reads as cancelled"

AGAIN=$(call purchase.order action_create_bills "[[$PO2]]")
if has_error "$AGAIN"; then
    ok "a cancelled order refuses to be billed again"
else
    no "a cancelled order was billed again — $(pg "SELECT count(*) FROM account_move WHERE partner_id=$VENDOR AND move_type='in_invoice'") bill(s) now exist"
fi

sec "11. the invariants, after the unwind"
JBAL2=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0)
              FROM account_move_line WHERE partner_id=$VENDOR")
t_eq "0" "${JBAL2%%.*}" "the books still balance after the reversal"

NET=$(pg "SELECT COALESCE(SUM(l.credit) - SUM(l.debit),0)
            FROM account_move_line l
            JOIN account_account a ON a.id=l.account_id
            JOIN account_move m ON m.id=l.move_id
           WHERE l.partner_id=$VENDOR AND a.account_type='liability_payable' AND m.state='posted'")
echo "    payable position for this vendor: ${NET:-0}"
t_eq "0" "${NET%%.*}" "nothing is owed to the vendor after the cancellation"

verdict

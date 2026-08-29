#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# FUNCTIONAL JOURNEY 01 — SELL.  (docs/109 §3)
#
#   quotation -> confirm -> deliver -> invoice -> post -> pay
#
# One continuous story, asserting the STATE AFTER EACH STEP rather than one
# call in isolation. That is the whole difference from the integration tier:
# every step here is individually covered somewhere in tests/integration, and
# the suite was still green while the *sequence* had gaps, because nothing
# checked that confirming an order produces something a delivery can validate,
# or that the invoice it later raises agrees with what was actually shipped.
#
# It ends on the two invariants an ERP cannot violate and that no per-module
# test ever checks, because they span modules:
#
#   * the books balance — every journal item this journey created nets to zero,
#   * stock ties out — what left the warehouse equals what was sold.
#
# Everything it creates is prefixed JN- / 'JN ' and removed on the way out,
# including on failure.
# =============================================================
auth_or_die

QTY=4
PRICE=250            # majors; the API scales to micro-units on the way in
EXPECT=$((QTY * PRICE))
M=1000000            # micro-units per major, as stored

cleanup() {
    pg "DELETE FROM account_move_line WHERE move_id IN (SELECT id FROM account_move WHERE ref LIKE 'JN-%' OR name LIKE 'JN-%')" >/dev/null
    pg "DELETE FROM account_move_line WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'JN %')" >/dev/null
    pg "DELETE FROM account_move      WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'JN %')" >/dev/null
    pg "DELETE FROM account_payment   WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'JN %')" >/dev/null
    pg "DELETE FROM stock_move_line   WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'JN-%')" >/dev/null
    pg "DELETE FROM stock_move        WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'JN-%')" >/dev/null
    pg "DELETE FROM stock_picking     WHERE origin LIKE 'JN%' OR partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'JN %')" >/dev/null
    pg "DELETE FROM stock_quant       WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'JN-%')" >/dev/null
    pg "DELETE FROM sale_order_line   WHERE order_id IN (SELECT id FROM sale_order WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'JN %'))" >/dev/null
    pg "DELETE FROM sale_order        WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'JN %')" >/dev/null
    pg "DELETE FROM product_product   WHERE default_code LIKE 'JN-%'" >/dev/null
    pg "DELETE FROM res_partner       WHERE name LIKE 'JN %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

# ------------------------------------------------------------------
sec "0. the customer and the thing being sold"
# ------------------------------------------------------------------
# Scaffolding, not the subject: the partner and the product go in directly,
# the same way tests/lib/fixtures.sh makes the canonical set. Everything the
# journey is actually about — the flow — goes through the HTTP API.
UOM=$(pg "SELECT id FROM uom_uom ORDER BY id LIMIT 1")
CAT=$(pg "SELECT id FROM product_category ORDER BY id LIMIT 1")
# company_id is set explicitly, not left NULL. A NULL company means "shared
# across every company", which is not what a sale journey wants — and it also
# leaks: the multi-company isolation test asserts no row is left companyless,
# and would fail on this journey's debris rather than on anything it tests.
PARTNER=$(pgid "INSERT INTO res_partner (name, active, company_id) VALUES ('JN Sell Customer', true, 1) RETURNING id")
PRODUCT=$(pgid "INSERT INTO product_product
    (name, default_code, type, categ_id, uom_id, uom_po_id, list_price, standard_price,
     qty_available, active, sale_ok, purchase_ok, company_id)
    VALUES ('JN Sellable','JN-SELL-1','product',$CAT,$UOM,$UOM,
            $((PRICE * 1000000)), 100000000, 0, true, true, true, 1) RETURNING id")
t_nonempty "$PARTNER" "a customer exists"
t_nonempty "$PRODUCT" "a sellable product exists"
[ -z "$PARTNER" ] || [ -z "$PRODUCT" ] && { verdict; exit 1; }

# Stock to ship from. Location 4 is the internal stock location.
call stock.quant set_on_hand "[{\"product_id\":$PRODUCT,\"location_id\":4,\"quantity\":100}]" >/dev/null
ONHAND0=$(pg "SELECT COALESCE(quantity,0) FROM stock_quant WHERE product_id=$PRODUCT AND location_id=4")
t_ge "${ONHAND0%%.*}" "$((QTY * M))" "enough stock on hand to ship the order"

# ------------------------------------------------------------------
sec "1. a quotation"
# ------------------------------------------------------------------
SO=$(call sale.order create "[{\"partner_id\":$PARTNER}]" | rid)
t_nonempty "$SO" "quotation created"
[ -z "$SO" ] && { verdict; exit 1; }

LINE=$(call sale.order.line create \
    "[{\"order_id\":$SO,\"product_id\":$PRODUCT,\"name\":\"JN line\",\"product_uom_qty\":$QTY,\"price_unit\":$PRICE}]" | rid)
t_nonempty "$LINE" "a line was added"

t_eq "draft" "$(pg "SELECT state FROM sale_order WHERE id=$SO")" "the order starts as a draft quotation"
# The header must agree with its lines the moment a line is written — a stale
# header is the classic sale-order bug, and it is invisible until an invoice
# is raised from it.
HDR=$(pg "SELECT amount_total FROM sale_order WHERE id=$SO")
LINES=$(pg "SELECT COALESCE(SUM(price_total),0) FROM sale_order_line WHERE order_id=$SO")
t_eq "$LINES" "$HDR" "the header equals the sum of its lines"
t_eq "$((EXPECT * 1000000))" "$HDR" "the quotation totals $EXPECT"

# ------------------------------------------------------------------
sec "2. confirming it"
# ------------------------------------------------------------------
call sale.order action_confirm "[[$SO]]" >/dev/null
t_eq "sale" "$(pg "SELECT state FROM sale_order WHERE id=$SO")" "the order is confirmed"
NAME=$(pg "SELECT name FROM sale_order WHERE id=$SO")
case "$NAME" in SO/*) ok "it took a sequence number ($NAME)" ;; *) no "unexpected order name '$NAME'" ;; esac

# Confirming must produce something to ship. Poll: the confirm creates the
# picking and its moves in the same request, but the commit can land a moment
# after the HTTP response.
PICK=""
for _ in 1 2 3 4 5 6; do
    PICK=$(pg "SELECT id FROM stock_picking WHERE origin='$NAME' ORDER BY id LIMIT 1")
    [ -n "$PICK" ] && break
    sleep 0.5
done
t_nonempty "$PICK" "confirming created a delivery"

# ------------------------------------------------------------------
sec "3. shipping it"
# ------------------------------------------------------------------
if [ -n "$PICK" ]; then
    # Stock quantities are BIGINT micro-units in the database, majors over the
    # API. Mixing the two is the classic reading error here — 4 units reads as
    # 4000000, and an assertion against 4 fails while nothing is wrong.
    MOVEQ=$(pg "SELECT COALESCE(SUM(product_uom_qty),0) FROM stock_move WHERE picking_id=$PICK AND product_id=$PRODUCT")
    t_eq "$((QTY * M))" "${MOVEQ%%.*}" "the delivery is for the quantity ordered"

    # confirm -> assign -> validate, in that order. Skipping the confirm leaves
    # the picking short of a state button_validate will act on.
    call stock.picking action_confirm  "[[$PICK]]" >/dev/null
    call stock.picking action_assign   "[[$PICK]]" >/dev/null
    VRES=$(call stock.picking button_validate "[[$PICK]]")
    has_error "$VRES" && no "validating the delivery failed: $(echo "$VRES" | head -c 200)"
    t_eq "done" "$(pg "SELECT state FROM stock_picking WHERE id=$PICK")" "the delivery is validated"

    ONHAND1=$(pg "SELECT COALESCE(quantity,0) FROM stock_quant WHERE product_id=$PRODUCT AND location_id=4")
    # stock_move.quantity is the quantity actually DONE, which is what the
    # warehouse really parted with — not product_uom_qty, which is the demand.
    SHIPPED=$(pg "SELECT COALESCE(SUM(quantity),0) FROM stock_move WHERE picking_id=$PICK AND product_id=$PRODUCT")
    echo "    on hand $ONHAND0 -> $ONHAND1, shipped $SHIPPED (micro-units)"
    # Stock ties out: what left the warehouse is exactly what the delivery says
    # it moved. Asserting the DELTA rather than an absolute keeps this true
    # whatever else is in the database.
    DELTA=$(pg "SELECT (${ONHAND0:-0} - ${ONHAND1:-0})")
    t_eq "${SHIPPED%%.*}" "${DELTA%%.*}" "on-hand fell by exactly what was shipped"
fi

# ------------------------------------------------------------------
sec "4. invoicing it"
# ------------------------------------------------------------------
call sale.order action_create_invoices "[[$SO]]" >/dev/null
INV=$(pg "SELECT id FROM account_move WHERE partner_id=$PARTNER AND move_type='out_invoice' ORDER BY id DESC LIMIT 1")
t_nonempty "$INV" "an invoice was raised from the order"
if [ -n "$INV" ]; then
    t_eq "draft" "$(pg "SELECT state FROM account_move WHERE id=$INV")" "it starts as a draft"
    INVTOT=$(pg "SELECT amount_total FROM account_move WHERE id=$INV")
    # The invoice must bill what was sold. A mismatch here is the expensive
    # kind of bug: it is money, and nobody notices until a customer does.
    t_eq "$((EXPECT * 1000000))" "$INVTOT" "the invoice bills what was ordered ($EXPECT)"
fi

# ------------------------------------------------------------------
sec "5. posting it"
# ------------------------------------------------------------------
if [ -n "$INV" ]; then
    call account.move action_post "[[$INV]]" >/dev/null
    t_eq "posted" "$(pg "SELECT state FROM account_move WHERE id=$INV")" "the invoice is posted"
    BAL=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$INV")
    t_eq "0" "${BAL%%.*}" "the invoice's journal items balance"
    t_ne "0" "$(pg "SELECT count(*) FROM account_move_line WHERE move_id=$INV")" "it has journal items at all"
fi

# ------------------------------------------------------------------
sec "6. getting paid"
# ------------------------------------------------------------------
if [ -n "$INV" ]; then
    RESID0=$(pg "SELECT amount_residual FROM account_move WHERE id=$INV")
    t_eq "$((EXPECT * 1000000))" "$RESID0" "the whole amount is outstanding before payment"

    PAYRES=$(call account.move action_register_payment "[[$INV]]")
    has_error "$PAYRES" && no "registering the payment failed: $(echo "$PAYRES" | head -c 200)"

    RESID1=$(pg "SELECT amount_residual FROM account_move WHERE id=$INV")
    PSTATE=$(pg "SELECT payment_state FROM account_move WHERE id=$INV")
    echo "    residual $RESID0 -> $RESID1, payment_state=$PSTATE"
    t_eq "0" "${RESID1%%.*}" "nothing is left outstanding"
    case "$PSTATE" in
        paid|in_payment) ok "the invoice reads as paid ($PSTATE)" ;;
        *)               no "payment_state is '$PSTATE' after a full payment" ;;
    esac
fi

# ------------------------------------------------------------------
sec "7. the invariants"
# ------------------------------------------------------------------
# Every journal item this journey produced, across every move, nets to zero.
# A single-move check would miss a payment that posts a lopsided entry.
JBAL=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0)
             FROM account_move_line WHERE partner_id=$PARTNER")
t_eq "0" "${JBAL%%.*}" "the books balance across everything this journey posted"

# And the customer owes nothing: the receivable side nets out once paid.
OWED=$(pg "SELECT COALESCE(SUM(amount_residual),0) FROM account_move
            WHERE partner_id=$PARTNER AND move_type='out_invoice' AND state='posted'")
t_eq "0" "${OWED%%.*}" "the customer owes nothing"

# ==================================================================
# THE CANCEL PATH
#
# The happy path above is the half that gets tested. This is the half that
# actually happens: a second order is confirmed and invoiced, and THEN the
# customer cancels.
#
# You cannot delete a posted invoice — it is numbered, it is in the ledger, and
# in most jurisdictions removing it is illegal. The correct unwind is a
# REVERSAL: a credit note in the RINV series that cancels the original entry by
# posting its opposite. What has to be true afterwards is not "the invoice is
# gone" but "the two together net to nothing", which is a stronger and much
# more testable claim.
# ==================================================================
sec "8. a second order, invoiced, that the customer then cancels"
SO2=$(call sale.order create "[{\"partner_id\":$PARTNER}]" | rid)
t_nonempty "$SO2" "a second order was raised"
if [ -n "$SO2" ]; then
    call sale.order.line create \
        "[{\"order_id\":$SO2,\"product_id\":$PRODUCT,\"name\":\"JN line 2\",\"product_uom_qty\":2,\"price_unit\":$PRICE}]" >/dev/null
    call sale.order action_confirm "[[$SO2]]" >/dev/null
    t_eq "sale" "$(pg "SELECT state FROM sale_order WHERE id=$SO2")" "it is confirmed"

    call sale.order action_create_invoices "[[$SO2]]" >/dev/null
    INV2=$(pg "SELECT id FROM account_move WHERE partner_id=$PARTNER AND move_type='out_invoice' ORDER BY id DESC LIMIT 1")
    t_nonempty "$INV2" "and invoiced"
    [ -n "$INV2" ] && call account.move action_post "[[$INV2]]" >/dev/null
    t_eq "posted" "$(pg "SELECT state FROM account_move WHERE id=$INV2")" "the invoice is posted and now owed"
fi

sec "9. reversing the invoice"
OWED_BEFORE=$(pg "SELECT COALESCE(SUM(amount_residual),0) FROM account_move
                   WHERE partner_id=$PARTNER AND move_type='out_invoice' AND state='posted'")
echo "    outstanding before the reversal: $OWED_BEFORE"
t_eq "$((2 * PRICE * M))" "${OWED_BEFORE%%.*}" "the second order is what is outstanding"

RRES=$(call account.move action_reverse "[[$INV2]]")
CN=$(echo "$RRES" | rid)
t_nonempty "$CN" "action_reverse produced a credit note"
if [ -n "$CN" ]; then
    t_eq "out_refund" "$(pg "SELECT move_type FROM account_move WHERE id=$CN")" "it is a customer refund, not another invoice"
    PRES=$(call account.move action_post "[[$CN]]")
    has_error "$PRES" && no "posting the credit note failed: $(echo "$PRES" | head -c 200)"
    CNNAME=$(pg "SELECT name FROM account_move WHERE id=$CN")
    case "$CNNAME" in
        RINV*) ok "it is numbered in the RINV series ($CNNAME)" ;;
        *)     no "the credit note is numbered '$CNNAME', not RINV — reversals must be a separate series" ;;
    esac
    t_eq "$((2 * PRICE * M))" "$(pg "SELECT amount_total FROM account_move WHERE id=$CN")" \
         "it is for the full amount of the invoice"
    CNBAL=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$CN")
    t_eq "0" "${CNBAL%%.*}" "the credit note balances on its own"

    # The pair must be mirror images. Summing them is what proves the reversal
    # reversed the right thing: a credit note that balances internally can
    # still credit the wrong account.
    PAIR=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0)
                 FROM account_move_line WHERE move_id IN ($INV2,$CN)")
    t_eq "0" "${PAIR%%.*}" "invoice and credit note cancel each other exactly"
fi

sec "10. cancelling the order itself"
CRES=$(call sale.order action_cancel "[[$SO2]]")
has_error "$CRES" && no "cancelling the order failed: $(echo "$CRES" | head -c 200)"
t_eq "cancel" "$(pg "SELECT state FROM sale_order WHERE id=$SO2")" "the order reads as cancelled"

# A cancelled order must not be able to raise more invoices. This is the guard
# that stops a cancelled sale being billed by a retry or a stale browser tab.
AGAIN=$(call sale.order action_create_invoices "[[$SO2]]")
INV_COUNT=$(pg "SELECT count(*) FROM account_move WHERE partner_id=$PARTNER AND move_type='out_invoice'")
if has_error "$AGAIN"; then
    ok "a cancelled order refuses to be invoiced again"
else
    no "a cancelled order was invoiced again — now $INV_COUNT invoice(s) exist for this customer"
fi

sec "11. the invariants, after the unwind"
JBAL2=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0)
              FROM account_move_line WHERE partner_id=$PARTNER")
t_eq "0" "${JBAL2%%.*}" "the books still balance after the reversal"

# The whole point of the cancel path: the customer is square again. The paid
# order contributed nothing, and the cancelled one was fully credited.
NET=$(pg "SELECT COALESCE(SUM(l.debit) - SUM(l.credit),0)
            FROM account_move_line l
            JOIN account_account a ON a.id=l.account_id
            JOIN account_move m ON m.id=l.move_id
           WHERE l.partner_id=$PARTNER AND a.account_type='asset_receivable' AND m.state='posted'")
echo "    receivable position for this customer: ${NET:-0}"
t_eq "0" "${NET%%.*}" "the customer owes nothing after the cancellation"

verdict

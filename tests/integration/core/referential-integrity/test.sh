#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# Whatever database this run starts from must be internally consistent.
#
# WHY THIS EXISTS — it found a real one. `baseline.dump` shipped with 173
# products whose `product_tmpl_id` pointed at product_template rows that had
# been deleted, and product_template's sequence restarted at 1. So the first
# template any test created took a low id and SILENTLY ADOPTED a crowd of demo
# products. The symptom was two unrelated-looking failures a long way from the
# cause:
#
#     pricelists      "category precedence failed: 90.0"  and  "formula gave 5.1"
#     product-variants "3 product(s) have no template", "row count drifted"
#
# Both were reading a demo resistor that had attached itself to their template.
# Nothing in the suite checked the state it was starting FROM, so a broken
# starting state could only ever surface as a puzzle somewhere else.
#
# It runs early (order=5) so this reads as "the ground is unsound" rather than
# as an area failure. Several of these columns carry no foreign key in the
# schema — which is exactly why they need asserting here.
# =============================================================

dangling() {  # dangling <child> <fk column> <parent> <label>
    local n
    n=$(pg "SELECT count(*) FROM $1 c
             WHERE c.$2 IS NOT NULL
               AND NOT EXISTS (SELECT 1 FROM $3 p WHERE p.id = c.$2)")
    if [ "${n:-0}" = "0" ]; then
        ok "$4"
    else
        no "$4 — $n row(s) in $1 point at a missing $3"
    fi
}

sec "products and templates"
# No FK on this column, and it is the one that broke.
dangling product_product product_tmpl_id product_template "every product's template exists"
# The other half of the same bug: even with no dangling rows, a sequence that
# hands out an id already in use recreates it on the next create().
NEXT=$(pg "SELECT last_value FROM product_template_id_seq")
MAXT=$(pg "SELECT COALESCE(MAX(id),0) FROM product_template")
echo "    product_template: max id $MAXT, sequence at $NEXT"
if [ -n "$NEXT" ] && [ -n "$MAXT" ] && [ "$NEXT" -ge "$MAXT" ]; then
    ok "the template sequence is past every id in use"
else
    no "the template sequence ($NEXT) is BEHIND the highest id ($MAXT) — the next created template will collide"
fi

sec "orders and their lines"
dangling sale_order_line     order_id    sale_order      "every sale line has its order"
dangling sale_order_line     product_id  product_product "every sale line has its product"
dangling purchase_order_line order_id    purchase_order  "every purchase line has its order"

sec "the ledger"
dangling account_move_line move_id    account_move    "every journal item has its move"
dangling account_move_line account_id account_account "every journal item has its account"
dangling account_move      journal_id account_journal "every move has its journal"

sec "stock"
dangling stock_move      product_id product_product "every stock move has its product"
dangling stock_move_line move_id    stock_move      "every move line has its move"
dangling stock_quant     product_id product_product "every quant has its product"

sec "the ledger balances"
# The invariant that outranks everything else here: posted double entry nets to
# zero. If this is false, no report downstream can be right.
BAL=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0)
            FROM account_move_line l
            JOIN account_move m ON m.id = l.move_id
           WHERE m.state = 'posted'")
t_eq "0" "${BAL%%.*}" "posted journal items balance to zero"

verdict

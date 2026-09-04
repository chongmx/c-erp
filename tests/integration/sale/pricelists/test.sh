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
# Pricelists (docs/096).
#
# A pricelist is an ordered set of rules and the first match wins. The three
# things that decide whether it is right:
#
#   * PRECEDENCE — a rule for this exact variant must beat one for its product,
#     which beats its category, which beats a global rule. Get this wrong and a
#     blanket discount silently overrides a negotiated price.
#   * QUANTITY BREAKS — a min_quantity rule must not apply below its threshold,
#     and the larger break must win once reached.
#   * DATES — a promotion outside its window must not apply. Half-open windows
#     are the usual bug; here both bounds are inclusive and both are asserted.
#
# Money is micro-units end to end, so the arithmetic is checked against exact
# integers rather than "about right".
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }

cleanup(){
    pg "DELETE FROM product_pricelist_item WHERE pricelist_id IN (SELECT id FROM product_pricelist WHERE name LIKE 'QA-PL%')" >/dev/null
    pg "DELETE FROM product_pricelist WHERE name LIKE 'QA-PL%'" >/dev/null
    pg "DELETE FROM product_product WHERE name LIKE 'QA-PL%'" >/dev/null
    pg "DELETE FROM product_template WHERE name LIKE 'QA-PL%'" >/dev/null
    pg "DELETE FROM product_category WHERE name LIKE 'QA-PL%'" >/dev/null
}
cleanup; trap cleanup EXIT

SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; echo "*** FAILURES ***"; exit 1; }
CTX="\"context\":{\"session_id\":\"$SID\"}"
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":{$CTX}}}"; }
rid(){ sed -n 's/.*"result":\([0-9][0-9]*\).*/\1/p'; }
# price <pricelist> <product> <qty> [date]  -> the resolved unit price.
# Built line by line rather than with a nested $(...) inside the quoted JSON —
# that produced malformed JSON for every dated call and looked like the date
# logic was broken when it was the shell escaping.
price(){
    local body="{\"pricelist_id\":$1,\"product_id\":$2,\"quantity\":$3"
    [ -n "$4" ] && body="$body,\"date\":\"$4\""
    body="$body}"
    call product.pricelist get_price "[$body]" | sed -n 's/.*"price":\([0-9.]*\).*/\1/p'
}

echo "############ fixture ############"
CAT=$(call product.category create '[{"name":"QA-PL Cat"}]' | rid)
T=$(call product.template create "[{\"name\":\"QA-PL Widget\",\"list_price\":100,\"standard_price\":40,\"categ_id\":$CAT}]" | rid)
call product.template generate_variants "[{\"product_tmpl_id\":$T}]" >/dev/null
P=$(pg "SELECT id FROM product_product WHERE product_tmpl_id=$T LIMIT 1")
PL=$(call product.pricelist create '[{"name":"QA-PL Trade"}]' | rid)
[ -n "$P" ] && [ -n "$PL" ] && ok "product ($P) and pricelist ($PL) created" \
    || { no "fixture failed"; echo "*** FAILURES ***"; exit 1; }

echo "############ no pricelist, and an empty one, fall back to the list price ############"
[ "$(price 0 "$P" 1)" = "100.0" ] && ok "no pricelist gives the list price" || no "got $(price 0 "$P" 1)"
[ "$(price "$PL" "$P" 1)" = "100.0" ] && ok "an empty pricelist gives the list price" || no "got $(price "$PL" "$P" 1)"

echo "############ a global percentage rule ############"
# 10% off 100 = 90
G=$(call product.pricelist.item create "[{\"pricelist_id\":$PL,\"applied_on\":\"3_global\",\"compute_price\":\"percentage\",\"percent_price\":10}]" | rid)
[ "$(price "$PL" "$P" 1)" = "90.0" ] && ok "10% off 100 = 90" || no "global percentage gave $(price "$PL" "$P" 1)"

echo "############ a category rule beats the global one ############"
C=$(call product.pricelist.item create "[{\"pricelist_id\":$PL,\"applied_on\":\"2_product_category\",\"categ_id\":$CAT,\"compute_price\":\"fixed\",\"fixed_price\":80}]" | rid)
[ "$(price "$PL" "$P" 1)" = "80.0" ] && ok "the category rule wins (80)" || no "category precedence failed: $(price "$PL" "$P" 1)"

echo "############ a product rule beats the category rule ############"
TR=$(call product.pricelist.item create "[{\"pricelist_id\":$PL,\"applied_on\":\"1_product\",\"product_tmpl_id\":$T,\"compute_price\":\"fixed\",\"fixed_price\":70}]" | rid)
[ "$(price "$PL" "$P" 1)" = "70.0" ] && ok "the product rule wins (70)" || no "product precedence failed: $(price "$PL" "$P" 1)"

echo "############ a variant rule beats them all ############"
VR=$(call product.pricelist.item create "[{\"pricelist_id\":$PL,\"applied_on\":\"0_product_variant\",\"product_id\":$P,\"compute_price\":\"fixed\",\"fixed_price\":60}]" | rid)
[ "$(price "$PL" "$P" 1)" = "60.0" ] && ok "the variant rule wins (60)" || no "variant precedence failed: $(price "$PL" "$P" 1)"

echo "############ quantity breaks ############"
# A break at 10 must not apply at 9, and must apply at 10 and above.
QB=$(call product.pricelist.item create "[{\"pricelist_id\":$PL,\"applied_on\":\"0_product_variant\",\"product_id\":$P,\"min_quantity\":10,\"compute_price\":\"fixed\",\"fixed_price\":50,\"sequence\":5}]" | rid)
[ "$(price "$PL" "$P" 9)"  = "60.0" ] && ok "below the break, the unit rule still applies (60)" || no "qty 9 gave $(price "$PL" "$P" 9)"
[ "$(price "$PL" "$P" 10)" = "50.0" ] && ok "at the break it applies (50)"                      || no "qty 10 gave $(price "$PL" "$P" 10)"
[ "$(price "$PL" "$P" 99)" = "50.0" ] && ok "and above it"                                     || no "qty 99 gave $(price "$PL" "$P" 99)"

echo "############ dated promotions ############"
pg "DELETE FROM product_pricelist_item WHERE id IN ($VR,$QB)" >/dev/null
D=$(call product.pricelist.item create "[{\"pricelist_id\":$PL,\"applied_on\":\"0_product_variant\",\"product_id\":$P,\"compute_price\":\"fixed\",\"fixed_price\":25,\"date_start\":\"2026-01-01\",\"date_end\":\"2026-01-31\",\"sequence\":1}]" | rid)
[ "$(price "$PL" "$P" 1 2026-01-15)" = "25.0" ] && ok "inside the window the promotion applies" || no "in-window gave $(price "$PL" "$P" 1 2026-01-15)"
[ "$(price "$PL" "$P" 1 2026-01-01)" = "25.0" ] && ok "the first day is included"               || no "start day excluded"
[ "$(price "$PL" "$P" 1 2026-01-31)" = "25.0" ] && ok "the last day is included"                || no "end day excluded"
[ "$(price "$PL" "$P" 1 2026-02-01)" = "70.0" ] && ok "the day after, it does not"              || no "after-window gave $(price "$PL" "$P" 1 2026-02-01)"
[ "$(price "$PL" "$P" 1 2025-12-31)" = "70.0" ] && ok "nor the day before"                      || no "before-window gave $(price "$PL" "$P" 1 2025-12-31)"

echo "############ formula: base, discount, surcharge ############"
pg "DELETE FROM product_pricelist_item WHERE id=$D" >/dev/null
pg "UPDATE product_pricelist_item SET compute_price='formula', base='standard_price',
      price_discount=0, price_surcharge=5000000 WHERE id=$TR" >/dev/null
# cost 40 + surcharge 5 = 45
[ "$(price "$PL" "$P" 1)" = "45.0" ] && ok "cost 40 + surcharge 5 = 45" || no "formula gave $(price "$PL" "$P" 1)"
pg "UPDATE product_pricelist_item SET base='list_price', price_discount=25000000, price_surcharge=0 WHERE id=$TR" >/dev/null
# list 100 less 25% = 75
[ "$(price "$PL" "$P" 1)" = "75.0" ] && ok "list 100 less 25% = 75" || no "discount formula gave $(price "$PL" "$P" 1)"

echo "############ a price is never negative ############"
pg "UPDATE product_pricelist_item SET price_discount=500000000, price_surcharge=0 WHERE id=$TR" >/dev/null
NEG=$(price "$PL" "$P" 1)
awk -v v="$NEG" 'BEGIN{exit !(v >= 0)}' && ok "a 500% discount floors at 0, not negative ($NEG)" \
                                        || no "negative price: $NEG"

echo "############ the breakdown explains the choice ############"
B=$(call product.pricelist price_breakdown "[{\"pricelist_id\":$PL,\"product_id\":$P,\"quantity\":1}]")
echo "$B" | grep -q '"applies"' && ok "price_breakdown lists candidate rules" || no "no breakdown: $(echo "$B" | head -c 120)"

echo "############ the screens resolve ############"
for m in product.pricelist product.pricelist.item; do
    R=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$m\",\"method\":\"get_views\",\"args\":[[[false,\"list\"]]],\"kwargs\":{$CTX}}}")
    echo "$R" | grep -q '"error"' && no "$m get_views failed" || ok "$m get_views resolves"
done
[ "$(pg "SELECT count(*) FROM information_schema.columns WHERE table_name='res_partner' AND column_name='property_product_pricelist_id'")" = "1" ] \
    && ok "a customer can be put on a pricelist" || no "res_partner has no pricelist column"
[ "$(pg "SELECT count(*) FROM information_schema.columns WHERE table_name='sale_order' AND column_name='pricelist_id'")" = "1" ] \
    && ok "a sale order records the pricelist it used" || no "sale_order has no pricelist column"

if [ -n "$FAILED" ]; then echo; echo "*** FAILURES ***"; exit 1; fi
echo; echo "  All checks passed."

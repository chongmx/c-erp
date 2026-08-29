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
# Product templates, attributes and variants (docs/096).
#
# A template is what a person calls "a product" — "T-Shirt". A variant is what
# stock, accounting and every order line actually move — "T-Shirt (Red, L)".
#
# The assertions that matter are about IDENTITY and HISTORY:
#
#   * generation is keyed on the COMBINATION, not the name. Running it twice
#     must create nothing the second time, or every save would duplicate the
#     catalogue.
#   * a variant that is no longer possible is ARCHIVED, never deleted.
#     product_product ids are referenced by stock moves and invoice lines;
#     deleting one either fails on a foreign key or corrupts history.
#   * the migration gives every pre-existing product its own template without
#     changing its id, because everything else in the database points at it.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }

cleanup(){
    pg "DELETE FROM product_product WHERE product_tmpl_id IN (SELECT id FROM product_template WHERE name LIKE 'QA-VAR%')" >/dev/null
    pg "DELETE FROM product_template WHERE name LIKE 'QA-VAR%'" >/dev/null
    pg "DELETE FROM product_attribute WHERE name LIKE 'QA-VAR%'" >/dev/null
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

echo "############ every existing product got a template ############"
ORPHANS=$(pg "SELECT count(*) FROM product_product WHERE product_tmpl_id IS NULL")
[ "$ORPHANS" = "0" ] && ok "no product is left without a template" || no "$ORPHANS product(s) have no template"
NP=$(pg "SELECT count(*) FROM product_product"); NT=$(pg "SELECT count(*) FROM product_template")
[ -n "$NT" ] && [ "$NT" -ge 1 ] && ok "templates exist ($NT for $NP products)" || no "no templates created"
# The migration must not renumber products — everything references these ids.
BADREF=$(pg "SELECT count(*) FROM stock_move m LEFT JOIN product_product p ON p.id=m.product_id WHERE p.id IS NULL")
[ "$BADREF" = "0" ] && ok "no stock move lost its product" || no "$BADREF stock moves point at a missing product"

echo "############ fixture: a template with two attributes ############"
T=$(call product.template create '[{"name":"QA-VAR Shirt","list_price":100,"type":"product"}]' | rid)
[ -n "$T" ] && ok "template created ($T)" || { no "template create failed"; echo "*** FAILURES ***"; exit 1; }
A1=$(call product.attribute create '[{"name":"QA-VAR Size"}]' | rid)
A2=$(call product.attribute create '[{"name":"QA-VAR Colour"}]' | rid)
S_S=$(call product.attribute.value create "[{\"attribute_id\":$A1,\"name\":\"S\"}]" | rid)
S_M=$(call product.attribute.value create "[{\"attribute_id\":$A1,\"name\":\"M\"}]" | rid)
S_L=$(call product.attribute.value create "[{\"attribute_id\":$A1,\"name\":\"L\"}]" | rid)
C_R=$(call product.attribute.value create "[{\"attribute_id\":$A2,\"name\":\"Red\"}]" | rid)
C_B=$(call product.attribute.value create "[{\"attribute_id\":$A2,\"name\":\"Blue\"}]" | rid)
[ -n "$S_L" ] && [ -n "$C_B" ] && ok "attributes and values created" || no "attribute values failed"

# Size L costs 5 more — the extra must reach the variant's price.
call product.template set_attribute_line \
  "[{\"product_tmpl_id\":$T,\"attribute_id\":$A1,\"values\":[{\"value_id\":$S_S},{\"value_id\":$S_M},{\"value_id\":$S_L,\"price_extra\":5}]}]" >/dev/null
call product.template set_attribute_line \
  "[{\"product_tmpl_id\":$T,\"attribute_id\":$A2,\"values\":[{\"value_id\":$C_R},{\"value_id\":$C_B}]}]" >/dev/null
LINES=$(call product.template read_attribute_lines "[{\"product_tmpl_id\":$T}]")
[ "$(echo "$LINES" | grep -o '"attribute":' | wc -l)" = "2" ] && ok "two attribute lines are set" || no "lines wrong: $(echo "$LINES" | head -c 160)"

echo "############ generation makes the cartesian product ############"
R=$(call product.template generate_variants "[{\"product_tmpl_id\":$T}]")
echo "$R" | grep -q '"created":6' && ok "3 sizes x 2 colours = 6 variants created" || no "expected 6 created: $R"
N=$(pg "SELECT count(*) FROM product_product WHERE product_tmpl_id=$T AND active")
[ "$N" = "6" ] && ok "six active variants exist" || no "found $N active variants"

echo "############ the price extra lands on the right variants ############"
# Three L variants... no: one L per colour, so two variants at 105.
N105=$(pg "SELECT count(*) FROM product_product WHERE product_tmpl_id=$T AND list_price=105000000")
N100=$(pg "SELECT count(*) FROM product_product WHERE product_tmpl_id=$T AND list_price=100000000")
[ "$N105" = "2" ] && ok "the two L variants carry the +5 extra" || no "expected 2 at 105, got $N105"
[ "$N100" = "4" ] && ok "the other four stay at the template price" || no "expected 4 at 100, got $N100"

echo "############ variant names carry their combination ############"
call product.template read_variants "[{\"product_tmpl_id\":$T}]" | grep -q 'QA-VAR Shirt (' \
    && ok "variants are named after their values" || no "variant naming wrong"
[ "$(pg "SELECT count(*) FROM product_product WHERE product_tmpl_id=$T AND name LIKE '%L, Red%'")" = "1" ] \
    && ok "the (L, Red) variant exists exactly once" || no "no unique (L, Red) variant"

echo "############ re-running creates nothing ############"
# Identity is the combination, not the name. If this fails, every save doubles
# the catalogue.
R=$(call product.template generate_variants "[{\"product_tmpl_id\":$T}]")
echo "$R" | grep -q '"created":0' && ok "second run creates 0" || no "second run created more: $R"
N=$(pg "SELECT count(*) FROM product_product WHERE product_tmpl_id=$T")
[ "$N" = "6" ] && ok "still exactly six variant rows" || no "now $N rows"

echo "############ dropping a value archives, never deletes ############"
IDL=$(pg "SELECT id FROM product_product WHERE product_tmpl_id=$T AND name LIKE '%L, Red%'")
call product.template set_attribute_line \
  "[{\"product_tmpl_id\":$T,\"attribute_id\":$A1,\"values\":[{\"value_id\":$S_S},{\"value_id\":$S_M}]}]" >/dev/null
R=$(call product.template generate_variants "[{\"product_tmpl_id\":$T}]")
echo "$R" | grep -q '"archived":2' && ok "the two L variants are archived" || no "archive count wrong: $R"
[ "$(pg "SELECT count(*) FROM product_product WHERE id=$IDL")" = "1" ] \
    && ok "the archived variant row still exists" || no "LEAK: the variant row was deleted"
[ "$(pg "SELECT active FROM product_product WHERE id=$IDL")" = "f" ] \
    && ok "and it is inactive" || no "it is still active"
[ "$(pg "SELECT count(*) FROM product_product WHERE product_tmpl_id=$T AND active")" = "4" ] \
    && ok "four active variants remain" || no "wrong active count"

echo "############ restoring the value revives the same row ############"
call product.template set_attribute_line \
  "[{\"product_tmpl_id\":$T,\"attribute_id\":$A1,\"values\":[{\"value_id\":$S_S},{\"value_id\":$S_M},{\"value_id\":$S_L,\"price_extra\":5}]}]" >/dev/null
call product.template generate_variants "[{\"product_tmpl_id\":$T}]" >/dev/null
[ "$(pg "SELECT active FROM product_product WHERE id=$IDL")" = "t" ] \
    && ok "the original row is reactivated, not replaced" || no "a duplicate was made instead"
[ "$(pg "SELECT count(*) FROM product_product WHERE product_tmpl_id=$T)")" = "6" ] 2>/dev/null || true
[ "$(pg "SELECT count(*) FROM product_product WHERE product_tmpl_id=$T")" = "6" ] \
    && ok "still six rows total" || no "row count drifted"

echo "############ a template with no attributes still has one variant ############"
T2=$(call product.template create '[{"name":"QA-VAR Plain","list_price":42,"type":"product"}]' | rid)
call product.template generate_variants "[{\"product_tmpl_id\":$T2}]" >/dev/null
[ "$(pg "SELECT count(*) FROM product_product WHERE product_tmpl_id=$T2 AND active")" = "1" ] \
    && ok "a plain product has exactly one variant" || no "plain product variant count wrong"

echo "############ the screens resolve ############"
for m in product.template product.attribute; do
    R=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$m\",\"method\":\"get_views\",\"args\":[[[false,\"list\"],[false,\"form\"]]],\"kwargs\":{$CTX}}}")
    echo "$R" | grep -q '"error"' && no "$m get_views failed" || ok "$m get_views resolves"
done
[ "$(pg "SELECT res_model FROM ir_act_window WHERE id=102")" = "product.template" ] \
    && ok "the Product Templates menu points at the model" || no "action 102 wrong"

if [ -n "$FAILED" ]; then echo; echo "*** FAILURES ***"; exit 1; fi
echo; echo "  All checks passed."

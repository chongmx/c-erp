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
# Electronics units, parametric search, and the part-lookup API (docs/097).
#
# The load-bearing assertion is CROSS-PREFIX MATCHING. Before this, part_unit
# held one row (Ohm) and search compared value_numeric directly, so a resistor
# entered as 4.7 kΩ and one entered as 4700 Ω did not match each other and
# neither matched a "4k–5k" range. Every value is now stored twice: as typed,
# and as `value_base` in the SI base of its quantity — which is the only reason
# a range query can be a plain BETWEEN.
#
# The lookup API is deliberately a THREE-step exchange: describe (here is my
# vocabulary), submit (here is what I found — staged, not applied), apply (a
# human confirms). An agent that browses the web will sometimes be confidently
# wrong, and a wrong resistance that lands silently is a part someone solders.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }

cleanup(){
    pg "DELETE FROM part_parameter WHERE product_id IN (SELECT id FROM product_product WHERE name LIKE 'QA-LK%')" >/dev/null
    pg "DELETE FROM part_manufacturer_info WHERE product_id IN (SELECT id FROM product_product WHERE name LIKE 'QA-LK%')" >/dev/null
    pg "DELETE FROM part_lookup_result WHERE query LIKE 'QA-LK%'" >/dev/null
    pg "DELETE FROM product_product WHERE name LIKE 'QA-LK%'" >/dev/null
    pg "DELETE FROM product_template WHERE name LIKE 'QA-LK%'" >/dev/null
    pg "DELETE FROM res_partner WHERE name='QA-LK Semiconductor'" >/dev/null
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

echo "############ the unit vocabulary exists ############"
NU=$(pg "SELECT count(*) FROM part_unit WHERE quantity_kind IS NOT NULL")
[ -n "$NU" ] && [ "$NU" -ge 30 ] && ok "$NU units seeded with a quantity and a factor" || no "only $NU units"
for sym in 'Ω' 'kΩ' 'F' 'µF' 'nF' 'pF' 'H' 'µH' 'V' 'mA' 'W' 'Hz' 'MHz' '°C' '%'; do
    [ "$(pg "SELECT count(*) FROM part_unit WHERE symbol='$sym'")" = "1" ] || { no "unit $sym missing"; MISS=1; }
done
[ -z "$MISS" ] && ok "the everyday electronics symbols are all present"
# The factor is what makes cross-prefix comparison work at all.
[ "$(pg "SELECT factor FROM part_unit WHERE symbol='kΩ'")" = "1000" ] && ok "kΩ has factor 1000" || no "kΩ factor wrong"
[ "$(pg "SELECT factor::text FROM part_unit WHERE symbol='nF'")" = "1e-09" ] && ok "nF has factor 1e-9" || no "nF factor: $(pg "SELECT factor::text FROM part_unit WHERE symbol='nF'")"
[ "$(pg "SELECT quantity_kind FROM part_unit WHERE symbol='µF'")" = "capacitance" ] && ok "µF is capacitance" || no "µF quantity wrong"

echo "############ existing parameters were normalised, not left behind ############"
[ "$(pg "SELECT count(*) FROM part_parameter WHERE value_base IS NULL")" = "0" ] \
    && ok "every existing parameter has a base value" || no "some parameters have no value_base"

echo "############ fixture: the same resistance written three ways ############"
mkprod(){ call product.product create "[{\"name\":\"$1\",\"type\":\"product\"}]" | rid; }
P1=$(mkprod "QA-LK R 4k7 in kohm")
P2=$(mkprod "QA-LK R 4700 in ohm")
P3=$(mkprod "QA-LK C 100n")
UK=$(pg "SELECT id FROM part_unit WHERE symbol='kΩ'")
UO=$(pg "SELECT id FROM part_unit WHERE symbol='Ω'")
UN=$(pg "SELECT id FROM part_unit WHERE symbol='nF'")
call part.parameter create "[{\"product_id\":$P1,\"name\":\"QA-LK Resistance\",\"value_numeric\":4.7,\"unit_id\":$UK}]" >/dev/null
call part.parameter create "[{\"product_id\":$P2,\"name\":\"QA-LK Resistance\",\"value_numeric\":4700,\"unit_id\":$UO}]" >/dev/null
call part.parameter create "[{\"product_id\":$P3,\"name\":\"QA-LK Capacitance\",\"value_numeric\":100,\"unit_id\":$UN}]" >/dev/null
B1=$(pg "SELECT value_base FROM part_parameter WHERE product_id=$P1")
B2=$(pg "SELECT value_base FROM part_parameter WHERE product_id=$P2")
[ "$B1" = "4700" ] && ok "4.7 kΩ normalises to 4700" || no "4.7kΩ -> $B1"
[ "$B2" = "4700" ] && ok "4700 Ω normalises to 4700" || no "4700Ω -> $B2"
[ "$B1" = "$B2" ] && ok "both spellings land on the same base value" || no "they differ: $B1 vs $B2"

echo "############ a range finds both, whichever unit they were entered in ############"
R=$(call part.parameter search_parts '[{"name":"QA-LK Resistance","min":"4k","max":"5k","unit":"Ω"}]')
echo "$R" | grep -q 'QA-LK R 4k7 in kohm' && ok "the kΩ part is found by a 4k–5k range" || no "kΩ part missed: $R"
echo "$R" | grep -q 'QA-LK R 4700 in ohm' && ok "the Ω part is found by the same range"  || no "Ω part missed"

echo "############ SI shorthand is understood ############"
# 4k7 is how it is written on a schematic and in every BOM.
R=$(call part.parameter search_parts '[{"name":"QA-LK Resistance","min":"4k7","max":"4k7","unit":"Ω"}]')
[ "$(echo "$R" | grep -o 'QA-LK R' | wc -l)" = "2" ] && ok "\"4k7\" matches both parts exactly" || no "4k7 matched $(echo "$R" | grep -o 'QA-LK R' | wc -l)"
R=$(call part.parameter search_parts '[{"name":"QA-LK Capacitance","min":"90n","max":"110n","unit":"F"}]')
echo "$R" | grep -q 'QA-LK C 100n' && ok "\"90n–110n\" finds the 100 nF part" || no "capacitance shorthand failed: $R"

echo "############ a quantity cannot match across kinds ############"
# 4700 Ω and 4700 F share a number; the unit must keep them apart.
R=$(call part.parameter search_parts '[{"min":"1p","max":"1M","unit":"F"}]')
echo "$R" | grep -q 'QA-LK R 4' && no "LEAK: a resistance matched a capacitance range" \
                                || ok "a capacitance range excludes resistances"
call part.parameter search_parts '[{"name":"QA-LK Resistance","min":"1","unit":"nope"}]' \
    | grep -q 'Unknown unit' && ok "an unknown unit is refused" || no "unknown unit accepted"

echo "############ the lookup contract: describe ############"
D=$(call part.lookup describe '[{}]')
for k in categories units known_parameters footprints value_formats schema_version; do
    echo "$D" | grep -q "\"$k\"" && ok "describe exposes $k" || no "describe missing $k"
done
echo "$D" | grep -q '"path":"All / Electronics / Passives / Resistors' \
    && ok "the category tree is given as full paths" || no "category paths missing"
echo "$D" | grep -q '"factor_to_base"' && ok "units carry their factor to base" || no "no factor in describe"

echo "############ submit stages, it does not apply ############"
SUB='[{"query":"QA-LK 4k7 0805 resistor","mpn":"QA-LK-4K7","manufacturer":"QA-LK Semiconductor",
       "name":"QA-LK Resistor 4.7k 0805","category_path":"All / Electronics / Passives / Resistors / SMD Resistors",
       "source":"https://example.invalid/ds.pdf","confidence":0.9,
       "parameters":[{"name":"Resistance","value":"4k7","unit":"Ω"},
                     {"name":"Tolerance","value":"1","unit":"%"},
                     {"name":"Power","value":"125m","unit":"W"}]}]'
R=$(call part.lookup submit "$SUB")
LID=$(echo "$R" | sed -n 's/.*"id":\([0-9]*\).*/\1/p' | head -1)
echo "$R" | grep -q '"ok":true' && ok "a well-formed result is accepted ($LID)" || no "submit failed: $R"
[ "$(pg "SELECT state FROM part_lookup_result WHERE id=$LID")" = "pending" ] \
    && ok "it is staged as pending" || no "wrong state"
[ "$(pg "SELECT count(*) FROM product_product WHERE name LIKE 'QA-LK Resistor%'")" = "0" ] \
    && ok "and NOTHING was written to the catalogue yet" || no "LEAK: submit created a product"

echo "############ submit reports what it distrusts ############"
BAD='[{"query":"QA-LK bad","parameters":[{"name":"Resistance","value":"4k7","unit":"furlongs"},
                                          {"name":"","value":"1","unit":"Ω"}]}]'
R=$(call part.lookup submit "$BAD")
echo "$R" | grep -q 'Unknown unit' && ok "an unknown unit is reported as an issue" || no "bad unit not caught: $R"
echo "$R" | grep -q 'Parameter name is required' && ok "a nameless parameter is reported" || no "nameless param not caught"
echo "$R" | grep -q '"ok":false' && ok "and the result is marked not-ok" || no "bad result reported ok"
BID=$(echo "$R" | sed -n 's/.*"id":\([0-9]*\).*/\1/p' | head -1)
[ "$(pg "SELECT state FROM part_lookup_result WHERE id=$BID")" = "invalid" ] \
    && ok "it is staged as invalid rather than thrown away" || no "invalid result not staged"

echo "############ apply writes the catalogue, once ############"
R=$(call part.lookup apply "[{\"id\":$LID}]")
NEWP=$(echo "$R" | sed -n 's/.*"product_id":\([0-9]*\).*/\1/p')
[ -n "$NEWP" ] && ok "apply created product $NEWP" || no "apply failed: $R"
[ "$(pg "SELECT count(*) FROM part_parameter WHERE product_id=$NEWP")" = "3" ] \
    && ok "its three parameters were written" || no "parameter count wrong"
# 4k7 with unit Ω must land as 4700 in base, same as everything else.
[ "$(pg "SELECT value_base FROM part_parameter WHERE product_id=$NEWP AND name='Resistance'")" = "4700" ] \
    && ok "the agent's \"4k7\" normalised to 4700" || no "applied resistance base wrong"
[ "$(pg "SELECT value_base::text FROM part_parameter WHERE product_id=$NEWP AND name='Power'")" = "0.125" ] \
    && ok "\"125m\" W normalised to 0.125" || no "power base: $(pg "SELECT value_base::text FROM part_parameter WHERE product_id=$NEWP AND name='Power'")"
[ "$(pg "SELECT c.name FROM product_product p JOIN product_category c ON c.id=p.categ_id WHERE p.id=$NEWP")" = "SMDResistors" ] \
    && ok "the proposed category path resolved" || no "category not applied"
[ "$(pg "SELECT count(*) FROM part_manufacturer_info WHERE product_id=$NEWP")" = "1" ] \
    && ok "the manufacturer part number was recorded" || no "no MPN row"
[ "$(pg "SELECT state FROM part_lookup_result WHERE id=$LID")" = "applied" ] \
    && ok "the result is marked applied" || no "state not updated"
call part.lookup apply "[{\"id\":$LID}]" | grep -q 'already been applied' \
    && ok "applying twice is refused" || no "double apply allowed"

echo "############ the applied part is now findable by parametric search ############"
R=$(call part.parameter search_parts '[{"name":"Resistance","min":"4k","max":"5k","unit":"Ω"}]')
echo "$R" | grep -q "\"id\":$NEWP" && ok "the new part appears in a 4k–5k search" || no "applied part not searchable"

echo "############ reject leaves the catalogue alone ############"
R=$(call part.lookup submit '[{"query":"QA-LK junk","mpn":"QA-LK-JUNK"}]')
JID=$(echo "$R" | sed -n 's/.*"id":\([0-9]*\).*/\1/p' | head -1)
call part.lookup reject "[[$JID]]" >/dev/null
[ "$(pg "SELECT state FROM part_lookup_result WHERE id=$JID")" = "rejected" ] \
    && ok "a rejected result is marked, not deleted" || no "reject failed"
[ "$(pg "SELECT count(*) FROM product_product WHERE name LIKE 'QA-LK junk%'")" = "0" ] \
    && ok "and no product came from it" || no "rejected result created a product"

if [ -n "$FAILED" ]; then echo; echo "*** FAILURES ***"; exit 1; fi
echo; echo "  All checks passed."

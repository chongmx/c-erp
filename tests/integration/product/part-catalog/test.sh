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
# Faceted parts catalogue — part.catalog (docs/098).
#
# The load-bearing behaviour here is FACET COUNTING, and it is the thing that
# is easy to get quietly wrong. A faceted browser has three rules:
#
#   within one facet   the selected values are OR'd   (0402 or 0603)
#   across facets      they are AND'd                 (0402 AND YAGEO)
#   a facet's counts   are computed with ITS OWN selection removed
#
# That third rule is the subtle one. Count a facet with its own clause applied
# and every unselected value collapses to zero — the user picks 0402 and the
# Package facet reports that no 0603 exists, so multi-select becomes impossible.
# Several checks below exist purely to pin that down.
#
# The other thing worth proving is that the strip and the table can never
# disagree: `facets` and `search` share one filter parser, so the count in the
# header is the count of the rows underneath it.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }

CAT=$(pg "SELECT id FROM product_category WHERE name='SMD Resistors' LIMIT 1")

cleanup(){
    pg "DELETE FROM product_product WHERE default_code LIKE 'QA-CAT-%'" >/dev/null
    pg "DELETE FROM res_partner WHERE name IN ('QA-CAT Alpha','QA-CAT Beta')" >/dev/null
    pg "DELETE FROM part_footprint WHERE name='QA-CAT-PKG'" >/dev/null
}
cleanup; trap cleanup EXIT

# ---- fixtures: six parts with a known, hand-checkable facet distribution ----
# Alpha/0402 x2, Alpha/0603 x1, Beta/0402 x1, Beta/0603 x2 — so every count in
# the assertions below can be verified by reading this table, not by trusting
# the code under test.
pg "INSERT INTO res_partner (name, active) VALUES ('QA-CAT Alpha',true),('QA-CAT Beta',true)" >/dev/null
UOM=$(pg "SELECT id FROM uom_uom ORDER BY id LIMIT 1")
UOHM=$(pg "SELECT id FROM part_unit WHERE symbol='Ω'")
F402=$(pg "SELECT id FROM part_footprint WHERE name='0402'")
F603=$(pg "SELECT id FROM part_footprint WHERE name='0603'")
A=$(pg "SELECT id FROM res_partner WHERE name='QA-CAT Alpha'")
B=$(pg "SELECT id FROM res_partner WHERE name='QA-CAT Beta'")

mk(){ # name code footprint mfr ohms type qty_micros
  local pid
  # head -1: psql prints the RETURNING row *and* the "INSERT 0 1" command tag,
  # and a two-line id silently corrupts every statement built from it.
  pid=$(pg "INSERT INTO product_product (name, default_code, type, categ_id, uom_id, uom_po_id,
             list_price, standard_price, qty_available, footprint_id, active, sale_ok, purchase_ok, company_id)
            VALUES ('$1','$2','product',$CAT,$UOM,$UOM,1000,500,$7,$3,true,true,true,NULL) RETURNING id" | head -1)
  pg "INSERT INTO part_parameter (product_id,name,value_numeric,unit_id,value_text,value_base,quantity_kind)
      VALUES ($pid,'Resistance',$5,$UOHM,'$5Ω',$5,'resistance'),
             ($pid,'Type',0,NULL,'$6',NULL,NULL)" >/dev/null
  pg "INSERT INTO part_manufacturer_info (product_id,manufacturer_id,part_number)
      VALUES ($pid,$4,'MPN-$2')" >/dev/null
}
mk 'QA-CAT r1' 'QA-CAT-1' "$F402" "$A" 100    'Thick Film' 5000000
mk 'QA-CAT r2' 'QA-CAT-2' "$F402" "$A" 1000   'Thick Film' 5000000
mk 'QA-CAT r3' 'QA-CAT-3' "$F603" "$A" 10000  'Thin Film'  5000000
mk 'QA-CAT r4' 'QA-CAT-4' "$F402" "$B" 100000 'Thick Film' 5000000
mk 'QA-CAT r5' 'QA-CAT-5' "$F603" "$B" 4700   'Thin Film'  0
mk 'QA-CAT r6' 'QA-CAT-6' "$F603" "$B" 47000  'Thin Film'  0

SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; echo "*** FAILURES ***"; exit 1; }
CTX="\"context\":{\"session_id\":\"$SID\"}"
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"part.catalog\",\"method\":\"$1\",\"args\":[$2],\"kwargs\":{$CTX}}}"; }

# A tiny python helper keeps the assertions readable — grepping nested JSON with
# sed is how the false failures in docs/097 happened.
py(){ python3 -c "$1" 2>/dev/null; }
export PYTHONIOENCODING=utf-8

Q_ALL="{\"q\":\"QA-CAT\"}"

echo "--- shape ---"
R=$(call facets "$Q_ALL")
echo "$R" | grep -q '"total"' && ok "facets returns a total" || no "facets returns a total"
TOT=$(echo "$R" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(d['total'])")
[ "$TOT" = "6" ] && ok "total counts the 6 fixtures ($TOT)" || no "total is $TOT, expected 6"

KEYS=$(echo "$R" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(','.join(f['key'] for f in d['facets']))")
echo "$KEYS" | grep -q 'mfr'             && ok "manufacturer facet present" || no "manufacturer facet missing ($KEYS)"
echo "$KEYS" | grep -q 'pkg'             && ok "package facet present"      || no "package facet missing ($KEYS)"
echo "$KEYS" | grep -q 'param:Resistance'&& ok "Resistance facet present"   || no "Resistance facet missing ($KEYS)"
echo "$KEYS" | grep -q 'param:Type'      && ok "Type facet present"         || no "Type facet missing ($KEYS)"

# Classification: a unitless text attribute must NOT become a numeric range.
# seedPartUnits_ backfills value_base=0 for unitless rows, so a naive
# "has numbers?" test would call Type numeric and render a min/max box.
KIND=$(echo "$R" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(next(f['kind'] for f in d['facets'] if f['key']=='param:Type'))")
[ "$KIND" = "enum" ] && ok "Type classified as enum, not range" || no "Type classified as $KIND"
KIND=$(echo "$R" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(next(f['kind'] for f in d['facets'] if f['key']=='param:Resistance'))")
[ "$KIND" = "range" ] && ok "Resistance classified as range" || no "Resistance classified as $KIND"

# A range facet must carry the units of its own quantity kind, and only those.
U=$(echo "$R" | py "
import json,sys
d=json.load(sys.stdin)['result']
f=next(f for f in d['facets'] if f['key']=='param:Resistance')
print(','.join(u['symbol'] for u in f['units']))")
echo "$U" | grep -q 'kΩ' && ok "Resistance offers kΩ"        || no "Resistance units: $U"
echo "$U" | grep -q 'F'  && no "Resistance offers farads ($U)" || ok "Resistance offers no farads"

echo "--- unfiltered counts ---"
cnt(){ # facet-key value  -> count from the facets response in $1
  echo "$2" | py "
import json,sys
d=json.load(sys.stdin)['result']
f=next((f for f in d['facets'] if f['key']=='$3'),None)
print(next((v['n'] for v in (f or {}).get('values',[]) if v['v']=='$4'),0))"
}
A402=$(cnt x "$R" pkg 0402); A603=$(cnt x "$R" pkg 0603)
[ "$A402" = "3" ] && ok "0402 counts 3" || no "0402 counts $A402, expected 3"
[ "$A603" = "3" ] && ok "0603 counts 3" || no "0603 counts $A603, expected 3"
MA=$(cnt x "$R" mfr 'QA-CAT Alpha'); MB=$(cnt x "$R" mfr 'QA-CAT Beta')
[ "$MA" = "3" ] && ok "Alpha counts 3" || no "Alpha counts $MA, expected 3"
[ "$MB" = "3" ] && ok "Beta counts 3"  || no "Beta counts $MB, expected 3"

echo "--- the self-exclusion rule ---"
# Select 0402. The PACKAGE facet must still report 0603 = 3, because a facet
# counts with its own selection removed. If this returns 0, multi-select is
# broken: the user could never add 0603 to their 0402 selection.
R2=$(call facets "{\"q\":\"QA-CAT\",\"enum\":{\"pkg\":[\"0402\"]}}")
S603=$(cnt x "$R2" pkg 0603)
[ "$S603" = "3" ] && ok "0603 still counts 3 while 0402 is selected" \
                  || no "0603 counts $S603 while 0402 selected, expected 3 (self-exclusion broken)"
S402=$(cnt x "$R2" pkg 0402)
[ "$S402" = "3" ] && ok "0402 still counts 3 (its own value unaffected)" || no "0402 counts $S402"

# ...but OTHER facets must narrow. Alpha has 2 of the 3 0402 parts.
SMA=$(cnt x "$R2" mfr 'QA-CAT Alpha'); SMB=$(cnt x "$R2" mfr 'QA-CAT Beta')
[ "$SMA" = "2" ] && ok "Alpha narrows to 2 under 0402" || no "Alpha is $SMA under 0402, expected 2"
[ "$SMB" = "1" ] && ok "Beta narrows to 1 under 0402"  || no "Beta is $SMB under 0402, expected 1"
T2=$(echo "$R2" | py "
import json,sys
print(json.load(sys.stdin)['result']['total'])")
[ "$T2" = "3" ] && ok "total narrows to 3 under 0402" || no "total is $T2 under 0402, expected 3"

echo "--- OR within a facet, AND across facets ---"
R3=$(call facets "{\"q\":\"QA-CAT\",\"enum\":{\"pkg\":[\"0402\",\"0603\"]}}")
T3=$(echo "$R3" | py "
import json,sys
print(json.load(sys.stdin)['result']['total'])")
[ "$T3" = "6" ] && ok "0402 OR 0603 gives all 6" || no "0402 OR 0603 gives $T3, expected 6"

R4=$(call facets "{\"q\":\"QA-CAT\",\"enum\":{\"pkg\":[\"0402\"],\"mfr\":[\"QA-CAT Alpha\"]}}")
T4=$(echo "$R4" | py "
import json,sys
print(json.load(sys.stdin)['result']['total'])")
[ "$T4" = "2" ] && ok "0402 AND Alpha gives 2" || no "0402 AND Alpha gives $T4, expected 2"

echo "--- numeric ranges ---"
tot(){ echo "$1" | py "
import json,sys
print(json.load(sys.stdin)['result']['total'])"; }
T=$(tot "$(call search "{\"q\":\"QA-CAT\",\"range\":{\"param:Resistance\":{\"min\":1,\"max\":10,\"unit\":\"kΩ\"}}}")")
[ "$T" = "3" ] && ok "1k–10kΩ finds 3 (1k, 4k7, 10k)" || no "1k–10kΩ finds $T, expected 3"

# The same span written in base ohms must give the same answer.
T=$(tot "$(call search "{\"q\":\"QA-CAT\",\"range\":{\"param:Resistance\":{\"min\":1000,\"max\":10000,\"unit\":\"Ω\"}}}")")
[ "$T" = "3" ] && ok "1000–10000Ω agrees with 1k–10kΩ" || no "1000–10000Ω finds $T, expected 3"

# ...and written in R-notation.
T=$(tot "$(call search "{\"q\":\"QA-CAT\",\"range\":{\"param:Resistance\":{\"min\":\"1k\",\"max\":\"10k\",\"unit\":\"Ω\"}}}")")
[ "$T" = "3" ] && ok "\"1k\"–\"10k\" text bounds agree" || no "text bounds find $T, expected 3"

# EMBEDDED-MULTIPLIER notation — 4k7 meaning 4.7k, which is how it is written
# on a schematic and in most vendor BOMs.
#
# This assertion moved here from the Parametric Search screen, which was
# removed as a strict subset of this one. The capability outlived the screen,
# so it needs a test that does not depend on the screen existing — otherwise
# deleting a redundant page silently deletes the coverage of a feature that is
# still there.
T=$(tot "$(call search "{\"q\":\"QA-CAT\",\"range\":{\"param:Resistance\":{\"min\":\"4k7\",\"max\":\"4k7\",\"unit\":\"Ω\"}}}")")
[ "$T" = "1" ] && ok "\"4k7\" resolves to 4700 and matches exactly one part" \
                || no "\"4k7\" exact match found $T, expected 1"
# And the same value spelled two other ways must find that same one part.
T=$(tot "$(call search "{\"q\":\"QA-CAT\",\"range\":{\"param:Resistance\":{\"min\":\"4.7k\",\"max\":\"4.7k\",\"unit\":\"Ω\"}}}")")
[ "$T" = "1" ] && ok "\"4.7k\" agrees with \"4k7\"" || no "\"4.7k\" found $T, expected 1"
T=$(tot "$(call search "{\"q\":\"QA-CAT\",\"range\":{\"param:Resistance\":{\"min\":4700,\"max\":4700,\"unit\":\"Ω\"}}}")")
[ "$T" = "1" ] && ok "and plain 4700 Ω agrees with both" || no "4700 Ω found $T, expected 1"

# Fixtures are 100, 1k, 4k7, 10k, 47k, 100kΩ — only the 100k is at or above 50k.
T=$(tot "$(call search "{\"q\":\"QA-CAT\",\"range\":{\"param:Resistance\":{\"min\":50,\"unit\":\"kΩ\"}}}")")
[ "$T" = "1" ] && ok "min-only 50kΩ finds just the 100kΩ part" || no "min-only finds $T, expected 1"
T=$(tot "$(call search "{\"q\":\"QA-CAT\",\"range\":{\"param:Resistance\":{\"min\":10,\"unit\":\"MΩ\"}}}")")
[ "$T" = "0" ] && ok "min-only above every fixture finds 0" || no "min-only 10MΩ finds $T, expected 0"
T=$(tot "$(call search "{\"q\":\"QA-CAT\",\"range\":{\"param:Resistance\":{\"max\":200,\"unit\":\"Ω\"}}}")")
[ "$T" = "1" ] && ok "max-only 200Ω finds just the 100Ω part" || no "max-only finds $T, expected 1"

# A resistance range must never match on a capacitance that shares a number:
# the quantity kind is part of the predicate, not an optimisation.
T=$(tot "$(call search "{\"q\":\"QA-CAT\",\"range\":{\"param:Resistance\":{\"min\":1,\"max\":10,\"unit\":\"µF\"}}}")")
[ "$T" = "0" ] && ok "a farad range matches no resistance" || no "farad range matched $T resistances"

call search "{\"q\":\"QA-CAT\",\"range\":{\"param:Resistance\":{\"min\":1,\"unit\":\"furlongs\"}}}" \
  | grep -q 'Unknown unit' && ok "unknown unit is rejected" || no "unknown unit was not rejected"

echo "--- strip and table agree ---"
FT=$(echo "$(call facets "{\"q\":\"QA-CAT\",\"enum\":{\"pkg\":[\"0402\"]}}")" | py "
import json,sys
print(json.load(sys.stdin)['result']['total'])")
ST=$(tot "$(call search "{\"q\":\"QA-CAT\",\"enum\":{\"pkg\":[\"0402\"]}}")")
[ "$FT" = "$ST" ] && ok "facets total == search total ($FT)" || no "facets says $FT, search says $ST"

echo "--- rows ---"
R5=$(call search "{\"q\":\"QA-CAT\",\"sort\":\"name\",\"dir\":\"asc\",\"limit\":3}")
N=$(echo "$R5" | py "
import json,sys
print(len(json.load(sys.stdin)['result']['rows']))")
[ "$N" = "3" ] && ok "limit 3 returns 3 rows" || no "limit 3 returned $N rows"
FIRST=$(echo "$R5" | py "
import json,sys
r=json.load(sys.stdin)['result']['rows'][0]
print(r['name'],'|',r['mpn'],'|',r['package'],'|',r['manufacturer'])")
echo "$FIRST" | grep -q 'QA-CAT r1'   && ok "rows sort by name asc"      || no "first row: $FIRST"
echo "$FIRST" | grep -q 'MPN-QA-CAT-1'&& ok "row carries its MPN"        || no "no MPN: $FIRST"
echo "$FIRST" | grep -q '0402'        && ok "row carries its package"    || no "no package: $FIRST"
echo "$FIRST" | grep -q 'QA-CAT Alpha'&& ok "row carries its manufacturer" || no "no manufacturer: $FIRST"

# Prices and quantities are bigint micros in the column; the wire must carry
# real numbers, not 1000000x of them.
PRICE=$(echo "$R5" | py "
import json,sys
print(json.load(sys.stdin)['result']['rows'][0]['list_price'])")
[ "$PRICE" = "0.001" ] && ok "list_price descaled from micros ($PRICE)" || no "list_price is $PRICE, expected 0.001"

PARAMS=$(echo "$R5" | py "
import json,sys
print(','.join(p['name'] for p in json.load(sys.stdin)['result']['rows'][0]['params']))")
echo "$PARAMS" | grep -q 'Resistance' && ok "row carries its parameters" || no "row params: $PARAMS"

echo "--- paging and sort ---"
P1=$(call search "{\"q\":\"QA-CAT\",\"sort\":\"name\",\"limit\":2,\"offset\":0}")
P2=$(call search "{\"q\":\"QA-CAT\",\"sort\":\"name\",\"limit\":2,\"offset\":2}")
N1=$(echo "$P1" | py "
import json,sys
print(json.load(sys.stdin)['result']['rows'][0]['code'])")
N2=$(echo "$P2" | py "
import json,sys
print(json.load(sys.stdin)['result']['rows'][0]['code'])")
[ "$N1" != "$N2" ] && ok "offset moves the window ($N1 -> $N2)" || no "offset returned the same row ($N1)"
TP=$(tot "$P2")
[ "$TP" = "6" ] && ok "total ignores the page window" || no "paged total is $TP, expected 6"

D=$(call search "{\"q\":\"QA-CAT\",\"sort\":\"name\",\"dir\":\"desc\",\"limit\":1}")
DN=$(echo "$D" | py "
import json,sys
print(json.load(sys.stdin)['result']['rows'][0]['name'])")
echo "$DN" | grep -q 'r6' && ok "dir=desc reverses the order" || no "desc first row: $DN"

# An unknown sort key must fall back, never reach SQL (S-49).
BAD=$(call search "{\"q\":\"QA-CAT\",\"sort\":\"name; DROP TABLE product_product\",\"limit\":1}")
echo "$BAD" | grep -q '"rows"' && ok "unknown sort key falls back safely" || no "unknown sort key broke the query"
[ "$(pg "SELECT count(*) FROM product_product WHERE default_code LIKE 'QA-CAT-%'")" = "6" ] \
  && ok "fixtures survived the injection attempt" || no "fixtures are gone"

echo "--- unknown facet keys are dropped, not interpolated ---"
BADK=$(call facets "{\"q\":\"QA-CAT\",\"enum\":{\"password\":[\"x\"]}}")
TB=$(echo "$BADK" | py "
import json,sys
print(json.load(sys.stdin)['result']['total'])")
[ "$TB" = "6" ] && ok "an unregistered facet key is ignored" || no "unregistered key changed the result ($TB)"

echo "--- in-stock flag ---"
T=$(tot "$(call search "{\"q\":\"QA-CAT\",\"in_stock\":true}")")
[ "$T" = "4" ] && ok "in_stock excludes the 2 zero-quantity parts" || no "in_stock gives $T, expected 4"

echo "--- text search ---"
T=$(tot "$(call search "{\"q\":\"MPN-QA-CAT-3\"}")")
[ "$T" = "1" ] && ok "search matches on manufacturer part number" || no "MPN search gives $T, expected 1"

echo "--- categories ---"
C=$(call categories "")
echo "$C" | grep -q 'SMD Resistors' && ok "categories lists the parts category" || no "categories missing SMD Resistors"
DEPTH=$(echo "$C" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(max(c['depth'] for c in d))")
[ -n "$DEPTH" ] && [ "$DEPTH" -ge 2 ] && ok "categories carry tree depth ($DEPTH)" || no "category depth is $DEPTH"
ROLL=$(echo "$C" | py "
import json,sys
d=json.load(sys.stdin)['result']
p=next((c for c in d if c['path'].endswith('Resistors')and'SMD'not in c['path']),None)
s=next((c for c in d if c['path'].endswith('SMD Resistors')),None)
print('yes' if p and s and p['n']>=s['n'] else 'no')")
[ "$ROLL" = "yes" ] && ok "parent count rolls up its children" || no "parent count does not include children"

echo "--- category scope includes the subtree ---"
PARENT=$(pg "SELECT id FROM product_category WHERE name='Resistors' LIMIT 1")
if [ -n "$PARENT" ]; then
  T=$(tot "$(call search "{\"q\":\"QA-CAT\",\"categ_id\":$PARENT}")")
  [ "$T" = "6" ] && ok "filtering by the parent finds child-category parts" \
                 || no "parent category finds $T, expected 6"
else
  no "no Resistors category to scope by"
fi

echo "--- demo catalogue is present and facetable ---"
DEMO=$(pg "SELECT count(*) FROM product_product WHERE default_code LIKE 'DP-%'")
if [ "${DEMO:-0}" -gt 100 ]; then
  ok "demo catalogue seeded ($DEMO parts)"
  R6=$(call facets "{}")
  NF=$(echo "$R6" | py "
import json,sys
print(len(json.load(sys.stdin)['result']['facets']))")
  [ -n "$NF" ] && [ "$NF" -ge 5 ] && ok "unfiltered catalogue offers $NF facets" || no "only $NF facets"
  T=$(tot "$(call search "{\"range\":{\"param:Resistance\":{\"min\":1,\"max\":10,\"unit\":\"kΩ\"}}}")")
  [ -n "$T" ] && [ "$T" -gt 0 ] && ok "1k–10kΩ finds $T demo resistors" || no "no demo resistors in 1k–10k"
  # Mixed notation is the point: both 4k7 and 4.7k must land in the same range.
  MIX=$(pg "SELECT count(DISTINCT value_text) FROM part_parameter WHERE name='Resistance' AND value_text LIKE '%k%'")
  [ -n "$MIX" ] && [ "$MIX" -gt 1 ] && ok "demo data mixes notations ($MIX distinct)" || no "demo notation not mixed"
else
  echo "    NOTE  demo catalogue not seeded (run ./scripts/seed/parts.sh) — skipping 4 checks"
fi

[ -z "$FAILED" ] && echo "  All checks passed." || echo "  *** FAILURES ***"

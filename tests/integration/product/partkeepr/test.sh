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
# PartKeepr PK2-PK4 — footprints, parametric parameters + SI units, MPNs.
#
# Proves through the real HTTP path:
#   * a product carries a footprint (PK2)
#   * a product has parametric specs with a unit (PK3), and parametric SEARCH
#     finds parts whose parameter is in a numeric range
#   * a product lists manufacturer part numbers (PK4)
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/pk_cookie.txt
cat > /tmp/pk_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/pk_auth.json > /tmp/pk_auth_out.json
grep -q '"session_id"' /tmp/pk_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
rval() { python3 -c "import json,sys; print(json.load(sys.stdin).get('result',''))" 2>/dev/null; }
mkprod() { pg "INSERT INTO product_product (name,type,uom_id,uom_po_id,active) VALUES ('$1','product',1,1,true) RETURNING id"; }
MANUF=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")

echo "############ PK2 — footprint ############"
# The fixture owns its own name. part_footprint.name is unique and docs/098
# seeds the standard package vocabulary (SOIC-8 among them), so a test that
# mints a real package name collides with the seed — and an empty $FP then
# corrupts every JSON body built from it. Same lesson as the part_unit(symbol)
# collision: the schema is right, the test must not squat on public names.
FP=$(callkw part.footprint create "[{\"name\":\"PKTEST-SOIC-8\",\"description\":\"8-pin small outline\"}]" | rval)
P=$(callkw product.product create "[{\"name\":\"PKTEST-Chip\",\"type\":\"product\",\"footprint_id\":$FP}]" | rval)
FPBACK=$(pg "SELECT footprint_id FROM product_product WHERE id=$P")
echo "    footprint=$FP  product=$P  product.footprint_id=$FPBACK"
[ -n "$FP" ] && ok "footprint created" || no "footprint create failed"
[ "$FPBACK" = "$FP" ] && ok "product carries the footprint" || no "footprint_id=$FPBACK"

echo
echo "############ PK3 — parameters + units + parametric search ############"
# docs/097: Ω is now a SEEDED unit and `symbol` is unique, so this no longer
# creates its own. That uniqueness is the point — two units both meaning ohms
# would let two parts hold the same resistance and never compare equal.
OHM=$(pg "SELECT id FROM part_unit WHERE symbol='Ω'")
callkw part.parameter create "[{\"product_id\":$P,\"name\":\"Resistance\",\"value_numeric\":4700,\"unit_id\":$OHM}]" >/dev/null
PARR=$(callkw part.parameter search_read "[[[\"product_id\",\"=\",$P]]]")
echo "    unit=$OHM  params -> $(printf '%s' "$PARR" | head -c 140)"
[ -n "$OHM" ] && ok "the seeded Ohm unit is available" || no "no seeded Ohm unit"
printf '%s' "$PARR" | grep -q '"name":"Resistance"' && ok "product has a Resistance parameter" || no "no parameter"
printf '%s' "$PARR" | grep -q '"value_numeric":4700' && ok "parameter value stored (4700)" || no "value not 4700"

# Three resistors at 100 / 4700 / 10000 Ω, then search 1k..5k -> only 4700.
RA=$(mkprod 'PKTEST-R100');  callkw part.parameter create "[{\"product_id\":$RA,\"name\":\"Resistance\",\"value_numeric\":100,\"unit_id\":$OHM}]"   >/dev/null
RB=$(mkprod 'PKTEST-R4700'); callkw part.parameter create "[{\"product_id\":$RB,\"name\":\"Resistance\",\"value_numeric\":4700,\"unit_id\":$OHM}]"  >/dev/null
RC=$(mkprod 'PKTEST-R10k');  callkw part.parameter create "[{\"product_id\":$RC,\"name\":\"Resistance\",\"value_numeric\":10000,\"unit_id\":$OHM}]" >/dev/null
SR=$(callkw part.parameter search_parts "[{\"name\":\"Resistance\",\"min\":1000,\"max\":5000}]")
IDS=$(printf '%s' "$SR" | python3 -c "import json,sys; print(sorted(x['id'] for x in json.load(sys.stdin)['result']))" 2>/dev/null)
echo "    search Resistance in [1000,5000] -> ids $IDS  (want just $P and $RB)"
printf '%s' "$SR" | grep -q "\"id\":$RB" && ok "parametric search found the 4700 part" || no "4700 part not found"
printf '%s' "$SR" | grep -q "\"id\":$RA" && no "100 part wrongly matched" || ok "100 (out of range) excluded"
printf '%s' "$SR" | grep -q "\"id\":$RC" && no "10000 part wrongly matched" || ok "10000 (out of range) excluded"

echo
echo "############ PK4 — manufacturer part numbers ############"
MPN=$(callkw part.manufacturer.info create "[{\"product_id\":$P,\"manufacturer_id\":$MANUF,\"part_number\":\"ABC-123\",\"notes\":\"preferred\"}]" | rval)
MRR=$(callkw part.manufacturer.info search_read "[[[\"product_id\",\"=\",$P]]]")
echo "    mpn=$MPN  -> $(printf '%s' "$MRR" | head -c 120)"
[ -n "$MPN" ] && ok "MPN line created" || no "MPN create failed"
printf '%s' "$MRR" | grep -q '"part_number":"ABC-123"' && ok "MPN reads back on the product" || no "MPN missing"

echo
echo "############ cleanup ############"
pg "DELETE FROM part_parameter WHERE product_id IN ($P,$RA,$RB,$RC)" >/dev/null
pg "DELETE FROM part_manufacturer_info WHERE product_id=$P" >/dev/null
pg "DELETE FROM product_product WHERE id IN ($P,$RA,$RB,$RC)" >/dev/null
pg "DELETE FROM part_footprint WHERE id=$FP" >/dev/null
rm -f "$CK" /tmp/pk_auth.json /tmp/pk_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

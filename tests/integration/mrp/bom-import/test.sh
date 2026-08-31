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
# BOM editor and importer (docs/107).
#
# The rule this whole feature is built on: the importer NEVER decides what a
# line is. It resolves candidates from the catalogue, reports what it found, and
# a person commits. Same rule as part.lookup, same reason — a wrong capacitor
# that lands silently becomes a board that does not work.
#
# So the load-bearing checks are:
#   * staged lines do not touch mrp_bom_line until commit,
#   * commit REFUSES while any line is in error — a half-imported BOM is worse
#     than none because it looks complete,
#   * the designator/quantity check catches the commonest hand-edit error,
#   * a designator cannot be used twice on one board,
#   * a kit is forced to a phantom BOM, because a kit is packed and never made.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }

cleanup(){
    pg "DELETE FROM mrp_bom_import_line WHERE bom_id IN (SELECT id FROM mrp_bom WHERE code LIKE 'QA-BOM%')" >/dev/null
    pg "DELETE FROM mrp_bom_line        WHERE bom_id IN (SELECT id FROM mrp_bom WHERE code LIKE 'QA-BOM%')" >/dev/null
    pg "DELETE FROM mrp_bom             WHERE code LIKE 'QA-BOM%'" >/dev/null
    pg "DELETE FROM part_manufacturer_info WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'QA-BI-%')" >/dev/null
    pg "DELETE FROM part_parameter      WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'QA-BI-%')" >/dev/null
    pg "DELETE FROM product_product     WHERE default_code LIKE 'QA-BI-%'" >/dev/null
    pg "DELETE FROM part_footprint      WHERE name='QA-BI-FP'" >/dev/null
}
cleanup; trap cleanup EXIT

SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; echo "*** FAILURES ***"; exit 1; }
CTX="\"context\":{\"session_id\":\"$SID\"}"
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":[$3],\"kwargs\":{$CTX}}}"; }
py(){ python3 -c "$1" 2>/dev/null; }
export PYTHONIOENCODING=utf-8

# ---- fixtures: three real parts the importer can resolve against -----------
UOM=$(pg "SELECT id FROM uom_uom ORDER BY id LIMIT 1")
CAT=$(pg "SELECT id FROM product_category ORDER BY id LIMIT 1")
UF=$(pg "SELECT id FROM part_unit WHERE symbol='F'")
FP=$(pg "INSERT INTO part_footprint (name, description) VALUES ('QA-BI-FP','test') RETURNING id" | head -1)
MFR=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")

mkpart(){ # name code value mpn
  local pid
  pid=$(pg "INSERT INTO product_product (name, default_code, type, categ_id, uom_id, uom_po_id,
              list_price, standard_price, qty_available, footprint_id, active, sale_ok, purchase_ok, company_id)
            VALUES ('$1','$2','product',$CAT,$UOM,$UOM,1000,500,0,$FP,true,true,true,NULL) RETURNING id" | head -1)
  [ -n "$3" ] && pg "INSERT INTO part_parameter (product_id,name,value_numeric,unit_id,value_text,value_base,quantity_kind)
                     VALUES ($pid,'Capacitance',0.0000001,$UF,'$3',0.0000001,'capacitance')" >/dev/null
  [ -n "$4" ] && pg "INSERT INTO part_manufacturer_info (product_id,manufacturer_id,part_number)
                     VALUES ($pid,$MFR,'$4')" >/dev/null
  echo "$pid"
}
P1=$(mkpart 'QA-BI cap 100nF' 'QA-BI-1' '100nF' 'GRM188R71C104KA01D')
P2=$(mkpart 'QA-BI res 10k'   'QA-BI-2' '10k'   'RC0402FR-0710KL')
# A deliberate duplicate value+footprint, so "several parts match" is reachable.
P3=$(mkpart 'QA-BI cap 100nF alt' 'QA-BI-3' '100nF' 'C0603C104K5RACTU')
PROD=$(pg "SELECT id FROM product_product WHERE default_code='QA-BI-1'")

echo "############ creating BOMs ############"
BID=$(call bom.editor create_bom "{\"product_id\":$PROD,\"kind\":\"pcba\",\"code\":\"QA-BOM-1\"}" | py "
import json,sys
print(json.load(sys.stdin)['result']['id'])")
[ -n "$BID" ] && ok "a PCBA BOM was created (id $BID)" || { no "create_bom failed"; echo '  *** FAILURES ***'; exit 1; }
[ "$(pg "SELECT bom_type FROM mrp_bom WHERE id=$BID")" = "normal" ] \
  && ok "a PCBA is a normal BOM — it is manufactured" || no "PCBA bom_type is wrong"

KID=$(call bom.editor create_bom "{\"product_id\":$PROD,\"kind\":\"kit\",\"code\":\"QA-BOM-KIT\"}" | py "
import json,sys
print(json.load(sys.stdin)['result']['id'])")
# docs/105 §5b — a kit is packed, never made. A kit with a manufacturing order
# would reserve components to build something that does not physically exist.
[ "$(pg "SELECT bom_type FROM mrp_bom WHERE id=$KID")" = "phantom" ] \
  && ok "a kit is forced to a phantom BOM" || no "kit bom_type is $(pg "SELECT bom_type FROM mrp_bom WHERE id=$KID")"

echo "############ describe — the agent contract ############"
D=$(call bom.import describe '{}')
for K in row_fields header_aliases known_footprints known_units contract; do
    echo "$D" | grep -q "\"$K\"" && ok "describe exposes $K" || no "describe missing $K"
done
echo "$D" | grep -qi 'Never supply a product id' \
  && ok "describe tells an agent not to choose parts" || no "the agent contract does not state the boundary"

echo "############ parse — headers, ranges, resolution ############"
CSV='Designator,Qty,Value,Footprint,MPN
C1,1,100nF,QA-BI-FP,GRM188R71C104KA01D
R1-R4,4,10k,QA-BI-FP,RC0402FR-0710KL'
JSON=$(python3 -c "
import json,sys
print(json.dumps({'bom_id': int(sys.argv[1]), 'text': sys.argv[2]}))" "$BID" "$CSV")
R=$(call bom.import parse "$JSON")
N=$(echo "$R" | py "
import json,sys
print(len(json.load(sys.stdin)['result']['rows']))")
[ "$N" = "2" ] && ok "two rows were staged" || no "staged $N rows"
OKC=$(echo "$R" | py "
import json,sys
print(json.load(sys.stdin)['result']['counts']['ok'])")
[ "$OKC" = "2" ] && ok "both rows resolved by MPN" || no "$OKC row(s) resolved cleanly"
# R1-R4 must become four designators and agree with the quantity of 4.
DES=$(echo "$R" | py "
import json,sys
d=json.load(sys.stdin)['result']['rows']
print(next(r['designators'] for r in d if r['quantity']==4))")
[ -n "$DES" ] && ok "the designator range survived as '$DES'" || no "range row missing"

echo "############ nothing reaches the BOM before commit ############"
[ "$(pg "SELECT count(*) FROM mrp_bom_line WHERE bom_id=$BID")" = "0" ] \
  && ok "staging did not touch mrp_bom_line" || no "staged rows leaked into the BOM"

echo "############ commit ############"
call bom.import commit "{\"bom_id\":$BID}" | grep -q '"ok":true' && ok "commit succeeded" || no "commit failed"
[ "$(pg "SELECT count(*) FROM mrp_bom_line WHERE bom_id=$BID")" = "2" ] \
  && ok "two lines are now in the BOM" || no "BOM has $(pg "SELECT count(*) FROM mrp_bom_line WHERE bom_id=$BID") lines"
[ "$(pg "SELECT reference_designators FROM mrp_bom_line WHERE bom_id=$BID AND product_qty=4")" = "R1,R2,R3,R4" ] \
  && ok "the expanded designators were written" \
  || no "designators are $(pg "SELECT reference_designators FROM mrp_bom_line WHERE bom_id=$BID AND product_qty=4")"
[ "$(pg "SELECT count(*) FROM mrp_bom_import_line WHERE bom_id=$BID")" = "0" ] \
  && ok "staging was cleared after commit" || no "staging survived the commit"

echo "############ designator count must equal quantity ############"
BAD='Designator,Qty,Value,Footprint,MPN
C1,C2,C5,4,100nF,QA-BI-FP,GRM188R71C104KA01D'
J2=$(python3 -c "
import json,sys
print(json.dumps({'bom_id': int(sys.argv[1]), 'text': 'Designator,Qty,Value,Footprint,MPN\n\"C1,C2,C5\",4,100nF,QA-BI-FP,GRM188R71C104KA01D'}))" "$BID")
R2=$(call bom.import parse "$J2")
SEV=$(echo "$R2" | py "
import json,sys
print(json.load(sys.stdin)['result']['rows'][0]['severity'])")
[ "$SEV" = "error" ] && ok "3 designators with quantity 4 is an error" || no "severity was '$SEV'"
echo "$R2" | grep -qi '3 designators but a quantity of 4' \
  && ok "the error says exactly what is wrong" || no "the message is not specific"

echo "############ commit refuses while a line is in error ############"
call bom.import commit "{\"bom_id\":$BID}" | grep -qi 'still have errors' \
  && ok "commit refused" || no "commit went ahead with an error present"
[ "$(pg "SELECT count(*) FROM mrp_bom_line WHERE bom_id=$BID")" = "2" ] \
  && ok "the previous lines were left untouched" || no "a refused commit still changed the BOM"

echo "############ a designator cannot be used twice ############"
J3=$(python3 -c "
import json
print(json.dumps({'bom_id': BID, 'text': 'Designator,Qty,Value,Footprint,MPN\nC1,1,100nF,QA-BI-FP,GRM188R71C104KA01D\nC1,1,10k,QA-BI-FP,RC0402FR-0710KL'}).replace('BID','$BID'))" 2>/dev/null \
    || python3 -c "
import json,sys
print(json.dumps({'bom_id': int(sys.argv[1]), 'text': 'Designator,Qty,Value,Footprint,MPN\nC1,1,100nF,QA-BI-FP,GRM188R71C104KA01D\nC1,1,10k,QA-BI-FP,RC0402FR-0710KL'}))" "$BID")
R3=$(call bom.import parse "$J3")
echo "$R3" | grep -qi 'appears more than once' \
  && ok "a duplicated designator is caught" || no "duplicate designators were accepted"

echo "############ ambiguity is a warning, not a silent pick ############"
J4=$(python3 -c "
import json,sys
print(json.dumps({'bom_id': int(sys.argv[1]), 'text': 'Designator,Qty,Value,Footprint\nC9,1,100nF,QA-BI-FP'}))" "$BID")
R4=$(call bom.import parse "$J4")
S4=$(echo "$R4" | py "
import json,sys
print(json.load(sys.stdin)['result']['rows'][0]['severity'])")
[ "$S4" = "warning" ] && ok "two matching parts gives a warning" || no "severity was '$S4'"
NC=$(echo "$R4" | py "
import json,sys
print(len(json.load(sys.stdin)['result']['rows'][0]['candidates']))")
[ "$NC" = "2" ] && ok "both candidates are offered" || no "$NC candidate(s) offered"
PIDSET=$(echo "$R4" | py "
import json,sys
print(json.load(sys.stdin)['result']['rows'][0]['product_id'])")
[ "$PIDSET" = "0" ] && ok "no part was chosen automatically" || no "the importer picked one by itself"

echo "############ set_line resolves the ambiguity ############"
LID=$(echo "$R4" | py "
import json,sys
print(json.load(sys.stdin)['result']['rows'][0]['id'])")
R5=$(call bom.import set_line "{\"id\":$LID,\"product_id\":$P1}")
S5=$(echo "$R5" | py "
import json,sys
print(json.load(sys.stdin)['result']['rows'][0]['severity'])")
[ "$S5" = "ok" ] && ok "choosing a candidate clears the warning" || no "severity is still '$S5'"
call bom.import set_line "{\"id\":$LID,\"product_id\":999999}" | grep -qi 'No such product' \
  && ok "an unknown product is rejected" || no "an unknown product was accepted"

echo "############ unresolvable lines ############"
J6=$(python3 -c "
import json,sys
print(json.dumps({'bom_id': int(sys.argv[1]), 'text': 'Designator,Qty,Value,Footprint,MPN\nU1,1,,,NOTHING-MATCHES-THIS'}))" "$BID")
R6=$(call bom.import parse "$J6")
echo "$R6" | grep -qi 'No part in the catalogue matches' \
  && ok "an unmatched MPN is reported plainly" || no "unmatched line message is wrong"

echo "############ unrecognised headers ask for help rather than guessing ############"
J7=$(python3 -c "
import json,sys
print(json.dumps({'bom_id': int(sys.argv[1]), 'text': 'colA,colB,colC\n1,2,3'}))" "$BID")
call bom.import parse "$J7" | grep -qi 'header row was not recognised' \
  && ok "an unknown layout is refused with guidance" || no "an unknown layout was guessed at"

echo "############ an agent may supply normalised rows ############"
J8=$(python3 -c "
import json,sys
print(json.dumps({'bom_id': int(sys.argv[1]),
                  'rows': [{'designators':'C7','quantity':1,'mpn':'GRM188R71C104KA01D'}]}))" "$BID")
R8=$(call bom.import parse "$J8")
S8=$(echo "$R8" | py "
import json,sys
print(json.load(sys.stdin)['result']['rows'][0]['severity'])")
[ "$S8" = "ok" ] && ok "rows supplied directly are resolved the same way" || no "agent rows gave '$S8'"

echo "############ the editor checks hand-typed lines too ############"
call bom.import discard "{\"bom_id\":$BID}" >/dev/null
call bom.editor add_line "{\"bom_id\":$BID,\"quantity\":2,\"designators\":\"C1\"}" >/dev/null
L=$(call bom.editor lines "{\"bom_id\":$BID}")
BADL=$(echo "$L" | py "
import json,sys
d=json.load(sys.stdin)['result']['lines']
print(sum(1 for l in d if l['severity']=='error'))")
[ -n "$BADL" ] && [ "$BADL" -ge 1 ] && ok "a typed line with no part and a bad count is flagged" \
  || no "the editor did not flag the bad hand-typed line"
echo "$L" | grep -qi 'No part chosen' && ok "the reason is given" || no "no reason on the flagged line"

echo "############ severities are a fixed vocabulary ############"
BADSEV=$(pg "SELECT count(*) FROM mrp_bom_import_line WHERE severity NOT IN ('ok','warning','error')")
[ "${BADSEV:-0}" = "0" ] && ok "no severity outside ok/warning/error" || no "$BADSEV odd severities stored"

echo "############ wiring ############"
grep -q "'bom.editor'" web/static/src/app.js && ok "bom.editor is a registered custom view" || no "not in CUSTOM_VIEWS"
grep -q 'BomEditor.js' web/static/index.html && ok "the component is loaded" || no "BomEditor.js not loaded"
[ "$(pg "SELECT count(*) FROM ir_ui_menu WHERE id=150")" = "1" ] && ok "the menu exists" || no "no BOM Editor menu"

[ -z "$FAILED" ] && echo "  All checks passed." || echo "  *** FAILURES ***"

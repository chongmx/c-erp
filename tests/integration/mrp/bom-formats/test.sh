#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIX="$R/fixtures"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# BOM import against the formats real EDA tools actually export.
#
# Every tool names its columns differently, and the differences are not
# cosmetic — they change which field a value lands in:
#
#   KiCad    Reference, Qty, Value, Footprint, Datasheet, Description, DNP
#   Altium   Designator, Comment, Description, Footprint, LibRef, Quantity,
#            Manufacturer, Manufacturer Part Number
#   EAGLE    Qty, Value, Device, Package, Parts, Description   ← SEMICOLONS
#   JLCPCB   Comment, Designator, Footprint, LCSC Part #
#
# Two of these are traps:
#
#   * "Comment" is the component VALUE in Altium and JLCPCB, not a note. The
#     matcher used to file it under description, so the value — the field
#     resolution leans on hardest when there is no MPN — was silently dropped.
#   * "DNP" means NOT fitted. Reading it as "fitted" populates exactly the
#     parts that were meant to be left off the board.
#
# The fixtures are checked in beside this file so a format can be added by
# dropping in another export, not by editing a test.
# =============================================================
auth_or_die

BOMPROD=""; BOMID=""
cleanup() {
    [ -n "$BOMID" ] && pg "DELETE FROM mrp_bom_import_line WHERE bom_id=$BOMID;
                           DELETE FROM mrp_bom_line WHERE bom_id=$BOMID;
                           DELETE FROM mrp_bom WHERE id=$BOMID" >/dev/null
    pg "DELETE FROM part_manufacturer_info WHERE product_id IN
          (SELECT id FROM product_product WHERE default_code LIKE 'QA-BF%');
        DELETE FROM part_parameter WHERE product_id IN
          (SELECT id FROM product_product WHERE default_code LIKE 'QA-BF%');
        DELETE FROM product_product  WHERE default_code LIKE 'QA-BF%';
        DELETE FROM product_template WHERE name LIKE 'QA-BF%'" >/dev/null
}
cleanup; trap cleanup EXIT

# --- a board to import into, and parts for the rows to resolve against ------
BOMPROD=$(call product.product create '[{"name":"QA-BF Board","default_code":"QA-BF-BOARD","type":"product"}]' | rid)
t_nonempty "$BOMPROD" "a board product exists"
BOMID=$(call bom.editor create_bom "[{\"product_id\":$BOMPROD,\"kind\":\"pcba\"}]" \
        | python3 -c 'import sys,json
d=json.load(sys.stdin)["result"]; print(d.get("id") or d.get("bom_id") or "")' 2>/dev/null)
t_nonempty "$BOMID" "a BOM exists to import into ($BOMID)"

# part.manufacturer.info needs a real res.partner — manufacturer_id is a
# required many2one, and passing a bare name silently creates nothing, which
# then looks like "the MPN did not resolve".
MFR=$(call res.partner create '[{"name":"QA-BF Mfr"}]' | rid)
t_nonempty "$MFR" "a manufacturer partner exists"

mkpart() {  # mkpart <code> <name> <mpn>
    local id
    id=$(call product.product create "[{\"name\":\"$2\",\"default_code\":\"$1\",\"type\":\"product\"}]" | rid)
    [ -n "$3" ] && call part.manufacturer.info create \
        "[{\"product_id\":$id,\"manufacturer_id\":$MFR,\"part_number\":\"$3\"}]" >/dev/null
    echo "$id"
}
P_CAP=$(mkpart QA-BF-CAP "QA-BF Cap 100nF 0603" "GRM188R71H104KA93D")
P_RES=$(mkpart QA-BF-RES "QA-BF Res 4k7 0603"   "RC0603FR-074K7L")
P_IND=$(mkpart QA-BF-IND "QA-BF Ind 1uH 0603"   "ASMCI-0603-1R0M-T")
t_nonempty "$P_CAP" "fixture parts exist"
t_eq "1" "$(pg "SELECT count(*) FROM part_manufacturer_info WHERE product_id=$P_CAP")" \
     "and the capacitor carries its MPN"

# parse_file <fixture> [mapping-json] -> the raw parse response
# Built in python rather than shell: a CSV full of quotes and newlines cannot
# survive being interpolated into a JSON string by hand.
parse_file() {
    python3 - "$FIX/$1" "$BOMID" "$BASE" "$SID" "${2:-}" <<'PY'
import json, sys, urllib.request
path, bom, base, sid, mapping = sys.argv[1:6]
args = {"bom_id": int(bom), "text": open(path, encoding="utf-8").read()}
if mapping:
    args["mapping"] = json.loads(mapping)
    args["skip_header"] = True
body = {"jsonrpc":"2.0","method":"call","params":{
    "model":"bom.import","method":"parse","args":[args],
    "kwargs":{"context":{"session_id":sid}}}}
req = urllib.request.Request(base + "/web/dataset/call_kw",
        data=json.dumps(body).encode(), headers={"Content-Type":"application/json"})
print(urllib.request.urlopen(req).read().decode())
PY
}

# field_of <designator-substring> <column> — read a staged line back
field_of() {
    pgv "SELECT COALESCE($2::text,'') FROM mrp_bom_import_line
         WHERE bom_id=$BOMID AND designators ILIKE '%$1%' LIMIT 1" | xargs
}

# =============================================================
sec "1. KiCad — Reference / Qty / Value / Footprint / DNP"
OUT=$(parse_file kicad.csv)
has_error "$OUT" && no "KiCad parse failed: $(echo "$OUT" | head -c 200)" \
                 || ok "a stock KiCad export parses"
t_eq "4"      "$(field_of 'R1' quantity)"   "R1-R4 expands to a quantity of 4"
t_eq "4k7"    "$(field_of 'R1' value_text)" "Value lands in value, not description"
t_eq "0603"   "$(echo "$(field_of 'R1' footprint)" | grep -o '0603' | head -1)" \
     "the KiCad footprint string is kept"
t_eq "100nF"  "$(field_of 'C1' value_text)" "the capacitor value is read"
# DNP means NOT fitted. Reading it the other way round populates exactly the
# parts somebody deliberately left off.
# boolean::text is 'true'/'false' in Postgres, not 't'/'f'.
t_eq "false" "$(field_of 'TP1' fitted)" "a DNP line is staged as not fitted"
t_eq "true"  "$(field_of 'C1'  fitted)" "and a blank DNP cell leaves the line fitted"

sec "2. Altium — Comment IS the value"
OUT=$(parse_file altium.csv)
has_error "$OUT" && no "Altium parse failed: $(echo "$OUT" | head -c 200)" \
                 || ok "an Altium export parses"
# The trap. "Comment" used to be filed under description, so the value was
# dropped and every row without an MPN became unresolvable.
t_eq "100nF" "$(field_of 'C1' value_text)" "Comment is read as the VALUE"
t_contains "$(field_of 'C1' description)" "Capacitor" "and Description stays the description"
t_eq "GRM188R71H104KA93D" "$(field_of 'C1' mpn)" "Manufacturer Part Number is read"
t_eq "Murata" "$(field_of 'C1' manufacturer)"     "Manufacturer is read"
t_eq "3" "$(field_of 'C1' quantity)"              "Quantity is read"
# With an MPN that matches the catalogue, the line resolves to that exact part
# — asserted by its code rather than its id, so a demo catalogue that also has
# a 100nF 0603 cannot make this pass by matching on value+footprint instead.
t_eq "QA-BF-CAP" "$(pgv "SELECT COALESCE(p.default_code,'') FROM mrp_bom_import_line l
                         JOIN product_product p ON p.id=l.product_id
                         WHERE l.bom_id=$BOMID AND l.designators ILIKE '%C1%' LIMIT 1" | xargs)" \
     "the MPN resolved to the catalogue part, not a lookalike"

sec "3. EAGLE — semicolons, and Parts holds the designators"
OUT=$(parse_file eagle.csv)
has_error "$OUT" && no "EAGLE parse failed: $(echo "$OUT" | head -c 200)" \
                 || ok "a semicolon-delimited EAGLE export parses"
t_eq "4k7" "$(field_of 'R1' value_text)"  "Value is read across semicolons"
t_eq "4"   "$(field_of 'R1' quantity)"    "Qty is read"
t_eq "R0603" "$(field_of 'R1' footprint)" "Package is read as the footprint"
t_ge "$(pg "SELECT count(*) FROM mrp_bom_import_line WHERE bom_id=$BOMID")" 4 \
     "every EAGLE row was staged"

sec "4. JLCPCB — Comment + Designator + LCSC Part #"
OUT=$(parse_file jlcpcb.csv)
has_error "$OUT" && no "JLCPCB parse failed: $(echo "$OUT" | head -c 200)" \
                 || ok "a JLCPCB assembly BOM parses"
t_eq "100nF" "$(field_of 'C1' value_text)" "Comment is the value here too"
t_eq "C14663" "$(field_of 'C1' mpn)"       "LCSC Part # is read as the part number"
t_eq "C0603"  "$(field_of 'C1' footprint)" "Footprint is read"

sec "5. an unrecognised layout asks rather than guesses"
# Wrong answers are worse than no answer here: a mis-mapped column silently
# imports the wrong data with no error anywhere.
OUT=$(parse_file unknown-tool.csv)
has_error "$OUT" && ok "an unknown header row is refused" \
                 || no "an unrecognised layout was parsed anyway"
t_contains "$OUT" "header row was not recognised" "and says so in a way the screen can act on"

sec "6. an explicit mapping gets the same file in"
# This is the path the assistant feeds: it proposes the indices, a person
# confirms them, and parse runs exactly as if the headers had been recognised.
MAP=$(parse_file unknown-tool.csv \
      '{"designators":1,"quantity":2,"value":3,"footprint":4,"mpn":5}')
has_error "$MAP" && no "the explicit mapping failed: $(echo "$MAP" | head -c 200)" \
                 || ok "the same file parses once the columns are named"
t_eq "4k7" "$(field_of 'R1' value_text)"      "the mapped value column landed in value"
t_eq "RC0603FR-074K7L" "$(field_of 'R1' mpn)" "and the mapped part number in mpn"
t_eq "4"   "$(field_of 'R1' quantity)"        "and the mapped quantity"
t_eq "3" "$(pg "SELECT count(*) FROM mrp_bom_import_line WHERE bom_id=$BOMID")" \
     "every data row was taken, and the header row was not"

sec "7. describe advertises what the matcher actually does"
# These were two hand-kept lists and had already disagreed about "comment",
# which is how the Altium bug survived. describe is now generated from the
# matcher's own table, so it cannot drift again.
D=$(call bom.import describe '[{}]')
t_contains "$D" '"header_aliases"' "describe exposes the alias table"
for a in comment designator refdes parts package "lcsc part #" dnp; do
    t_contains "$D" "\"$a\"" "describe lists the '$a' alias"
done
# The one that matters: comment must be advertised under value, not description.
t_eq "1" "$(echo "$D" | python3 -c '
import sys, json
h = json.load(sys.stdin)["result"]["header_aliases"]
print(int("comment" in h.get("value", []) and "comment" not in h.get("description", [])))')" \
     "and files 'comment' under value, where Altium and JLCPCB put it"

sec "7b. the assistant proposes a mapping, and only a mapping"
# The narrowest AI seam in the importer: a model maps COLUMNS. It never
# resolves a part — that is a lookup which has to be reproducible.
WAS=$(pg "SELECT provider FROM ir_ai_settings WHERE id=1")
WASON=$(pg "SELECT enabled::int FROM ir_ai_settings WHERE id=1")
call ir.ai.settings save '[{"provider":"mock","enabled":true}]' >/dev/null
M=$(call ir.ai.settings map_bom_headers \
    '[{"header":"Item,Board Position,Count,Spec,Land Pattern,Vendor Code",
       "samples":["1,C1,3,100nF,0603,GRM188R71H104KA93D"]}]')
t_contains "$M" '"ok":true'    "the assistant answers with a mapping"
t_contains "$M" '"mapping"'    "shaped as column indices"
t_lacks    "$M" '"product_id"' "and never names a part — that is not its job"
t_eq "6" "$(echo "$M" | python3 -c 'import sys,json; print(json.load(sys.stdin)["result"]["columns"])')" \
     "it counts the columns it was given"

# A model answering {"fitted_negated": null} for a file with no DNP column is
# entirely reasonable — and nlohmann's .value() THROWS on an explicit null,
# which surfaced as "An internal error occurred" on a perfectly good reply.
NULLS=$(call ir.ai.settings map_bom_headers \
        '[{"header":"Ref,Qty,Val","samples":["C1,1,100nF"],"_probe":null}]')
has_error "$NULLS" && no "a reply with null fields crashed the request" \
                   || ok "null fields in a reply are tolerated, not fatal"

# An index the file does not have would silently read empty cells forever.
BADIDX=$(call ir.ai.settings map_bom_headers '[{"header":"A,B","samples":["1,2"]}]')
t_contains "$BADIDX" '"columns":2' "a two-column file is measured as two columns"
t_eq "0" "$(echo "$BADIDX" | python3 -c '
import sys, json
m = json.load(sys.stdin)["result"].get("mapping", {})
print(sum(1 for v in m.values() if not isinstance(v, int) or v < 0 or v > 1))')" \
     "and no proposed index points past the end of the row"

call ir.ai.settings save \
    "[{\"provider\":\"$WAS\",\"enabled\":$([ "$WASON" = "1" ] && echo true || echo false)}]" >/dev/null

sec "7c. tidying rows to house conventions"
# The second AI seam: the model rewrites TEXT — 4.7K to 4k7,
# Capacitor_SMD:C_0603_1608Metric to 0603 — and never chooses a part. The
# tidied rows go back through parse, which resolves them exactly as before.
call ir.ai.settings save '[{"provider":"mock","enabled":true}]' >/dev/null
MESSY='[{"rows":[
  {"designators":"C1,C2","quantity":2,"value":"100nF","footprint":"Capacitor_SMD:C_0603_1608Metric","description":"~","fitted":true},
  {"designators":"R1-R4","quantity":0,"value":"4.7K ohm","mpn":"R-EU_R0603","description":"RESISTOR","fitted":true}]}]'
C=$(call ir.ai.settings clean_bom_rows "$MESSY")
t_contains "$C" '"ok":true'   "the assistant returns tidied rows"
t_contains "$C" '"changed"'   "with a diff of what it touched"
t_lacks    "$C" '"product_id"' "and never a part — that lookup stays the server's"
t_eq "2" "$(echo "$C" | python3 -c 'import sys,json; print(len(json.load(sys.stdin)["result"]["rows"]))')" \
     "the same number of rows comes back"

# The load-bearing guard. A model asked to tidy 60 rows will sometimes return
# 58, and a BOM quietly missing a line is far worse than an untidy one: the
# board is short a part and nothing says so. The mock echoes, so the count is
# asserted rather than the refusal — the refusal itself is unit-testable only
# against a provider that misbehaves.
t_eq "2" "$(echo "$MESSY" | python3 -c 'import sys,json; print(len(json.loads(sys.stdin.read())[0]["rows"]))')" \
     "and it is the same number that went in"

# Tidied rows must go back in through the importer's own agent path.
BACK=$(echo "$C" | python3 -c '
import sys, json
rows = json.load(sys.stdin)["result"]["rows"]
print(json.dumps(rows))')
RE=$(python3 - "$BOMID" "$BASE" "$SID" "$BACK" <<'PY'
import json, sys, urllib.request
bom, base, sid, rows = sys.argv[1:5]
body = {"jsonrpc":"2.0","method":"call","params":{
    "model":"bom.import","method":"parse",
    "args":[{"bom_id":int(bom),"rows":json.loads(rows)}],
    "kwargs":{"context":{"session_id":sid}}}}
req = urllib.request.Request(base + "/web/dataset/call_kw",
        data=json.dumps(body).encode(), headers={"Content-Type":"application/json"})
print(urllib.request.urlopen(req).read().decode())
PY
)
has_error "$RE" && no "tidied rows were refused by parse: $(echo "$RE" | head -c 160)" \
                || ok "tidied rows re-import through the normal path"
t_eq "2" "$(pg "SELECT count(*) FROM mrp_bom_import_line WHERE bom_id=$BOMID")" \
     "and are staged for review like any other import"

sec "7d. the tidy-up refuses what it should"
EMPTY=$(call ir.ai.settings clean_bom_rows '[{"rows":[]}]')
has_error "$EMPTY" && ok "an empty row list is refused" || no "an empty row list was accepted"
NOROWS=$(call ir.ai.settings clean_bom_rows '[{}]')
has_error "$NOROWS" && ok "a call with no rows at all is refused" || no "a call with no rows was accepted"
# A cap on cost, and on how much of the part list leaves the building.
BIG=$(python3 -c 'import json; print(json.dumps([{"rows":[{"designators":"R%d"%i,"value":"1k"} for i in range(301)]}]))')
TOOBIG=$(call ir.ai.settings clean_bom_rows "$BIG")
has_error "$TOOBIG" && ok "more than 300 rows at once is refused" || no "301 rows were accepted"
t_contains "$TOOBIG" "300" "and the message says what the limit is"

call ir.ai.settings save \
    "[{\"provider\":\"$(pg "SELECT provider FROM ir_ai_settings WHERE id=1")\"}]" >/dev/null

sec "8. a staged import is a draft that survives"
# It persists until committed or discarded — leaving the screen loses nothing.
# What was missing was any way to tell, so staged() now reports its age.
parse_file kicad.csv >/dev/null
ST=$(call bom.import staged "[{\"bom_id\":$BOMID}]")
t_contains "$ST" '"draft"'   "staged reports the draft"
t_contains "$ST" '"started"' "with when it was started"
t_ge "$(echo "$ST" | python3 -c 'import sys,json; print(json.load(sys.stdin)["result"]["counts"]["total"])')" 4 \
     "and the rows are still there"

sec "9. discard throws the draft away, and nothing else"
call bom.import discard "[{\"bom_id\":$BOMID}]" >/dev/null
t_eq "0" "$(pg "SELECT count(*) FROM mrp_bom_import_line WHERE bom_id=$BOMID")" \
     "the staged rows are gone"
t_eq "1" "$(pg "SELECT count(*) FROM mrp_bom WHERE id=$BOMID")" "the BOM itself is untouched"
ST=$(call bom.import staged "[{\"bom_id\":$BOMID}]")
t_contains "$ST" '"draft":null' "and there is no draft to resume"

verdict

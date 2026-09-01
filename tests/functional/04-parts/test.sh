#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# FUNCTIONAL JOURNEY 04 — PARTS.  (docs/109 §3)
#
#   a BOM arrives as a CSV -> parts are resolved against the catalogue ->
#   what cannot be resolved is REFUSED -> a human fixes it -> commit ->
#   the parts are findable again by parameter -> labels print
#
# The engineer's day. What separates it from the importer's own test is that
# it resolves against the REAL demo catalogue rather than fixtures it planted
# a moment earlier: the MPNs and the resistance values are read out of the
# database first and fed back in as if they had come from a vendor's
# spreadsheet. If the catalogue and the importer ever disagree about what a
# part is, this is where it shows.
#
# The load-bearing rule, stated in the importer's own describe(): THE IMPORTER
# NEVER PICKS A PART. A row it cannot resolve with confidence is an error a
# person has to settle, because a wrongly-matched resistor is one somebody
# solders onto a board.
#
# Prefixed PT- / 'PT ' and removed on the way out.
# =============================================================
auth_or_die

cleanup() {
    pg "DELETE FROM mrp_bom_import_line WHERE bom_id IN (SELECT id FROM mrp_bom WHERE code LIKE 'PT-%')" >/dev/null
    pg "DELETE FROM mrp_bom_line   WHERE bom_id IN (SELECT id FROM mrp_bom WHERE code LIKE 'PT-%')" >/dev/null
    pg "DELETE FROM mrp_bom        WHERE code LIKE 'PT-%'" >/dev/null
    pg "DELETE FROM product_product  WHERE default_code LIKE 'PT-%'" >/dev/null
    pg "DELETE FROM product_template WHERE default_code LIKE 'PT-%'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

py() { python3 -c "$1" 2>/dev/null; }

# ------------------------------------------------------------------
sec "1. the catalogue we are going to design against"
# ------------------------------------------------------------------
# Read real parts out of the catalogue. Nothing here is planted: if the demo
# catalogue is missing, this journey says so instead of quietly testing itself
# against its own fixtures.
DEMO=$(pg "SELECT count(*) FROM product_product WHERE default_code LIKE 'DP-%'")
t_ge "${DEMO:-0}" 50 "there is a catalogue to design against"
if [ "${DEMO:-0}" -lt 50 ]; then
    echo "    the demo catalogue is not loaded — rebuild it with scripts/seed/parts.sh"
    verdict; exit 1
fi

read_part() {  # read_part <n> -> "code|mpn|value_text"
    pg "SELECT p.default_code || '|' || mi.part_number || '|' || COALESCE(pp.value_text,'')
          FROM product_product p
          JOIN part_manufacturer_info mi ON mi.product_id = p.id
          LEFT JOIN part_parameter pp ON pp.product_id = p.id AND pp.name='Resistance'
         WHERE p.default_code LIKE 'DP-R-%' AND mi.part_number <> ''
         ORDER BY p.id OFFSET $1 LIMIT 1"
}
P1=$(read_part 0); P2=$(read_part 1)
MPN1=$(echo "$P1" | cut -d'|' -f2); VAL1=$(echo "$P1" | cut -d'|' -f3)
MPN2=$(echo "$P2" | cut -d'|' -f2); VAL2=$(echo "$P2" | cut -d'|' -f3)
echo "    designing with $MPN1 ($VAL1) and $MPN2 ($VAL2)"
t_nonempty "$MPN1" "a real part with an MPN was found"
t_nonempty "$MPN2" "and a second one"
[ -z "$MPN1" ] || [ -z "$MPN2" ] && { verdict; exit 1; }

# ------------------------------------------------------------------
sec "2. the board and its BOM"
# ------------------------------------------------------------------
UOM=$(pg "SELECT id FROM uom_uom ORDER BY id LIMIT 1")
CAT=$(pg "SELECT id FROM product_category ORDER BY id LIMIT 1")
TMPL=$(pgid "INSERT INTO product_template (name, default_code, type, categ_id, uom_id, uom_po_id,
             list_price, standard_price, active, sale_ok, purchase_ok, company_id)
             VALUES ('PT Controller Board','PT-PCBA-1','product',$CAT,$UOM,$UOM,0,0,true,true,true,1)
             RETURNING id")
BOARD=$(pgid "INSERT INTO product_product (name, default_code, type, categ_id, uom_id, uom_po_id,
              list_price, standard_price, qty_available, active, sale_ok, purchase_ok, company_id,
              product_tmpl_id)
              VALUES ('PT Controller Board','PT-PCBA-1','product',$CAT,$UOM,$UOM,0,0,0,true,true,true,1,$TMPL)
              RETURNING id")
t_nonempty "$BOARD" "the board product exists"

BOMRES=$(call bom.editor create_bom "[{\"product_id\":$BOARD,\"kind\":\"pcba\",\"code\":\"PT-BOM-1\"}]")
BID=$(pg "SELECT id FROM mrp_bom WHERE code='PT-BOM-1'")
t_nonempty "$BID" "a BOM was created for it"
[ -z "$BID" ] && { no "create_bom said: $(echo "$BOMRES" | head -c 200)"; verdict; exit 1; }
# A PCBA is manufactured; a kit is only a collection put in a pack. The
# distinction decides whether this ever produces a manufacturing order.
t_eq "normal" "$(pg "SELECT bom_type FROM mrp_bom WHERE id=$BID")" "a PCBA BOM is manufactured, not a kit"

# ------------------------------------------------------------------
sec "3. importing the vendor's spreadsheet"
# ------------------------------------------------------------------
# Two resolvable rows and one deliberate unknown — which is the realistic
# case, and the only one worth testing.
CSV="Designator,Qty,Value,MPN
R1-R4,4,$VAL1,$MPN1
C1,1,$VAL2,$MPN2
U1,1,SomeMCU,PT-NO-SUCH-MPN-9999"
JSON=$(py "
import json,sys
print(json.dumps({'bom_id': $BID, 'text': '''$CSV'''}))")
RES=$(call bom.import parse "[$JSON]")
ROWS=$(echo "$RES" | py "
import json,sys
print(len(json.load(sys.stdin)['result']['rows']))")
t_eq "3" "${ROWS:-0}" "three rows were staged"

OKC=$(echo "$RES" | py "
import json,sys
print(json.load(sys.stdin)['result']['counts']['ok'])")
t_eq "2" "${OKC:-0}" "the two real MPNs resolved against the catalogue"

# The designator range must survive as the individual references — this is
# what the assembler reads off the board.
DES=$(echo "$RES" | py "
import json,sys
d=json.load(sys.stdin)['result']['rows']
print(next((r.get('designators','') for r in d if r.get('quantity')==4), ''))")
t_eq "R1,R2,R3,R4" "$DES" "the designator range R1-R4 expanded"

# ------------------------------------------------------------------
sec "4. what it could not resolve, it refuses to guess"
# ------------------------------------------------------------------
BAD=$(echo "$RES" | py "
import json,sys
d=json.load(sys.stdin)['result']['rows']
print(sum(1 for r in d if r.get('status') not in ('ok','resolved')))")
t_ge "${BAD:-0}" 1 "the unknown part is flagged rather than matched"

# Nothing may reach the BOM before a human commits it.
t_eq "0" "$(pg "SELECT count(*) FROM mrp_bom_line WHERE bom_id=$BID")" \
     "staging did not touch the real BOM"

# And committing with an unresolved row must be refused outright.
CRES=$(call bom.import commit "[{\"bom_id\":$BID}]")
if echo "$CRES" | grep -qi 'error\|still have'; then
    ok "commit is refused while a row is unresolved"
else
    no "commit went through with an unresolved row: $(echo "$CRES" | head -c 200)"
fi
t_eq "0" "$(pg "SELECT count(*) FROM mrp_bom_line WHERE bom_id=$BID")" \
     "and still nothing reached the BOM"

# ------------------------------------------------------------------
sec "5. a human settles it"
# ------------------------------------------------------------------
LID=$(pg "SELECT id FROM mrp_bom_import_line WHERE bom_id=$BID AND product_id IS NULL ORDER BY id LIMIT 1")
PICK=$(pg "SELECT id FROM product_product WHERE default_code LIKE 'DP-%' ORDER BY id LIMIT 1")
t_nonempty "$LID"  "the unresolved row is there to be fixed"
t_nonempty "$PICK" "and there is a part to point it at"
if [ -n "$LID" ] && [ -n "$PICK" ]; then
    SRES=$(call bom.import set_line "[{\"id\":$LID,\"product_id\":$PICK}]")
    has_error "$SRES" && no "set_line failed: $(echo "$SRES" | head -c 200)"
    t_eq "$PICK" "$(pg "SELECT product_id FROM mrp_bom_import_line WHERE id=$LID")" \
         "the row now points at the part a person chose"

    # A person cannot point it at something that does not exist either.
    NRES=$(call bom.import set_line "[{\"id\":$LID,\"product_id\":999999999}]")
    if echo "$NRES" | grep -qi 'no such product\|error'; then
        ok "pointing a row at a non-existent product is refused"
    else
        no "a row was pointed at product 999999999"
    fi
fi

# ------------------------------------------------------------------
sec "6. committing the BOM"
# ------------------------------------------------------------------
CRES2=$(call bom.import commit "[{\"bom_id\":$BID}]")
echo "$CRES2" | grep -q '"ok":true' && ok "commit succeeded once every row was settled" \
                                    || no "commit failed: $(echo "$CRES2" | head -c 200)"
LINES=$(pg "SELECT count(*) FROM mrp_bom_line WHERE bom_id=$BID")
t_eq "3" "${LINES:-0}" "all three lines are now in the BOM"
# Selected by "the row that has designators" rather than by quantity: BOM line
# quantities are written in micro-units by some paths and plain units by
# others, and pinning the assertion to one of them tests the writer's scaling
# rather than the designators.
t_eq "R1,R2,R3,R4" "$(pg "SELECT reference_designators FROM mrp_bom_line
                           WHERE bom_id=$BID AND COALESCE(reference_designators,'') <> ''
                           ORDER BY id LIMIT 1")" \
     "the expanded designators were written to the BOM"

# The BOM must reference the catalogue parts themselves, not copies of them.
LINKED=$(pg "SELECT count(*) FROM mrp_bom_line l
              JOIN product_product p ON p.id = l.product_id
             WHERE l.bom_id=$BID AND p.default_code LIKE 'DP-%'")
t_ge "${LINKED:-0}" 2 "the BOM lines point at real catalogue parts"

# ------------------------------------------------------------------
sec "7. the parts are still findable, and now they are used"
# ------------------------------------------------------------------
# Parametric search has to find the part the BOM just consumed — the catalogue
# and the BOM must agree about what exists.
FOUND=$(call part.catalog search '[{"query":"'"$MPN1"'"}]')
t_contains "$FOUND" "$MPN1" "the catalogue still finds the part by its MPN"

RANGE=$(call part.catalog search '[{"range":{"param:Resistance":{"min":1,"max":1000000,"unit":"Ω"}}}]')
TOT=$(echo "$RANGE" | py "
import json,sys
r=json.load(sys.stdin)['result']
print(r.get('total', len(r.get('rows',[]))))")
t_ge "${TOT:-0}" 1 "a parametric range still returns resistors"

# Where-used: the part now appears in a BOM, which is the question an engineer
# asks before changing anything.
USED=$(pg "SELECT count(*) FROM mrp_bom_line l JOIN mrp_bom b ON b.id=l.bom_id
            WHERE b.code='PT-BOM-1' AND l.product_id IN
                  (SELECT product_id FROM part_manufacturer_info WHERE part_number='$MPN1')")
t_ge "${USED:-0}" 1 "the part reports as used by this board's BOM"

# ------------------------------------------------------------------
sec "8. labels for the bench"
# ------------------------------------------------------------------
# The last step of the day: print what you are about to put in a drawer.
CODE=$(pg "SELECT default_code FROM product_product WHERE id=$PICK")
QR=$(http_code "/label/qr?data=$MPN1")
t_eq "200" "$QR" "a QR label renders for the part"
BODY=$(http_get "/label/qr?data=$MPN1")
t_contains "$BODY" "svg" "and it comes back as a drawable symbol"

SHEET=$(http_code "/labels/sheet?ids=$PICK")
case "$SHEET" in
    200) ok "a label sheet renders for the picked part" ;;
    *)   no "the label sheet returned HTTP $SHEET" ;;
esac

verdict

#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# The parameter vocabulary — part.parameter.keyword.  (CERP-8)
#
# Reported: "at my part look up, sometimes new parameters emerges, now it is
# being handled as error … should we create a new one? or should we put it
# under our existing parameters? I need an AI assisted suggestion on this."
#
# So the rule this file exists to pin is: A NEW PARAMETER NAME IS NEVER AN
# ERROR. It is matched against the vocabulary, and the answer is one of four —
# exact, a known spelling, close to something we have, or genuinely new — with
# the evidence attached. Whichever it is, `submit` still stages the part.
#
# Matching is on the NORMALISED name (lowercase, letters and digits only), so
# "Rds(on)", "RDS_ON" and "rds on" are one name. That alone removes most of the
# variation; the fuzzy step catches the rest ("Tolerence").
#
# The two actions a person can take are `create_keyword` (adopt it) and `merge`
# (fold it into an existing one). Merge is the one that writes to products: it
# renames every part_parameter row using the old name, which is what puts the
# parts back in each other's search results, and keeps the old spelling as an
# alias so nobody is asked the same question twice.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
PFX='ZZPV'

cleanup(){
    pg "DELETE FROM part_parameter WHERE product_id IN
          (SELECT id FROM product_product WHERE name LIKE '${PFX}%')" >/dev/null
    pg "DELETE FROM part_lookup_result WHERE query LIKE '${PFX}%'" >/dev/null
    pg "DELETE FROM product_product WHERE name LIKE '${PFX}%'" >/dev/null
    pg "DELETE FROM product_template WHERE name LIKE '${PFX}%'" >/dev/null
    pg "DELETE FROM part_parameter_alias WHERE keyword_id IN
          (SELECT id FROM part_parameter_keyword WHERE name LIKE '${PFX}%')" >/dev/null
    pg "DELETE FROM part_parameter_keyword WHERE name LIKE '${PFX}%'" >/dev/null
    pg "DELETE FROM part_parameter_alias WHERE alias LIKE '${PFX}%'" >/dev/null
    # The units added while proving the review desk's way out of a dead end.
    pg "DELETE FROM part_unit WHERE symbol IN ('Mbps','${PFX}bps','${PFX}Gbps')
          OR quantity_kind='${PFX}_rate'" >/dev/null
}
cleanup; trap cleanup EXIT
auth_or_die

jq_(){ python3 -c "import json,sys; d=json.load(sys.stdin); print($1)" 2>/dev/null; }
res(){ python3 -c "import json,sys; print(json.dumps(json.load(sys.stdin).get('result')))" 2>/dev/null; }

# -------------------------------------------------------------------------
sec "1. the vocabulary is seeded, and describes itself"
# -------------------------------------------------------------------------
NK=$(pg "SELECT count(*) FROM part_parameter_keyword WHERE active")
t_ge "$NK" 15 "keywords seeded"
for k in Resistance Capacitance Tolerance Power Frequency; do
    t_eq "1" "$(pg "SELECT count(*) FROM part_parameter_keyword WHERE name='$k'")" \
         "'$k' is in the vocabulary"
done
t_ge "$(pg "SELECT count(*) FROM part_parameter_alias")" 20 "with the spellings datasheets use"
# Advice, not a parameter: these two are the ways a lookup produced a
# plausible, wrong number before anyone noticed.
# pgv, not pg: pg() strips spaces, and these assertions are about sentences.
t_contains "$(pgv "SELECT advice FROM part_parameter_keyword WHERE name='Package'")" \
           "footprint" "'Package' says to use the footprint field instead"
t_contains "$(pgv "SELECT advice FROM part_parameter_keyword WHERE name='Operating Temperature'")" \
           "two parameters" "'Operating Temperature' says to send a range as two parameters"

D=$(call part.lookup describe '[{}]' | res)
printf '%s' "$D" | grep -q '"Resistance"' && ok "describe publishes the vocabulary, not whatever is in use" \
    || no "describe has no Resistance: $(printf '%s' "$D" | head -c 120)"
printf '%s' "$D" | grep -q 'parameter_vocabulary' && ok "and publishes the aliases, so an agent can get it right first time" \
    || no "describe carries no parameter_vocabulary"

# -------------------------------------------------------------------------
sec "2. suggest — the four answers"
# -------------------------------------------------------------------------
S=$(call part.parameter.keyword suggest '[["Resistance","Ohms","Tolerence","Blorptivity"]]' | res)
echo "    $(printf '%s' "$S" | head -c 400)"
KIND(){ printf '%s' "$S" | python3 -c "import json,sys; print([r['kind'] for r in json.load(sys.stdin)][$1])" 2>/dev/null; }
CANON(){ printf '%s' "$S" | python3 -c "import json,sys; print([r['canonical'] for r in json.load(sys.stdin)][$1])" 2>/dev/null; }
t_eq "exact"   "$(KIND 0)" "a name we have is 'exact'"
t_eq "alias"   "$(KIND 1)" "a known spelling is 'alias'"
t_eq "Resistance" "$(CANON 1)" "and resolves to the canonical name"
t_eq "similar" "$(KIND 2)" "a misspelling is 'similar'"
t_eq "Tolerance"  "$(CANON 2)" "and names what it is probably meant to be"
t_eq "unknown" "$(KIND 3)" "something genuinely new is 'unknown'"
ACT=$(printf '%s' "$S" | python3 -c "import json,sys; print([r['action'] for r in json.load(sys.stdin)][3])" 2>/dev/null)
t_contains "$ACT" "add as a new parameter" "and the suggested action says so in words"

# Punctuation and case are not different names.
S2=$(call part.parameter.keyword suggest '[["RESISTANCE","resistance (ohm)","  Resistance  "]]' | res)
t_eq "exact" "$(printf '%s' "$S2" | python3 -c "import json,sys; print(json.load(sys.stdin)[0]['kind'])" 2>/dev/null)" \
     "case alone is not a new parameter"
t_eq "Resistance" "$(printf '%s' "$S2" | python3 -c "import json,sys; print(json.load(sys.stdin)[2]['canonical'])" 2>/dev/null)" \
     "nor is surrounding whitespace"

# -------------------------------------------------------------------------
sec "3. submit — a new name is a QUESTION, never an error"
# -------------------------------------------------------------------------
SUB=$(call part.lookup submit '[{"query":"'${PFX}' mosfet","mpn":"'${PFX}'-M1",
      "parameters":[{"name":"Rds(on)","value":"22m","unit":"Ω"},
                    {"name":"Ohms","value":"4k7","unit":"Ω"},
                    {"name":"Tolerence","value":"1","unit":"%"}]}]' | res)
echo "    $(printf '%s' "$SUB" | head -c 300)"
LID=$(printf '%s' "$SUB" | jq_ "d['id']")
t_nonempty "$LID" "the part was staged"
t_eq "True" "$(printf '%s' "$SUB" | jq_ "d['ok']")" \
     "and staged OK — an unknown parameter name does not fail the submit"
t_eq "pending" "$(pg "SELECT state FROM part_lookup_result WHERE id=${LID:-0}")" \
     "it is pending review, not 'invalid'"

ISS=$(pgv "SELECT issues::text FROM part_lookup_result WHERE id=${LID:-0}")
t_lacks "$ISS" '"level":"error"' "no issue is an error"
t_contains "$ISS" "not in the parameter vocabulary" "the new name is reported as a question"
t_contains "$ISS" "Closest existing parameter" "the near-miss names what it is probably meant to be"

# A known spelling is CORRECTED in the stored payload, the way a unit symbol
# is — so the reviewer sees the name the catalogue uses.
PAY=$(pgv "SELECT payload::text FROM part_lookup_result WHERE id=${LID:-0}")
t_contains "$PAY" '"Resistance"' "a known spelling is rewritten to the canonical name"
t_lacks    "$PAY" '"Ohms"'       "and the alias is not what gets stored"
t_contains "$ISS" "recorded as" "with the rewrite reported, not done silently"

# -------------------------------------------------------------------------
sec "4. adopting a new name"
# -------------------------------------------------------------------------
NEW=$(call part.parameter.keyword create_keyword \
      '[{"name":"'${PFX}' Channel Resistance","quantity_kind":"resistance","aliases":["Rds(on)","RDS ON"]}]' | res)
KID=$(printf '%s' "$NEW" | jq_ "d['id']")
t_nonempty "$KID" "a keyword can be added"
t_eq "alias" "$(call part.parameter.keyword suggest '[["rds_on"]]' | res | jq_ "d[0]['kind']")" \
     "and its aliases resolve immediately — punctuation and all"
t_eq "${PFX} Channel Resistance" "$(call part.parameter.keyword suggest '[["RDS(ON)"]]' | res | jq_ "d[0]['canonical']")" \
     "however the datasheet spells it"

# The two vocabularies must not overlap: a name that already MEANS something
# cannot quietly become a spelling of something else.
BAD=$(call part.parameter.keyword add_alias "[{\"keyword_id\":${KID:-0},\"alias\":\"Resistance\"}]")
has_error "$BAD" && ok "an existing parameter cannot be turned into an alias of another" \
                 || no "aliasing over a real parameter was allowed"
BAD2=$(call part.parameter.keyword create_keyword '[{"name":"Ohms"}]')
has_error "$BAD2" && ok "nor can a known spelling be promoted behind the vocabulary's back" \
                  || no "'Ohms' was accepted as a parameter of its own"

# -------------------------------------------------------------------------
sec "5. merge — the answer to 'put it under our existing parameters'"
# -------------------------------------------------------------------------
PROD=$(call product.product create '[{"name":"'${PFX}' Resistor","type":"product"}]' \
       | python3 -c "import json,sys; print(json.load(sys.stdin).get('result',''))" 2>/dev/null)
UO=$(pg "SELECT id FROM part_unit WHERE symbol='Ω'")
call part.parameter create "[{\"product_id\":${PROD:-0},\"name\":\"${PFX} Ohmic Value\",\"value_numeric\":4700,\"unit_id\":$UO}]" >/dev/null
t_eq "1" "$(pg "SELECT count(*) FROM part_parameter WHERE name='${PFX} Ohmic Value'")" \
     "a product carries a name the vocabulary does not know"

UNM=$(call part.parameter.keyword unmatched '[{}]' | res)
printf '%s' "$UNM" | grep -q "${PFX} Ohmic Value" \
    && ok "the screen's 'unmatched' list finds it" || no "unmatched missed it: $(printf '%s' "$UNM" | head -c 200)"

RESID=$(pg "SELECT id FROM part_parameter_keyword WHERE name='Resistance'")
MG=$(call part.parameter.keyword merge \
     "[{\"from_name\":\"${PFX} Ohmic Value\",\"into_keyword_id\":$RESID}]" | res)
echo "    merge -> $MG"
t_eq "1" "$(printf '%s' "$MG" | jq_ "d['renamed']")" "merging renames the parameter rows it covers"
t_eq "0" "$(pg "SELECT count(*) FROM part_parameter WHERE name='${PFX} Ohmic Value'")" \
     "the old name is gone from the catalogue"
t_eq "Resistance" "$(pg "SELECT name FROM part_parameter WHERE product_id=${PROD:-0}")" \
     "the part is now filed under Resistance, where a search will find it"
t_eq "1" "$(pg "SELECT count(*) FROM part_parameter_alias WHERE alias='${PFX} Ohmic Value'")" \
     "and the old spelling survives as an alias, so nobody is asked twice"
t_eq "alias" "$(call part.parameter.keyword suggest "[[\"${PFX} ohmic value\"]]" | res | jq_ "d[0]['kind']")" \
     "the next payload using it resolves silently"

# Merging a keyword into another takes its aliases with it.
TMP=$(call part.parameter.keyword create_keyword '[{"name":"'${PFX}' Spare","aliases":["'${PFX}' Spare Two"]}]' | res)
TID=$(printf '%s' "$TMP" | jq_ "d['id']")
call part.parameter.keyword merge "[{\"from_keyword_id\":${TID:-0},\"into_keyword_id\":$RESID}]" > /dev/null
t_eq "0" "$(pg "SELECT count(*) FROM part_parameter_keyword WHERE id=${TID:-0}")" "the merged keyword is gone"
t_eq "$RESID" "$(pg "SELECT keyword_id FROM part_parameter_alias WHERE alias='${PFX} Spare Two'")" \
     "its aliases moved to the keyword it was merged into"

# -------------------------------------------------------------------------
sec "6. the screen it is all for"
# -------------------------------------------------------------------------
t_eq "1" "$(pg "SELECT count(*) FROM ir_ui_menu WHERE id=88 AND name='Parameter Keywords' AND parent_id=52")" \
     "Products ▸ Configuration ▸ Parameter Keywords exists"
t_eq "part.parameter.keyword" "$(pg "SELECT res_model FROM ir_act_window WHERE id=130")" \
     "and opens the vocabulary screen"
L=$(call part.parameter.keyword list '[{}]' | res)
printf '%s' "$L" | grep -q '"aliases"' && ok "the list carries each keyword's aliases" || no "no aliases in the list"
printf '%s' "$L" | grep -q '"uses"'    && ok "and how many parameters actually use it" || no "no usage count"

# -------------------------------------------------------------------------
sec "7. the two dead ends on the review desk"
# -------------------------------------------------------------------------
# Reported with a screenshot: a 32-bit MCU proposal stuck at INVALID on
# "Cannot read value 'MIPS32 M4K'" and "Unknown unit 'Mbps'". Both are the
# normal consequence of a catalogue that is still growing — a new part brings
# a unit nobody has entered and a value that is not a magnitude — and neither
# had any way out of the screen.
MCU=$(call part.lookup submit '[{"query":"'${PFX}' mcu","mpn":"'${PFX}'-MCU1",
      "parameters":[{"name":"core","value":"MIPS32 M4K"},
                    {"name":"frequency_max","value":"80","unit":"MHz"},
                    {"name":"ethernet","value":"10/100","unit":"Mbps"}]}]' | res)
MID=$(printf '%s' "$MCU" | jq_ "d['id']")
t_nonempty "$MID" "the MCU proposal is staged"
t_eq "invalid" "$(printf '%s' "$MCU" | jq_ "d['state']")" \
     "and is invalid, exactly as reported — two errors it cannot clear by itself"

MISS=$(pgv "SELECT issues::text FROM part_lookup_result WHERE id=${MID:-0}")
# The stored JSON is pretty-printed, so match the value rather than a
# hand-written `"kind":"…"` pair that only holds while nothing reformats it.
t_contains "$MISS" 'add_unit'     "the unknown unit carries what is needed to add it"
t_contains "$MISS" 'keep_as_text' "the unreadable value offers to be kept as text"
# A pair is not a magnitude. "10/100" parses as 10, and the second speed was
# silently dropped — the same failure as "-55 to 125" reading as -55.
t_contains "$MISS" "not a single number" "'10/100' is reported as a pair, not read as 10"

# --- adding the unit ---
QS=$(call part.unit quantities '[{}]' | res)
t_contains "$QS" 'resistance' "the quantities on offer are the ones units already measure"
U=$(call part.unit create_unit '[{"symbol":"Mbps","name":"Megabit per second","quantity_kind":"'${PFX}'_rate"}]' | res)
echo "    create_unit -> $U"
t_eq "True" "$(printf '%s' "$U" | jq_ "d['is_base']")" \
     "the first unit of a new quantity becomes its base"
t_eq "1.0"  "$(printf '%s' "$U" | jq_ "float(d['factor'])")" "with a factor of 1, by definition"
DUP=$(call part.unit create_unit '[{"symbol":"Mbps","name":"Again"}]')
has_error "$DUP" && ok "the same symbol cannot be added twice" || no "a duplicate unit was accepted"
# A second unit of a known quantity must say how it converts, or every value
# in it is multiplied against nothing.
BAD=$(call part.unit create_unit "[{\"symbol\":\"${PFX}bps\",\"name\":\"Bit per second\",\"quantity_kind\":\"${PFX}_rate\",\"factor\":1}]")
has_error "$BAD" && ok "a second unit with no real factor is refused, and says what to give" \
                 || no "a factor-1 rival to the base was accepted"
GOOD=$(call part.unit create_unit "[{\"symbol\":\"${PFX}Gbps\",\"name\":\"Gigabit per second\",\"quantity_kind\":\"${PFX}_rate\",\"factor\":1000}]" | res)
t_eq "False" "$(printf '%s' "$GOOD" | jq_ "d['is_base']")" "a converting unit joins the quantity, not as its base"

# --- clearing both errors, the way the buttons do ---
UPD=$(call part.lookup update '[{"id":'${MID:-0}',
      "parameters":[{"name":"core","value":"MIPS32 M4K","text":true},
                    {"name":"frequency_max","value":"80","unit":"MHz"},
                    {"name":"ethernet","value":"10/100","unit":"Mbps"}]}]' | res)
echo "    after both fixes -> $(printf '%s' "$UPD" | head -c 120)"
t_eq "pending" "$(printf '%s' "$UPD" | jq_ "d['state']")" \
     "with the unit added and the value kept as text, the proposal is pending again"
t_eq "True" "$(printf '%s' "$UPD" | jq_ "d['ok']")" "and has no errors left"
t_contains "$(pgv "SELECT issues::text FROM part_lookup_result WHERE id=${MID:-0}")" \
           "kept as text" "the decision is recorded, not hidden"

# --- and it applies, with both values intact ---
AP=$(call part.lookup apply "[{\"id\":${MID:-0}}]" | res)
PID=$(printf '%s' "$AP" | jq_ "d['product_id']")
t_nonempty "$PID" "it can now be applied to a product"
t_eq "MIPS32 M4K" "$(pgv "SELECT value_text FROM part_parameter WHERE product_id=${PID:-0} AND name='core'" | sed 's/^ *//;s/ *$//')" \
     "the text value is stored whole, not as a number"
t_eq "10/100" "$(pgv "SELECT value_text FROM part_parameter WHERE product_id=${PID:-0} AND name='ethernet'" | sed 's/^ *//;s/ *$//')" \
     "and the pair keeps BOTH speeds — it used to be stored as 10"
t_eq "Mbps" "$(pg "SELECT u.symbol FROM part_parameter p JOIN part_unit u ON u.id=p.unit_id
                    WHERE p.product_id=${PID:-0} AND p.name='ethernet'")" \
     "against the unit that was just added"
t_eq "80000000" "$(pg "SELECT value_base::numeric(20,0) FROM part_parameter
                        WHERE product_id=${PID:-0} AND name='frequency_max'")" \
     "while the parameter that IS a number is still normalised"
pg "DELETE FROM part_parameter WHERE product_id=${PID:-0}" >/dev/null
pg "DELETE FROM product_product WHERE id=${PID:-0}" >/dev/null

verdict

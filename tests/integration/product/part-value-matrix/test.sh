#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# The value/unit matrix, across the component families a real catalogue holds.
#
# This exists because of a bug that was invisible until somebody looked at the
# numbers: `std::to_string(double)` formats with SIX decimal places, so 1e-8
# arrived at the database as the string "0.000000". Every nanofarad, picofarad
# and nanohenry written with the prefix in the value — "10n", "4n7", "10p" —
# was stored as ZERO. That is most of the passive components in any real
# catalogue, silently, with nothing logged and nothing refused.
#
# It survived because the tests only ever used 4k7 and 125m: magnitudes big
# enough to fit in six decimals. A matrix is the fix — the range of a value is
# a dimension of the input, and testing one point on it tests almost nothing.
#
# So this walks the ranges components are actually specified in:
#
#     pF ── nF ── µF ── mF ── F        capacitors
#     nH ── µH ── mH ── H              inductors and ferrite beads
#     mΩ ── Ω ── kΩ ── MΩ              resistors, bead DC resistance
#     Hz ── kHz ── MHz ── GHz          crystals, filters
#
# and the notations they are written in: 4k7, 3V3, 6V3, 2R2, 32k768, 100n.
# =============================================================
auth_or_die

cleanup() {
    pg "DELETE FROM part_parameter WHERE product_id IN
          (SELECT id FROM product_product WHERE default_code LIKE 'QA-VM%');
        DELETE FROM part_manufacturer_info WHERE product_id IN
          (SELECT id FROM product_product WHERE default_code LIKE 'QA-VM%');
        DELETE FROM part_lookup_result WHERE query LIKE 'QA-VM%';
        DELETE FROM product_product  WHERE default_code LIKE 'QA-VM%';
        DELETE FROM product_template WHERE name LIKE 'QA-VM%'" >/dev/null
}
cleanup; trap cleanup EXIT

# stage_apply <tag> <parameters-json> -> prints the new product id
stage_apply() {
    local tag="$1" params="$2" r id
    r=$(call part.lookup submit "[{\"query\":\"QA-VM $tag\",\"mpn\":\"QA-VM-$tag\",
         \"name\":\"QA-VM $tag\",\"confidence\":0.9,\"parameters\":$params}]")
    if echo "$r" | grep -q '"state":"invalid"'; then
        echo "INVALID: $(echo "$r" | python3 -c 'import sys,json
print(" ".join(i["message"] for i in json.load(sys.stdin)["result"]["issues"]))' 2>/dev/null)"
        return
    fi
    id=$(echo "$r" | python3 -c 'import sys,json; print(json.load(sys.stdin)["result"]["id"])' 2>/dev/null)
    call part.lookup apply "[{\"id\":$id}]" \
        | python3 -c 'import sys,json; print(json.load(sys.stdin)["result"]["product_id"])' 2>/dev/null
}

# base_is <product> <param> <expected> <label>
# Compared with a relative tolerance: these are doubles, and 10 * 1e-6 is not
# bit-identical to 1e-5. Demanding equality would fail on arithmetic, not data.
base_is() {
    local got
    got=$(pg "SELECT CASE WHEN value_base IS NULL THEN 'null'
                          WHEN abs(value_base - ($3)) <= abs($3)*1e-9 + 1e-30 THEN 'ok'
                          ELSE value_base::text END
              FROM part_parameter WHERE product_id=$1 AND name='$2'")
    t_eq "ok" "$got" "$4"
}

# =============================================================
sec "1. the vocabulary covers the families a catalogue actually holds"
for kind in capacitance inductance resistance voltage current power frequency time temperature ratio length; do
    t_ge "$(pg "SELECT count(*) FROM part_unit WHERE quantity_kind='$kind'")" 1 "$kind units exist"
done
# The decade steps are what a range search walks across.
for s in 'pF' 'nF' 'µF' 'mF' 'F' 'nH' 'µH' 'mH' 'H' 'mΩ' 'Ω' 'kΩ' 'MΩ' 'Hz' 'kHz' 'MHz' 'GHz'; do
    t_eq "1" "$(pg "SELECT count(*) FROM part_unit WHERE symbol='$s'")" "$s is known"
done

sec "2. ASCII spellings of the micro sign"
# Datasheets, distributor listings and every CSV export write uF, not µF. The
# glyph is not on a keyboard and does not survive most tooling. Refusing it
# meant refusing the way capacitance is written essentially everywhere.
P=$(stage_apply ASCII '[{"name":"capacitance","value":"10","unit":"uF"},
                        {"name":"inductance","value":"4","unit":"uH"},
                        {"name":"current","value":"500","unit":"uA"},
                        {"name":"resistance","value":"100","unit":"ohm"}]')
t_nonempty "$P" "a result using uF / uH / uA / ohm is accepted ($P)"
base_is "$P" capacitance 1e-5   "uF is read as µF"
base_is "$P" inductance  4e-6   "uH is read as µH"
base_is "$P" current     5e-4   "uA is read as µA"
base_is "$P" resistance  100    "\"ohm\" is read as Ω"
# It must be STORED canonically, not just accepted, or the facets split in two.
t_eq "µF" "$(pgv "SELECT u.symbol FROM part_parameter pa JOIN part_unit u ON u.id=pa.unit_id
                  WHERE pa.product_id=$P AND pa.name='capacitance'" | xargs)" \
     "and stored against the canonical symbol"

sec "3. capacitors, across the decades"
# The whole range, because six decimal places of precision covered µF and lost
# everything smaller.
C=$(stage_apply CAPS '[{"name":"c_pf","value":"10p","unit":"F"},
                       {"name":"c_nf","value":"10n","unit":"F"},
                       {"name":"c_100n","value":"100n","unit":"F"},
                       {"name":"c_uf","value":"10u","unit":"F"},
                       {"name":"c_4u7","value":"4u7","unit":"F"},
                       {"name":"c_mf","value":"10m","unit":"F"}]')
t_nonempty "$C" "a capacitor across six decades is accepted"
base_is "$C" c_pf   1e-11 "10p F  -> 1e-11"
base_is "$C" c_nf   1e-8  "10n F  -> 1e-8"
base_is "$C" c_100n 1e-7  "100n F -> 1e-7"
base_is "$C" c_uf   1e-5  "10u F  -> 1e-5"
base_is "$C" c_4u7  4.7e-6 "4u7 F  -> 4.7e-6 (prefix as the decimal point)"
base_is "$C" c_mf   1e-2  "10m F  -> 1e-2"

sec "4. the same capacitance written the other way round"
# "10n" + F and "10" + nF must land on the same number, or a range search finds
# one and not the other.
C2=$(stage_apply CAPS2 '[{"name":"c_pf","value":"10","unit":"pF"},
                         {"name":"c_nf","value":"10","unit":"nF"},
                         {"name":"c_uf","value":"10","unit":"µF"}]')
base_is "$C2" c_pf 1e-11 "10 pF  -> 1e-11"
base_is "$C2" c_nf 1e-8  "10 nF  -> 1e-8"
base_is "$C2" c_uf 1e-5  "10 µF  -> 1e-5"
t_eq "1" "$(pg "SELECT (abs(a.value_base - b.value_base) < 1e-20)::int
                FROM part_parameter a, part_parameter b
                WHERE a.product_id=$C AND a.name='c_nf'
                  AND b.product_id=$C2 AND b.name='c_nf'")" \
     "both notations land on exactly the same base value"

sec "5. inductors and ferrite beads"
L=$(stage_apply IND '[{"name":"inductance","value":"10n","unit":"H"},
                      {"name":"l_1uh","value":"1u","unit":"H"},
                      {"name":"l_4n7","value":"4n7","unit":"H"},
                      {"name":"l_mh","value":"2m2","unit":"H"}]')
t_nonempty "$L" "an inductor is accepted"
base_is "$L" inductance 1e-8   "10n H -> 1e-8"
base_is "$L" l_1uh      1e-6   "1u H  -> 1e-6"
base_is "$L" l_4n7      4.7e-9 "4n7 H -> 4.7e-9"
base_is "$L" l_mh       2.2e-3 "2m2 H -> 2.2e-3"

# A ferrite bead is specified by its impedance AT a frequency, plus a DC
# resistance in milliohms and a rated current — three different quantities on
# one part, which is exactly where a per-kind base value earns its keep.
FB=$(stage_apply BEAD '[{"name":"impedance","value":"600","unit":"Ω"},
                        {"name":"test_frequency","value":"100","unit":"MHz"},
                        {"name":"dc_resistance","value":"25","unit":"mΩ"},
                        {"name":"rated_current","value":"2","unit":"A"}]')
t_nonempty "$FB" "a ferrite bead is accepted"
base_is "$FB" impedance      600   "600 Ω impedance"
base_is "$FB" test_frequency 1e8   "100 MHz -> 1e8"
base_is "$FB" dc_resistance  0.025 "25 mΩ -> 0.025 (not 25)"
base_is "$FB" rated_current  2     "2 A"

sec "6. the notations components are actually marked with"
N=$(stage_apply NOTE '[{"name":"r_4k7","value":"4k7","unit":"Ω"},
                       {"name":"r_2r2","value":"2R2","unit":"Ω"},
                       {"name":"r_1m5","value":"1M5","unit":"Ω"},
                       {"name":"v_3v3","value":"3V3","unit":"V"},
                       {"name":"v_6v3","value":"6V3","unit":"V"},
                       {"name":"f_32k768","value":"32k768","unit":"Hz"},
                       {"name":"p_125m","value":"125m","unit":"W"}]')
t_nonempty "$N" "the shorthand notations are accepted"
base_is "$N" r_4k7    4700    "4k7 -> 4700"
base_is "$N" r_2r2    2.2     "2R2 -> 2.2"
base_is "$N" r_1m5    1.5e6   "1M5 -> 1.5e6"
# 3V3 and 6V3 are how every regulator and electrolytic is marked.
base_is "$N" v_3v3    3.3     "3V3 -> 3.3"
base_is "$N" v_6v3    6.3     "6V3 -> 6.3"
base_is "$N" f_32k768 32768   "32k768 -> 32768 (the watch crystal)"
base_is "$N" p_125m   0.125   "125m W -> 0.125"

sec "7. packages are identifiers, not numbers"
# "0603" parses fine as 603 — and that is the problem. The package belongs in
# the footprint field; when it arrives as a parameter anyway, the leading zero
# must survive, because 603 is not a package.
K=$(stage_apply PKG '[{"name":"package","value":"0603","unit":""},
                      {"name":"case_code","value":"0805","unit":""},
                      {"name":"pin_count","value":"8","unit":""}]')
t_nonempty "$K" "an identifier-valued parameter is accepted"
t_eq "0603" "$(pg "SELECT value_text FROM part_parameter WHERE product_id=$K AND name='package'")" \
     "0603 keeps its leading zero"
t_eq "0805" "$(pg "SELECT value_text FROM part_parameter WHERE product_id=$K AND name='case_code'")" \
     "and so does 0805"
t_eq "" "$(pg "SELECT COALESCE(value_base::text,'') FROM part_parameter
               WHERE product_id=$K AND name='package'")" \
     "a package has no numeric base — it is not a quantity"
# A genuine count is still a number, or every integer becomes a label.
t_eq "" "$(pg "SELECT COALESCE(value_text,'') FROM part_parameter
               WHERE product_id=$K AND name='pin_count'")" \
     "but a plain 8 is still numeric"
base_is "$K" pin_count 8 "and keeps its value"

sec "7b. a range crammed into one value is not read as a number"
# Observed from a live lookup: operating_temperature = "-55 to 125" °C.
# parseSiValue takes the first number, so the part would have recorded -55 °C
# as its specification — a plausible-looking figure with the upper limit gone.
RG=$(stage_apply RANGE '[{"name":"operating_temperature","value":"-55 to 125","unit":"°C"},
                         {"name":"supply","value":"3~5","unit":"V"},
                         {"name":"single_temp","value":"-40","unit":"°C"}]')
t_nonempty "$RG" "a range-valued parameter is accepted"
t_eq "-55to125" "$(pg "SELECT value_text FROM part_parameter
                       WHERE product_id=$RG AND name='operating_temperature'")" \
     "the whole range is kept as text, not truncated to -55"
t_eq "" "$(pg "SELECT COALESCE(value_base::text,'') FROM part_parameter
               WHERE product_id=$RG AND name='operating_temperature'")" \
     "and it claims no numeric value it cannot honestly hold"
t_eq "3~5" "$(pg "SELECT value_text FROM part_parameter WHERE product_id=$RG AND name='supply'")" \
     "a tilde range is caught too"
# A genuine negative number must still be a number.
base_is "$RG" single_temp -40 "but a plain -40 is still numeric"

sec "8. the footprint field is where a package really belongs"
FPN=$(pgv "SELECT name FROM part_footprint ORDER BY id LIMIT 1" | xargs)
if [ -n "$FPN" ]; then
    FP=$(stage_apply FOOT "[{\"name\":\"resistance\",\"value\":\"1k\",\"unit\":\"Ω\"}]" )
    # submit carries `footprint` through the payload; apply resolves it.
    FR=$(call part.lookup submit "[{\"query\":\"QA-VM FP2\",\"mpn\":\"QA-VM-FP2\",
          \"name\":\"QA-VM FP2\",\"footprint\":\"$FPN\",
          \"parameters\":[{\"name\":\"resistance\",\"value\":\"1k\",\"unit\":\"Ω\"}]}]")
    FID=$(echo "$FR" | python3 -c 'import sys,json; print(json.load(sys.stdin)["result"]["id"])')
    FPID=$(call part.lookup apply "[{\"id\":$FID}]" \
           | python3 -c 'import sys,json; print(json.load(sys.stdin)["result"]["product_id"])')
    t_eq "$FPN" "$(pgv "SELECT f.name FROM product_product p
                        JOIN part_footprint f ON f.id=p.footprint_id
                        WHERE p.id=$FPID" | xargs)" \
         "a known footprint is attached to the product"
else
    echo "    NOTE  no footprints are seeded — the footprint path was not checked."
fi

sec "9. a quantity cannot match across kinds"
# 10 nF and 10 nH are both 1e-8 in base. Only the quantity kind keeps a
# capacitance search from returning an inductor.
RES=$(call part.parameter search_parts '[{"min":"1p","max":"1","unit":"F"}]')
t_contains "$RES" "\"id\":$C"  "a capacitance range finds the capacitor"
t_lacks    "$RES" "\"id\":$L"  "and never the inductor that shares its number"
RES=$(call part.parameter search_parts '[{"min":"1p","max":"1","unit":"H"}]')
t_contains "$RES" "\"id\":$L"  "an inductance range finds the inductor"
t_lacks    "$RES" "\"id\":$C"  "and not the capacitor"

sec "10. a range search reaches the small end"
# This is the assertion that would have caught the truncation bug: before the
# fix every one of these was stored as 0, so a picofarad range found nothing
# while a microfarad range worked perfectly.
RES=$(call part.parameter search_parts '[{"name":"c_pf","min":"5p","max":"20p","unit":"F"}]')
t_contains "$RES" "\"id\":$C" "a 5p–20p search finds the 10 pF part"
RES=$(call part.parameter search_parts '[{"name":"c_nf","min":"9n","max":"11n","unit":"F"}]')
t_contains "$RES" "\"id\":$C" "a 9n–11n search finds the 10 nF part"
RES=$(call part.parameter search_parts '[{"name":"l_4n7","min":"4n","max":"5n","unit":"H"}]')
t_contains "$RES" "\"id\":$L" "a 4n–5n search finds the 4n7 inductor"
RES=$(call part.parameter search_parts '[{"name":"c_pf","min":"1n","max":"1","unit":"F"}]')
t_lacks "$RES" "\"id\":$C" "and a 1n–1F search excludes the 10 pF part"

sec "11. a connector: counts, pitch and current on one part"
CN=$(stage_apply CONN '[{"name":"pin_count","value":"40","unit":""},
                        {"name":"pitch","value":"2.54","unit":"mm"},
                        {"name":"current_rating","value":"3","unit":"A"},
                        {"name":"voltage_rating","value":"250","unit":"V"},
                        {"name":"operating_temperature","value":"85","unit":"°C"}]')
t_nonempty "$CN" "a connector is accepted"
base_is "$CN" pin_count 40      "40 pins, dimensionless"
base_is "$CN" pitch     0.00254 "2.54 mm -> 0.00254 m"
base_is "$CN" current_rating 3  "3 A"
base_is "$CN" voltage_rating 250 "250 V"
base_is "$CN" operating_temperature 85 "85 °C"

verdict

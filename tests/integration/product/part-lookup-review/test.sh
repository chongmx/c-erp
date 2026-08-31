#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# Part Lookup — the REVIEW DESK (docs/097, docs/110).
#
# The sibling `part-lookup` test covers the agent-facing contract: describe,
# submit, apply, and the unit normalisation underneath. This one covers what a
# PERSON does with a proposal afterwards — edit it, correct it, reject it,
# refuse it — and the state machine that keeps those honest.
#
# The state machine is the whole safety story, so it is tested as a machine
# rather than one transition at a time:
#
#     submit ──► pending ──────► applied      (a person said yes)
#            └─► invalid  ──┘    rejected     (a person said no)
#                   ▲   │
#                   └───┘  update: re-validated every time
#
# What must hold, and what each rule is protecting:
#
#   * `invalid` cannot be applied      — an unreadable value must not reach a
#                                        catalogue somebody orders parts from
#   * `applied` cannot be edited,      — it is the record of what was written;
#     rejected or re-applied             editing it would erase the evidence
#   * `confidence` cannot be edited    — it is the agent's claim, and a human
#                                        overwriting it destroys the one signal
#                                        saying how hard to check the rest
#   * a missing id is an ERROR         — "ok" for a row that does not exist is
#                                        the failure mode that hides the others
# =============================================================
auth_or_die

cleanup() {
    pg "DELETE FROM part_parameter WHERE product_id IN
          (SELECT id FROM product_product WHERE default_code LIKE 'QA-RV%');
        DELETE FROM part_manufacturer_info WHERE product_id IN
          (SELECT id FROM product_product WHERE default_code LIKE 'QA-RV%');
        DELETE FROM part_lookup_result WHERE query LIKE 'QA-RV%';
        DELETE FROM product_product  WHERE default_code LIKE 'QA-RV%';
        DELETE FROM product_template WHERE name LIKE 'QA-RV%';
        DELETE FROM res_partner WHERE name='QA-RV Mfr'" >/dev/null
}
cleanup; trap cleanup EXIT

# stage <query-suffix> [extra-json] -> prints the new proposal id
stage() {
    local q="QA-RV $1" extra="${2:-}"
    local body="{\"query\":\"$q\",\"mpn\":\"QA-RV-$1\",\"manufacturer\":\"QA-RV Mfr\",
                 \"name\":\"QA-RV Part $1\",\"confidence\":0.8${extra:+,$extra}}"
    call part.lookup submit "[$body]" \
        | python3 -c 'import sys,json; print(json.load(sys.stdin)["result"]["id"])' 2>/dev/null
}

GOOD_P='"parameters":[{"name":"resistance","value":"4k7","unit":"Ω"}]'
BAD_P='"parameters":[{"name":"resistance","value":"4k7","unit":"furlongs"}]'

# =============================================================
sec "1. a proposal starts staged, and stages nothing else"
P1=$(stage A "$GOOD_P")
t_nonempty "$P1" "submit returned an id"
t_eq "pending" "$(pg "SELECT state FROM part_lookup_result WHERE id=$P1")" "a clean result is pending"
t_eq "0" "$(pg "SELECT count(*) FROM product_product WHERE default_code='QA-RV-A'")" \
     "and no product exists yet"

sec "2. reading it back"
RD=$(call part.lookup read "[[$P1]]")
t_contains "$RD" '"payload"'    "read returns the full payload"
t_contains "$RD" '"issues"'     "and the issues list"
t_contains "$RD" '"confidence"' "and the confidence"
MISSING=$(call part.lookup read '[[999999]]')
has_error "$MISSING" && ok "reading an id that does not exist is an error" \
                     || no "read invented a result for a missing id"

sec "3. editing what the agent got wrong"
U=$(call part.lookup update "[{\"id\":$P1,\"manufacturer\":\"QA-RV Mfr\",\"name\":\"QA-RV Part A edited\"}]")
t_contains "$U" '"ok":true' "an edit is accepted"
t_eq "QA-RVPartAedited" "$(pg "SELECT payload->>'name' FROM part_lookup_result WHERE id=$P1")" \
     "the edited field is stored"
t_eq "QA-RVA" "$(pg "SELECT payload->>'query' FROM part_lookup_result WHERE id=$P1")" \
     "a field the editor never sent survives the merge"
t_eq "1" "$(pg "SELECT jsonb_array_length(payload->'parameters') FROM part_lookup_result WHERE id=$P1")" \
     "and so do the parameters"

sec "4. parameters can be replaced wholesale"
call part.lookup update "[{\"id\":$P1,\"parameters\":[
      {\"name\":\"resistance\",\"value\":\"10k\",\"unit\":\"Ω\"},
      {\"name\":\"tolerance\",\"value\":\"1\",\"unit\":\"%\"}]}]" >/dev/null
t_eq "2" "$(pg "SELECT jsonb_array_length(payload->'parameters') FROM part_lookup_result WHERE id=$P1")" \
     "a replaced parameter list takes effect"
call part.lookup update "[{\"id\":$P1,\"parameters\":[]}]" >/dev/null
t_eq "0" "$(pg "SELECT jsonb_array_length(payload->'parameters') FROM part_lookup_result WHERE id=$P1")" \
     "and every parameter can be removed"
call part.lookup update "[{\"id\":$P1,\"parameters\":[{\"name\":\"resistance\",\"value\":\"4k7\",\"unit\":\"Ω\"}]}]" >/dev/null

sec "5. confidence is reported, never edited"
# It is the AGENT'S statement about its own certainty. A reviewer overwriting
# it does not make the part more reliable — it destroys the one signal that
# says how hard the rest needs checking.
C0=$(pg "SELECT confidence FROM part_lookup_result WHERE id=$P1")
call part.lookup update "[{\"id\":$P1,\"confidence\":0.99}]" >/dev/null
t_eq "$C0" "$(pg "SELECT confidence FROM part_lookup_result WHERE id=$P1")" \
     "confidence survives an explicit attempt to change it"

sec "6. an edit is validated exactly like a submit"
BADU=$(call part.lookup update "[{\"id\":$P1,$BAD_P}]")
t_contains "$BADU" '"state":"invalid"' "a bad unit fails validation on edit"
t_contains "$BADU" 'Unknown unit'      "and says which one"
t_eq "invalid" "$(pg "SELECT state FROM part_lookup_result WHERE id=$P1")" "the proposal becomes invalid"

sec "7. an invalid proposal CANNOT be applied"
# This is the guarantee the whole staging design rests on: a value the ERP
# cannot read must never reach a catalogue somebody orders parts from.
AP=$(call part.lookup apply "[{\"id\":$P1}]")
has_error "$AP" && ok "applying an invalid proposal is refused" \
                || no "an invalid proposal was applied: $(echo "$AP" | head -c 160)"
t_eq "0" "$(pg "SELECT count(*) FROM product_product WHERE default_code='QA-RV-A'")" \
     "and it created no product"

sec "8. correcting it brings it back"
call part.lookup update "[{\"id\":$P1,$GOOD_P}]" >/dev/null
t_eq "pending" "$(pg "SELECT state FROM part_lookup_result WHERE id=$P1")" \
     "fixing the error returns it to pending"
AP=$(call part.lookup apply "[{\"id\":$P1}]")
t_contains "$AP" '"ok":true' "and now it applies"
NEWP=$(echo "$AP" | python3 -c 'import sys,json; print(json.load(sys.stdin)["result"]["product_id"])')
t_nonempty "$NEWP" "a product was created"
t_eq "4700" "$(pg "SELECT value_base::bigint FROM part_parameter
                   WHERE product_id=$NEWP AND name='resistance'")" \
     "the corrected value normalised to 4700"

sec "9. an applied proposal is frozen"
# It is the record of what was written to the catalogue. Editing, rejecting or
# re-applying it would leave no way to tell what actually happened.
FR=$(call part.lookup update "[{\"id\":$P1,\"manufacturer\":\"rewriting history\"}]")
has_error "$FR" && ok "it cannot be edited" || no "an applied proposal was edited"
FR=$(call part.lookup apply "[{\"id\":$P1}]")
has_error "$FR" && ok "it cannot be applied twice" || no "a proposal was applied twice"
FR=$(call part.lookup reject "[[$P1]]")
has_error "$FR" && ok "it cannot be rejected after the fact" \
                || no "an applied proposal was rejected — the record now lies"
t_eq "applied" "$(pg "SELECT state FROM part_lookup_result WHERE id=$P1")" "its state is unchanged"

sec "10. rejecting"
P2=$(stage B "$GOOD_P")
RJ=$(call part.lookup reject "[[$P2]]")
t_contains "$RJ" '"ok":true' "a pending proposal can be rejected"
t_eq "rejected" "$(pg "SELECT state FROM part_lookup_result WHERE id=$P2")" "it is marked, not deleted"
t_eq "0" "$(pg "SELECT count(*) FROM product_product WHERE default_code='QA-RV-B'")" \
     "and nothing was written"
# A rejected proposal must not then be applied — "no" has to mean no.
AR=$(call part.lookup apply "[{\"id\":$P2}]")
has_error "$AR" && ok "a rejected proposal cannot be applied" \
                || no "a rejected proposal was applied anyway"

sec "11. an id that does not exist is an error, not a success"
# "ok" for a row that was never touched is the failure mode that hides every
# other one: the screen reports success and the database never changed.
for m in reject apply; do
    case $m in
        reject) RESP=$(call part.lookup reject '[[999999]]') ;;
        apply)  RESP=$(call part.lookup apply  '[{"id":999999}]') ;;
    esac
    has_error "$RESP" && ok "$m refuses a missing id" || no "$m reported success for a missing id"
done
RESP=$(call part.lookup update '[{"id":999999,"mpn":"x"}]')
has_error "$RESP" && ok "update refuses a missing id" || no "update reported success for a missing id"

sec "12. apply refuses a product that does not exist"
# The reviewer types this id by hand. Without a check the parameters are
# written against a dangling id and the FK aborts the transaction, which
# surfaces as an unexplained 500.
P3=$(stage C "$GOOD_P")
BADPROD=$(call part.lookup apply "[{\"id\":$P3,\"product_id\":999999}]")
has_error "$BADPROD" && ok "an unknown product_id is refused" \
                     || no "apply accepted a product_id that does not exist"
t_eq "pending" "$(pg "SELECT state FROM part_lookup_result WHERE id=$P3")" \
     "and the proposal is left alone for another try"

sec "13. apply refuses a category that does not exist"
BADCAT=$(call part.lookup apply "[{\"id\":$P3,\"category_id\":999999}]")
has_error "$BADCAT" && ok "an unknown category_id is refused" \
                    || no "apply accepted a category_id that does not exist"

sec "14. applying onto an EXISTING product"
EXP=$(call product.product create '[{"name":"QA-RV Host","default_code":"QA-RV-HOST","type":"product"}]' | rid)
t_nonempty "$EXP" "a host product was created"
AP=$(call part.lookup apply "[{\"id\":$P3,\"product_id\":$EXP}]")
t_contains "$AP" '"ok":true' "the proposal applies onto it"
t_eq "$EXP" "$(pg "SELECT product_id FROM part_lookup_result WHERE id=$P3")" \
     "the proposal records which product it went to"
t_ge "$(pg "SELECT count(*) FROM part_parameter WHERE product_id=$EXP")" 1 \
     "and its parameters were written there"

sec "15. the queue"
LIST=$(call part.lookup search_read '[[["state","=","rejected"]]]')
t_contains "$LIST" "\"id\":$P2" "filtering by state finds the rejected one"
t_lacks    "$LIST" "\"id\":$P1" "and excludes the applied one"
ALL=$(call part.lookup search_read '[[]]')
t_contains "$ALL" "\"id\":$P1" "an empty domain returns everything"
t_contains "$ALL" '"issues"'   "each row carries an issue count for the list"

sec "16. submit still refuses what it cannot identify"
NOID=$(call part.lookup submit '[{"manufacturer":"QA-RV Mfr"}]')
has_error "$NOID" && ok "a result with neither query nor mpn is refused" \
                  || no "an unidentifiable result was staged"
NOTOBJ=$(call part.lookup submit '[["not","an","object"]]')
has_error "$NOTOBJ" && ok "a non-object body is refused" || no "a list was accepted as a result"

# =============================================================
sec "17. the screen itself"
# Everything above passes happily while the panel renders nothing: an OWL
# template is parsed as XML in the CLIENT, so a bad template throws in the
# browser and is invisible server-side. This is the only check that catches it
# — and the only one that can see whether the inputs are really editable.
if [ ! -d node_modules/puppeteer-core ]; then
    echo "    NOTE  puppeteer-core is not installed — the screen was NOT checked."
    echo "          npm i -D puppeteer-core     (see tests/docs/browser-render-checks.md)"
elif [ ! -x /usr/bin/google-chrome ]; then
    echo "    NOTE  Chrome is not at /usr/bin/google-chrome — the screen was NOT checked."
else
    # The driver asks the agent, so point it at the mock: a test that calls a
    # paid API is a test nobody can run. The operator's own choice is restored
    # afterwards either way.
    WAS=$(pg "SELECT provider FROM ir_ai_settings WHERE id=1")
    WASON=$(pg "SELECT enabled::int FROM ir_ai_settings WHERE id=1")
    call ir.ai.settings save '[{"provider":"mock","enabled":true}]' >/dev/null
    restore_ai() {
        call ir.ai.settings save \
            "[{\"provider\":\"$WAS\",\"enabled\":$([ "$WASON" = "1" ] && echo true || echo false)}]" >/dev/null
    }

    OUT=$(SHOT=/tmp/part-lookup.png timeout 200 node \
          tests/integration/product/part-lookup-review/drive.mjs 2>&1)
    RC=$?
    restore_ai
    pg "DELETE FROM part_lookup_result WHERE query='QA-UI-DRIVE'" >/dev/null

    jq_() { echo "$OUT" | python3 -c "
import json,sys
raw = sys.stdin.read(); s = raw.find('{')
d = json.loads(raw[s:raw.rfind('}')+1]) if s >= 0 else {}
cur = d
for k in '$1'.split('.'):
    cur = (cur or {}).get(k) if isinstance(cur, dict) else None
print(cur if cur is not None else '')" 2>/dev/null; }

    [ "$RC" = "0" ] && ok "the screen renders and drives with no console errors" \
                    || no "browser check failed (exit $RC): $(echo "$OUT" | tail -4 | tr '\n' ' ')"
    t_eq "True" "$(jq_ reached)"        "Part Lookup opened through the menus"
    t_ge "$(jq_ candidates)" 1          "asking drew at least one candidate"
    t_contains "$(jq_ badge)" "memory"  "the mock is labelled as not having searched"
    t_eq "True" "$(jq_ notes)"          "the agent's notes are shown"
    # The mock answers "4k7" + "kΩ" on purpose. The correction must reach the
    # reviewer's eyes, not just the server log.
    t_contains "$(jq_ adjustedShown)" "applied twice" "the double-multiplier warning is on the candidate"

    FIELDS=$(jq_ editorFields)
    t_contains "$FIELDS" "Part number"   "staging opens an editor"
    t_contains "$FIELDS" "Datasheet"     "with the datasheet field"
    t_lacks    "$FIELDS" "Confidence"    "and NO confidence field — it is the agent's claim, not ours"
    t_contains "$(jq_ confBadge.cls)" "pl-cbadge" "confidence is shown as a badge instead"
    t_ge "$(jq_ paramRows)" 3            "the parameter rows rendered"

    t_contains "$(jq_ savedNotice)" "Saved"          "an edit saves from the screen"
    t_eq "QA-UI Edited Mfr" "$(jq_ mfrAfterSave)"    "and the edit round-trips through the server"
    t_ge "$(jq_ rowsAfterAdd)" 4                     "a parameter row can be added"

    # A button that offers an apply the server will refuse reads as a broken
    # screen rather than as the rule it is.
    t_eq "True" "$(jq_ applyAfterInvalid.disabled)"  "Apply is disabled once the proposal is invalid"
    t_contains "$(jq_ applyAfterInvalid.label)" "Fix the errors" "and says why"
    t_ge "$(jq_ issuesShown)" 1                      "the issue is listed above the data"
    echo "    screenshots: /tmp/part-lookup.png, /tmp/part-lookup-invalid.png"
fi

verdict

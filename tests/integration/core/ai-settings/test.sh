#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# Settings -> AI Agent: ir.ai.settings (docs/110).
#
# This is a screen that holds a CREDENTIAL, so most of what follows asserts
# what does NOT come back. The key is write-only: stored on save, never
# returned by a read, never in an error message. One path returns it —
# reveal_for_setup — and it is admin-only and audited, because an operator
# sometimes has to paste it into a systemd unit.
#
# These assertions are the reason a future refactor cannot quietly start
# echoing the key: they fail the moment any read path includes it.
# =============================================================
auth_or_die

FAKE='sk-ant-api03-TESTKEYTESTKEYTESTKEYTESTKEY1234'

restore_state() {
    pg "UPDATE ir_ai_settings SET api_key='', enabled=FALSE, provider='anthropic',
        model='claude-sonnet-5', last_error='' WHERE id=1" >/dev/null
    # §7c applies a proposal, which is the only way to prove the stored base
    # value is right. Applying creates a real product, so it is undone here.
    pg "DELETE FROM part_parameter WHERE product_id IN
          (SELECT id FROM product_product WHERE default_code='MOCK-0001');
        DELETE FROM part_lookup_result WHERE query LIKE 'AI-GUARD%';
        DELETE FROM product_product  WHERE default_code='MOCK-0001';
        DELETE FROM product_template WHERE name LIKE 'Mock part for: AI-GUARD-%'" >/dev/null
    # Prompt overrides are deployment state, not test debris — but a test that
    # leaves one behind changes what every later AI call actually sends.
    pg "DELETE FROM ir_ai_prompt" >/dev/null
}
restore_state
trap 'restore_state' EXIT

sec "1. the settings row exists and is a singleton"
t_eq "1" "$(pg "SELECT count(*) FROM ir_ai_settings")" "exactly one settings row"
t_eq "1" "$(pg "SELECT id FROM ir_ai_settings")" "and it is id 1"
# Configuration lives in its own table, NOT in ir_config_parameter — that table
# has no access control, so a key there would be readable by any authenticated
# user via search_read.
t_eq "0" "$(pg "SELECT count(*) FROM ir_config_parameter WHERE key ILIKE '%api_key%' OR value LIKE 'sk-ant-%'")" \
     "no key is hiding in ir_config_parameter"

sec "2. reading it never returns the key"
G=$(call ir.ai.settings get '[{}]')
has_error "$G" && { no "get failed: $(echo "$G" | head -c 200)"; verdict; exit 1; }
t_contains "$G" '"configured"' "get reports whether a key is configured"
t_lacks    "$G" '"api_key"'    "and does not carry an api_key field at all"

sec "3. saving a key stores it, and still does not echo it"
S=$(call ir.ai.settings save "[{\"api_key\":\"$FAKE\",\"enabled\":true}]")
has_error "$S" && no "save failed: $(echo "$S" | head -c 200)"
t_eq "1" "$(pg "SELECT count(*) FROM ir_ai_settings WHERE api_key='$FAKE'")" "the key reached the database"
t_lacks "$S" "$FAKE" "the save response does not contain the key"
t_lacks "$S" "sk-ant" "not even a fragment of it"

# Every read path, checked against the actual secret rather than a field name.
for m in get read search_read web_read web_search_read; do
    RESP=$(call ir.ai.settings "$m" '[{}]')
    t_lacks "$RESP" "$FAKE" "$m does not return the key"
done

sec "4. what it shows instead"
G=$(call ir.ai.settings get '[{}]')
t_contains "$G" '"configured":true' "it reports a key IS configured"
t_contains "$G" '"key_tail":"1234"' "and shows only the last four characters"
t_contains "$G" '"enabled":true'    "the enabled flag saved"

sec "5. an empty key means 'leave it alone', not 'wipe it'"
# A screen that has never seen the key must not be able to erase it just by
# saving some other field.
call ir.ai.settings save '[{"max_output_tokens":4096}]' >/dev/null
t_eq "1" "$(pg "SELECT count(*) FROM ir_ai_settings WHERE api_key='$FAKE'")" \
     "saving another field left the key intact"
t_eq "4096" "$(pg "SELECT max_output_tokens FROM ir_ai_settings WHERE id=1")" "and the other field saved"

sec "6. validation"
BAD=$(call ir.ai.settings save '[{"provider":"openai"}]')
has_error "$BAD" && ok "an unknown provider is refused" || no "provider 'openai' was accepted"
t_eq "anthropic" "$(pg "SELECT provider FROM ir_ai_settings WHERE id=1")" "and the stored provider is unchanged"

sec "7. test_connection, and the providers behind it"
# Keys live per PROVIDER now (ir_ai_provider), not in the settings row, so a
# provider can be switched without re-entering one. The suite only ever
# exercises `mock`: a test that calls a paid API is a test nobody can run.
PROVS=$(call ir.ai.settings providers '[{}]')
has_error "$PROVS" && no "providers failed: $(echo "$PROVS" | head -c 160)"
for p in anthropic xai mock; do
    t_contains "$PROVS" "\"$p\"" "the $p provider is known"
done
t_lacks "$PROVS" "$FAKE" "the provider list never returns a key"

call ir.ai.settings save '[{"provider":"mock"}]' >/dev/null
M=$(call ir.ai.settings test_connection '[{}]')
t_contains "$M" '"ok":true'   "the mock provider answers"
t_contains "$M" "No network"  "without touching the network — which is why the suite can run it"

sec "7b. the bridge stages, it does not write"
A=$(call ir.ai.settings ask '[{"query":"AI-GUARD-1"}]')
t_contains "$A" '"ok":true'      "ask returns a proposal from the mock provider"
t_contains "$A" '"candidates"'   "as a candidate list — an incomplete query has several answers"
t_contains "$A" '"result"'       "with the first still under 'result' for older callers"
t_contains "$A" '"notes"'        "and the agent's own commentary"
t_lacks    "$A" "$FAKE"          "and no key in the response"
# Whether it actually READ anything is not the same question as whether it
# answered. A model with no search still returns a part number and a URL.
t_contains "$A" '"searched":false' "the mock reports that it did not search"
# The bridge must not touch the catalogue: proposing is not applying.
t_eq "0" "$(pg "SELECT count(*) FROM product_product WHERE default_code='MOCK-0001'")" \
     "asking created no product — a human still has to apply it"

sec "7c. the double-multiplier guard"
# The first live call answered resistance value="4k7" unit="kΩ". Read literally
# that is 4700 kΩ — 4.7 MΩ, a thousand times the real part — because apply()
# multiplies the parsed value by the unit's factor. The mock provider now
# returns that exact shape on purpose, so this guard is exercised with no
# network and cannot rot.
t_contains "$A" '"adjusted"'                     "the response carries an adjustment report"
t_contains "$A" 'would have been applied twice'  "and says what it corrected"
t_contains "$A" '"unit":"Ω"'                     "the prefixed unit was demoted to its base"
t_lacks    "$A" '"unit":"kΩ"'                    "so the multiplier is not applied twice"
# The two parameters that were already right must be left alone — a guard that
# "fixes" correct input is worse than none.
t_contains "$A" '"unit":"W","value":"125m"' "a prefix in the VALUE alone is untouched"
t_contains "$A" '"unit":"%","value":"1"'    "a plain value with a base unit is untouched"

# The assertion that actually matters is the number that lands in the database:
# value_base is what parametric search filters on.
RES=$(echo "$A" | python3 -c 'import sys,json; print(json.dumps(json.load(sys.stdin)["result"]["result"]))')
SUB=$(call part.lookup submit "[$RES]")
PROP=$(echo "$SUB" | python3 -c 'import sys,json; print(json.load(sys.stdin)["result"]["id"])')
t_nonempty "$PROP" "the proposal staged"
call part.lookup apply "[{\"id\":$PROP}]" >/dev/null
PRODID=$(pgid "SELECT id FROM product_product WHERE default_code='MOCK-0001' ORDER BY id DESC LIMIT 1")
t_eq "4700" "$(pg "SELECT value_base::bigint FROM part_parameter
                   WHERE product_id=$PRODID AND name='resistance'")" \
     "4k7 stored as 4700 Ω, not 4700000"
t_eq "Ω" "$(pgv "SELECT u.symbol FROM part_parameter pa JOIN part_unit u ON u.id=pa.unit_id
                 WHERE pa.product_id=$PRODID AND pa.name='resistance'" | xargs)" \
     "against the base unit"
t_eq "0.125" "$(pg "SELECT value_base FROM part_parameter
                   WHERE product_id=$PRODID AND name='power'")" \
     "and 125m W is still 0.125 W — the correct one was not 'corrected'"

sec "7d. editing a staged proposal"
# The queue is where a person decides whether a proposal is true. Without an
# edit path, disagreeing with one field means rejecting the whole thing and
# retyping it — so the realistic alternative is applying something known to be
# slightly wrong.
E=$(call part.lookup submit '[{"query":"AI-GUARD-EDIT","mpn":"AI-GUARD-EDIT",
     "manufacturer":"Before","parameters":[{"name":"resistance","value":"1k","unit":"Ω"}]}]')
EID=$(echo "$E" | python3 -c 'import sys,json; print(json.load(sys.stdin)["result"]["id"])')
t_nonempty "$EID" "a proposal to edit was staged"

U=$(call part.lookup update "[{\"id\":$EID,\"manufacturer\":\"After\",
     \"parameters\":[{\"name\":\"resistance\",\"value\":\"10k\",\"unit\":\"Ω\"},
                     {\"name\":\"tolerance\",\"value\":\"5\",\"unit\":\"%\"}]}]")
t_contains "$U" '"ok":true' "an edit is accepted"
t_eq "After" "$(pg "SELECT manufacturer FROM part_lookup_result WHERE id=$EID")" \
     "the edited field was stored"
t_eq "2" "$(pg "SELECT jsonb_array_length(payload->'parameters') FROM part_lookup_result WHERE id=$EID")" \
     "and a parameter added by hand took effect"
# A field the editor never showed must survive being edited.
t_eq "AI-GUARD-EDIT" "$(pg "SELECT payload->>'query' FROM part_lookup_result WHERE id=$EID")" \
     "a field the screen never sent was not wiped"

# Confidence is the AGENT'S claim about its own certainty. A reviewer
# overwriting it does not make the part more reliable — it destroys the signal
# that says how hard the rest needs checking. Read-only on the server, not just
# hidden on the screen.
CONF0=$(pg "SELECT confidence FROM part_lookup_result WHERE id=$EID")
call part.lookup update "[{\"id\":$EID,\"confidence\":0.99}]" >/dev/null
t_eq "$CONF0" "$(pg "SELECT confidence FROM part_lookup_result WHERE id=$EID")" \
     "confidence cannot be edited, even through the API"

# An edit is held to the SAME rules as a submit — correcting one field is not
# a way to smuggle a bad one past validation.
BADU=$(call part.lookup update "[{\"id\":$EID,\"parameters\":[{\"name\":\"r\",\"value\":\"1\",\"unit\":\"furlongs\"}]}]")
t_contains "$BADU" '"state":"invalid"' "an unknown unit still fails validation on edit"
t_eq "invalid" "$(pg "SELECT state FROM part_lookup_result WHERE id=$EID")" \
     "and the proposal is marked needs-fixing"
# Fixing it must bring it back, or "needs fixing" is a dead end.
call part.lookup update "[{\"id\":$EID,\"parameters\":[{\"name\":\"resistance\",\"value\":\"10k\",\"unit\":\"Ω\"}]}]" >/dev/null
t_eq "pending" "$(pg "SELECT state FROM part_lookup_result WHERE id=$EID")" \
     "correcting the error returns it to pending"

# An applied proposal is a record of what was written. Editing it would leave
# no way to tell what actually happened.
FROZEN=$(call part.lookup update "[{\"id\":$PROP,\"manufacturer\":\"Rewriting history\"}]")
has_error "$FROZEN" && ok "an applied proposal cannot be edited" \
                    || no "an applied proposal was edited"

call ir.ai.settings save '[{"provider":"anthropic"}]' >/dev/null

sec "8. reveal_for_setup is the one path that returns it"
RV=$(call ir.ai.settings reveal_for_setup '[{}]')
t_contains "$RV" "$FAKE" "reveal returns the key, deliberately"
t_contains "$RV" "systemd" "with a systemd line to paste"
t_contains "$RV" "export ANTHROPIC_API_KEY" "and a shell export"
# It must leave a trace. A credential that can be revealed without one is a
# credential nobody can reason about after an incident.
t_ge "$(pg "SELECT count(*) FROM audit_log WHERE model='ir.ai.settings' AND operation='reveal_for_setup'")" 1 \
     "the reveal is written to the audit log"

sec "9. removing the key"
call ir.ai.settings clear_key '[{}]' >/dev/null
t_eq "" "$(pg "SELECT api_key FROM ir_ai_settings WHERE id=1")" "the key is gone"
t_eq "0" "$(pg "SELECT enabled::int FROM ir_ai_settings WHERE id=1")" "and the agent was disabled with it"
NOKEY=$(call ir.ai.settings reveal_for_setup '[{}]')
has_error "$NOKEY" && ok "revealing nothing is refused" || no "reveal returned something with no key set"

sec "9b. the help assistant"
call ir.ai.settings save '[{"provider":"mock","enabled":true,"api_key":"x"}]' >/dev/null
ST=$(call ir.ai.settings status '[{}]')
t_contains "$ST" '"ready":true'  "status reports the agent is usable"
t_contains "$ST" '"admin":true'  "and whether this user may configure it"
# status is the ONE method here a non-admin may call, so it must give away
# nothing: no key, no tail, no provider, no URL.
t_lacks "$ST" "api_key"  "status leaks no key field"
t_lacks "$ST" "sk-"      "nor any key fragment"
t_lacks "$ST" "base_url" "nor the endpoint"

# Retrieval is term-by-term. Matching the whole question as one substring finds
# nothing — no article contains a sentence verbatim — and the assistant then
# says "not in the manual" about things the manual covers well.
H=$(call ir.ai.settings ask_help '[{"question":"How should I write the value 4k7 and its unit?"}]')
t_contains "$H" '"ok":true'      "ask_help answers"
t_contains "$H" '"grounded":true' "and reports that it had articles to work from"
t_contains "$H" 'parts-units'    "retrieval found the units article for a natural-language question"

# A citation must be a slug that EXISTS: a model can cite one it invented, and
# a dead button is worse than no button.
t_eq "0" "$(echo "$H" | python3 -c '
import sys, json
cited = json.load(sys.stdin)["result"].get("cited", [])
print(sum(1 for c in cited if not c.get("slug")))')" \
     "every citation carries a real slug"

sec "9c. prompts are files, not string literals"
# They were compiled in, which made the part of this most likely to need
# tuning per deployment the part that needed a rebuild to change.
P=$(call ir.ai.settings prompts '[{}]')
for t in part_lookup help_assistant bom_headers; do
    t_contains "$P" "\"$t\"" "the $t prompt is listed"
done
t_contains "$P" '"source":"file"' "and is being read from prompts/"
t_contains "$P" 'prompts/part_lookup.md' "with the path it came from"
t_contains "$P" '"placeholders"' "and the placeholders the code supplies"
# The shipped text must NOT be in the database, or "reset" is just a copy of
# whatever happened to be default at install time.
t_eq "0" "$(pg "SELECT count(*) FROM ir_ai_prompt")" "no override exists until somebody makes one"

sec "9d. an override wins, and can be taken back"
call ir.ai.settings save_prompt '[{"task":"help_assistant",
     "body":"OVERRIDDEN. Articles: {{articles}} Question: {{question}}"}]' >/dev/null
P=$(call ir.ai.settings prompts '[{}]')
t_contains "$P" '"source":"override"' "the override becomes the live text"
t_eq "1" "$(pg "SELECT count(*) FROM ir_ai_prompt WHERE task='help_assistant'")" \
     "and it is stored in the database, not written back to the file"
# The UI must never write back to a git-tracked file: a process that rewrites
# its own sources is a process fighting whoever deployed it.
grep -q 'You are the assistant' prompts/help_assistant.md \
    && ok "the file on disk is untouched" \
    || no "saving an override rewrote prompts/help_assistant.md"
t_lacks "$(cat prompts/help_assistant.md)" "OVERRIDDEN" "the override text never reached the file"
# It must actually be the text that gets SENT, not just displayed.
call ir.ai.settings save '[{"provider":"mock","enabled":true,"api_key":"x"}]' >/dev/null
H=$(call ir.ai.settings ask_help '[{"question":"units"}]')
t_contains "$H" '"ok":true' "asking still works with an override in place"

call ir.ai.settings reset_prompt '[{"task":"help_assistant"}]' >/dev/null
t_eq "0" "$(pg "SELECT count(*) FROM ir_ai_prompt WHERE task='help_assistant'")" \
     "reset drops the override"
P=$(call ir.ai.settings prompts '[{}]')
t_lacks "$P" '"source":"override"' "and the file is in effect again"

sec "9e. a prompt cannot be saved into uselessness"
# A part-lookup prompt with no {{query}} asks the model about nothing, and the
# failure looks like a bad model rather than a bad edit.
BAD=$(call ir.ai.settings save_prompt '[{"task":"part_lookup","body":"Find a part. No placeholder here."}]')
has_error "$BAD" && ok "dropping a required placeholder is refused" \
                 || no "a prompt with no {{query}} was accepted"
t_contains "$BAD" "query" "and the message names what is missing"
EMPTY=$(call ir.ai.settings save_prompt '[{"task":"part_lookup","body":"   "}]')
has_error "$EMPTY" && ok "an empty prompt is refused" || no "an empty prompt was stored"
UNK=$(call ir.ai.settings save_prompt '[{"task":"nope","body":"{{query}}"}]')
has_error "$UNK" && ok "an unknown task is refused" || no "an unknown task was accepted"
t_eq "0" "$(pg "SELECT count(*) FROM ir_ai_prompt")" "none of those stored anything"

sec "10. the screen is wired"
t_eq "200" "$(http_code "/src/components/AiSettings.js")" "AiSettings.js is served"
t_contains "$(http_get "/index.html")" "components/AiSettings.js" "index.html loads it"
t_contains "$(http_get "/src/app.js")" "'ir.ai.settings':     AiSettings" "app.js maps the model to it"
t_contains "$(http_get "/src/app.css")" ".ai-shell" "the stylesheet carries its rules"
t_eq "1" "$(pg "SELECT count(*) FROM ir_ui_menu WHERE id=403 AND action_id=117")" "the Settings menu entry exists"
t_eq "ir.ai.settings" "$(pg "SELECT res_model FROM ir_act_window WHERE id=117")" "pointing at the right model"

verdict

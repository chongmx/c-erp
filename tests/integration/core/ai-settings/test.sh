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

sec "7. test_connection is honest about what it proved"
T=$(call ir.ai.settings test_connection '[{}]')
t_contains "$T" '"ok":true' "a well-formed key passes the shape check"
t_contains "$T" "NOT yet verified" "and it says plainly that the API was not called"
t_lacks    "$T" "$FAKE" "the result does not contain the key"

call ir.ai.settings save '[{"provider":"mock"}]' >/dev/null
M=$(call ir.ai.settings test_connection '[{}]')
t_contains "$M" '"ok":true'   "the mock provider answers"
t_contains "$M" "No network"  "without touching the network — which is why the suite can run it"
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

sec "10. the screen is wired"
t_eq "200" "$(http_code "/src/components/AiSettings.js")" "AiSettings.js is served"
t_contains "$(http_get "/index.html")" "components/AiSettings.js" "index.html loads it"
t_contains "$(http_get "/src/app.js")" "'ir.ai.settings':     AiSettings" "app.js maps the model to it"
t_contains "$(http_get "/src/app.css")" ".ai-shell" "the stylesheet carries its rules"
t_eq "1" "$(pg "SELECT count(*) FROM ir_ui_menu WHERE id=403 AND action_id=117")" "the Settings menu entry exists"
t_eq "ir.ai.settings" "$(pg "SELECT res_model FROM ir_act_window WHERE id=117")" "pointing at the right model"

verdict

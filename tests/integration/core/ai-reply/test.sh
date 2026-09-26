#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# When the model answers badly.  (CERP-11, CERP-12, CERP-9)
#
# Reported: asking for "8Mhz temperature controlled crystal" came back as "the
# AI agent did not reply with valid JSON". The cause was a reply CUT OFF at the
# output-token ceiling — a model that reasons before it answers spends that
# budget thinking — read with a rule ("first { to last }") that cannot tell a
# truncated reply from a malformed one.
#
# The failures worth testing are the ones where the model misbehaves, and
# neither the mock provider (which always answers well) nor a real one (which
# cannot be asked to misbehave) can produce them. So this points the ERP at
# tests/lib/fake_provider.py, which answers exactly as badly as required.
#
# Three things are pinned:
#   * a truncated reply is REPORTED as truncated, naming the ceiling it hit
#   * a second, cheaper model is asked to read it, and its answer is used
#   * the model list comes from the provider, and AUTO picks a fast one
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
PORT=8791

SNAP=$(pgv "SELECT format('UPDATE ir_ai_settings SET provider=%L, enabled=%s, parser_model=%L, parser_enabled=%s WHERE id=1',
              provider, enabled, parser_model, parser_enabled) FROM ir_ai_settings WHERE id=1" | sed 's/^ *//')
SNAPP=$(pgv "SELECT format('UPDATE ir_ai_provider SET base_url=%L, api_key=%L, model=%L, search_style=%L WHERE name=''xai''',
              base_url, api_key, model, search_style) FROM ir_ai_provider WHERE name='xai'" | sed 's/^ *//')
FAKE_PID=""
cleanup(){
    [ -n "$FAKE_PID" ] && kill "$FAKE_PID" 2>/dev/null
    [ -n "$SNAPP" ] && pg "$SNAPP" >/dev/null 2>&1
    [ -n "$SNAP" ]  && pg "$SNAP"  >/dev/null 2>&1
    pg "UPDATE ir_ai_provider SET models='[]'::jsonb, models_at=NULL WHERE name='xai'" >/dev/null 2>&1
    pg "DELETE FROM ir_ai_job WHERE query LIKE 'ZZAR%'" >/dev/null 2>&1
}
cleanup; trap cleanup EXIT
auth_or_die

start_fake(){   # start_fake <mode>
    [ -n "$FAKE_PID" ] && kill "$FAKE_PID" 2>/dev/null
    python3 tests/lib/fake_provider.py "$PORT" "$1" > /tmp/zzar_fake.log 2>&1 &
    FAKE_PID=$!
    for i in $(seq 1 30); do
        curl -s -m 1 "http://127.0.0.1:$PORT/v1/models" > /dev/null 2>&1 && return 0
        sleep 0.2
    done
    return 1
}

# The ERP talks to the fake instead of xAI. Everything else about the provider
# — the wire, the header, the key — stays exactly as it is in production.
pg "UPDATE ir_ai_provider SET base_url='http://127.0.0.1:$PORT', api_key='fake-key',
      model='fake-large-reasoning', search_style='' WHERE name='xai'" >/dev/null
pg "UPDATE ir_ai_settings SET provider='xai', enabled=true, parser_enabled=true,
      parser_model='' WHERE id=1" >/dev/null

# -------------------------------------------------------------------------
sec "1. a truncated reply is reported as truncated"
# -------------------------------------------------------------------------
start_fake truncated || { no "the fake provider did not start"; verdict; exit 1; }
ok "a provider that answers badly on demand is listening on $PORT"

# Off, so this section sees the raw failure rather than a rescue.
pg "UPDATE ir_ai_settings SET parser_enabled=false WHERE id=1" >/dev/null
CEIL=$(pg "SELECT max_output_tokens FROM ir_ai_settings WHERE id=1")
R=$(call ir.ai.settings ask '[{"query":"ZZAR 8Mhz temperature controlled crystal"}]')
echo "    $(printf '%s' "$R" | head -c 260)"
t_contains "$R" '"ok":false'   "the lookup fails, as it must — there is no answer in that reply"
t_contains "$R" 'cut off'      "and says the reply was CUT OFF, not that it was bad JSON"
t_contains "$R" "$CEIL"        "naming the ceiling it hit ($CEIL tokens)"
t_contains "$R" 'Max output tokens' "and what to change"
t_contains "$R" '"truncated":true' "the reason is machine-readable too"
# The evidence travels with the failure: without it the only account of what
# happened is our summary of it.
t_contains "$R" 'FAKE-8MHZ-TC' "the model's own words come back, so they can be read on screen"

# -------------------------------------------------------------------------
sec "2. prose is not truncation"
# -------------------------------------------------------------------------
# A model that answers in English needs different advice from one that ran out
# of room. Telling someone to raise a token ceiling they have not hit wastes
# their afternoon.
start_fake prose || no "could not restart the fake provider"
P=$(call ir.ai.settings ask '[{"query":"ZZAR prose answer"}]')
t_contains "$P" '"ok":false'      "a prose answer is still a failure"
t_lacks    "$P" 'cut off'         "but it is NOT reported as truncated"
t_contains "$P" 'not JSON'        "it says the reply was not JSON"
t_contains "$P" 'Prompts'         "and points at the prompt, which is the thing to change"

# -------------------------------------------------------------------------
sec "3. the fallback parser — the last line of defence"
# -------------------------------------------------------------------------
# The fake answers the RESEARCH call with a cut-off reply and the REPAIR call
# with valid JSON, which is the fallback's whole job seen from outside.
start_fake repair || no "could not restart the fake provider"
pg "UPDATE ir_ai_settings SET parser_enabled=true, parser_model='fake-mini' WHERE id=1" >/dev/null
F=$(call ir.ai.settings ask '[{"query":"ZZAR salvage this"}]')
echo "    $(printf '%s' "$F" | head -c 220)"
t_contains "$F" '"ok":true'          "the lookup now succeeds on a reply the main model botched"
t_contains "$F" 'FAKE-8MHZ-TCXO'     "with the candidate salvaged out of it"
t_contains "$F" '"repaired_by":"fake-mini"' \
           "and it says WHICH model salvaged it — a reviewer should know it was reconstructed"

# Switched off, the same reply fails. That is what proves section 3 is the
# fallback working rather than the fake being generous.
pg "UPDATE ir_ai_settings SET parser_enabled=false WHERE id=1" >/dev/null
OFF=$(call ir.ai.settings ask '[{"query":"ZZAR salvage disabled"}]')
t_contains "$OFF" '"ok":false' "with the fallback switched off, the same reply fails"
pg "UPDATE ir_ai_settings SET parser_enabled=true WHERE id=1" >/dev/null

# -------------------------------------------------------------------------
sec "4. the model list, and AUTO"
# -------------------------------------------------------------------------
start_fake models || no "could not restart the fake provider"
M=$(call ir.ai.settings models '[{"refresh":true}]')
echo "    $(printf '%s' "$M" | head -c 200)"
t_contains "$M" '"ok":true'    "the provider's model list can be read from the provider"
t_contains "$M" 'fake-mini'    "and carries what it offers"
t_eq "3" "$(pg "SELECT jsonb_array_length(models) FROM ir_ai_provider WHERE name='xai'")" \
     "the list is cached against the provider, not re-fetched on every page open"
t_nonempty "$(pg "SELECT models_at FROM ir_ai_provider WHERE name='xai'")" "with the time it was read"

C=$(call ir.ai.settings models '[{}]')
t_contains "$C" '"cached":true' "a second read comes from the cache — opening Settings costs nothing"

# AUTO: no parser model named, so it picks a fast one out of that list.
# "fake-mini" is the small one; "fake-large-reasoning" is not.
start_fake repair || no "could not restart the fake provider"
pg "UPDATE ir_ai_settings SET parser_model='' WHERE id=1" >/dev/null
A=$(call ir.ai.settings ask '[{"query":"ZZAR auto pick"}]')
t_contains "$A" '"repaired_by":"fake-mini"' \
           "AUTO picks the small model out of the list, not the reasoning one"

verdict

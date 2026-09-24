#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Asking the agent is a JOB, and the timeouts are configurable.  (CERP-10)
#
# Reported: "when I use AI agent that take more time, the part lookup AI assist
# timed out … I need a configurable time out for AI assist reply."
#
# Three layers each gave up before the model did, and the shortest won: the
# browser at a fixed 45 s, nginx at 60 s, and the server at 60/180 s hard-coded.
# The agent answered; the screen had stopped listening.
#
# The timeouts are now configuration. More importantly, the WAIT is no longer
# a held-open request: `ask_async` returns a job id immediately, a worker fills
# the row in, and the screen polls. That is what this file pins —
#
#   * ask_async answers in milliseconds, whatever the model is doing
#   * the job reaches a terminal state, with the answer in it
#   * a job belongs to ONE person: another user cannot read it, and a job that
#     does not exist is refused in exactly the same words
#   * a person cannot start unbounded work
#
# — because each of those is a way the old shape failed, or a way this one
# could fail worse.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
PFX='ZZAJ'

SNAP=$(pgv "SELECT format('UPDATE ir_ai_settings SET reply_timeout_s=%s, search_timeout_s=%s WHERE id=1',
              reply_timeout_s, search_timeout_s) FROM ir_ai_settings WHERE id=1" | sed 's/^ *//')
cleanup(){
    [ -n "$SNAP" ] && pg "$SNAP" >/dev/null 2>&1
    pg "DELETE FROM ir_ai_job WHERE query LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_users WHERE login='${PFX}_other'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name='${PFX} Other'" >/dev/null 2>&1
}
cleanup; trap cleanup EXIT
auth_or_die

# -------------------------------------------------------------------------
sec "1. the timeouts are configuration, not constants"
# -------------------------------------------------------------------------
S=$(call ir.ai.settings read '[{}]')
t_contains "$S" "reply_timeout_s"  "the settings carry a reply timeout"
t_contains "$S" "search_timeout_s" "and a separate one for a browsing search"
# The screen has to wait at least as long as the server will, so it must be
# able to ask what that is without being an administrator's settings page.
ST=$(call ir.ai.settings status '[{}]')
t_contains "$ST" "reply_timeout_s" "status reports the limits, so a screen can size its own wait"

call ir.ai.settings save '[{"reply_timeout_s":90,"search_timeout_s":240}]' > /dev/null
t_eq "90"  "$(pg "SELECT reply_timeout_s FROM ir_ai_settings WHERE id=1")"  "a longer reply timeout is saved"
t_eq "240" "$(pg "SELECT search_timeout_s FROM ir_ai_settings WHERE id=1")" "and a longer search timeout"

# Bounded at both ends: below 5 s nothing finishes, and past 600 s every layer
# in front of us has given up anyway, so a bigger number would be a promise
# nothing else keeps.
LOW=$(call ir.ai.settings save '[{"reply_timeout_s":2}]')
has_error "$LOW" && ok "a timeout under 5 s is refused" || no "2 s was accepted"
HIGH=$(call ir.ai.settings save '[{"search_timeout_s":100000}]')
has_error "$HIGH" && ok "and one past 600 s, which nothing downstream would honour" \
                  || no "100000 s was accepted"
t_eq "90" "$(pg "SELECT reply_timeout_s FROM ir_ai_settings WHERE id=1")" \
     "a refused value leaves the saved one alone"

# -------------------------------------------------------------------------
sec "2. asking returns at once, and the work carries on without the caller"
# -------------------------------------------------------------------------
START=$(date +%s%3N)
J=$(call ir.ai.settings ask_async "[{\"query\":\"${PFX} 4.7k 0805 1% resistor\"}]")
ELAPSED=$(( $(date +%s%3N) - START ))
echo "    ask_async answered in ${ELAPSED} ms: $(printf '%s' "$J" | head -c 100)"
JID=$(printf '%s' "$J" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['job_id'])" 2>/dev/null)
t_nonempty "$JID" "a job id comes back"
# The whole point: the HTTP call does not wait for the model. Two seconds is
# enormous for a database insert and far below anything a model does.
[ "$ELAPSED" -lt 2000 ] && ok "and it returned immediately (${ELAPSED} ms), not when the model did" \
                        || no "ask_async took ${ELAPSED} ms — it is waiting for the answer"
t_eq "1" "$(pg "SELECT count(*) FROM ir_ai_job WHERE id=${JID:-0}")" "the job is a row, so it survives this request"

# The mock provider answers locally, so this finishes quickly; a real one is
# the same shape, only slower.
for i in $(seq 1 30); do
    STATE=$(pg "SELECT state FROM ir_ai_job WHERE id=${JID:-0}")
    case "$STATE" in done|failed|cancelled) break ;; esac
    sleep 1
done
echo "    job $JID ended as $STATE"
t_eq "done" "$STATE" "the worker carried the job to a terminal state"

Q=$(call ir.ai.settings ask_status "[{\"job_id\":${JID:-0}}]")
t_contains "$Q" '"state"'   "the status call reports the state"
t_contains "$Q" '"seconds"' "and how long it has been going, for the screen to show"
t_contains "$Q" 'candidates' "and carries the answer itself once it is done"
t_eq "done" "$(printf '%s' "$Q" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['state'])" 2>/dev/null)" \
     "which is what a reloaded screen reads to catch up"

# Nothing is left running for a screen that has gone away.
t_eq "0" "$(pg "SELECT count(*) FROM ir_ai_job WHERE state IN ('queued','running') AND query LIKE '${PFX}%'")" \
     "no job is left hanging"

# -------------------------------------------------------------------------
sec "3. a job belongs to one person"
# -------------------------------------------------------------------------
# The id is a number, not a capability. Guessing one must get nothing — not
# the answer, and not even the fact that somebody else asked something.
PID=$(pgid "INSERT INTO res_partner (name, active, company_id) VALUES ('${PFX} Other', true, 1) RETURNING id")
# Through the API, not SQL: passwords are hashed, and a row with a plaintext
# one is a user who cannot sign in — which would make this section pass by
# never testing anything.
UID2=$(call res.users create "[{\"login\":\"${PFX}_other\",\"password\":\"Zzaj-Pass-1\",\"partner_id\":${PID:-0},\"active\":true}]" | rid)
t_nonempty "$UID2" "a second user exists to try with"

OTHER=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"${PFX}_other\",\"password\":\"Zzaj-Pass-1\"}}" \
        | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('session_id') or (d.get('result') or {}).get('session_id') or '')" 2>/dev/null)
if [ -n "$OTHER" ]; then
    X=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"ir.ai.settings\",\"method\":\"ask_status\",\"args\":[{\"job_id\":${JID:-0}}],\"kwargs\":{\"context\":{\"session_id\":\"$OTHER\"}}}}")
    has_error "$X" && ok "another signed-in user cannot read it" \
                   || no "a second user read someone else's job: $(printf '%s' "$X" | head -c 140)"
    t_lacks "$X" "candidates" "and gets none of the answer"
else
    no "the second user could not sign in — this section proved nothing"
fi

# Not yours and not there are deliberately the same answer, so the error
# cannot be used to count other people's questions.
MISS=$(call ir.ai.settings ask_status '[{"job_id":999999}]')
has_error "$MISS" && ok "a job that does not exist is refused" || no "a missing job was readable"
t_contains "$MISS" "No such job" "in the same words as one that is not yours"

# Anonymous callers get nothing at all.
ANON=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
       --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"ir.ai.settings\",\"method\":\"ask_status\",\"args\":[{\"job_id\":${JID:-0}}],\"kwargs\":{}}}")
has_error "$ANON" && ok "an unauthenticated caller is refused" || no "a job was readable without signing in"

# -------------------------------------------------------------------------
sec "4. nobody can start unbounded work"
# -------------------------------------------------------------------------
# Each question is a paid call to somebody's API and a thread of ours, so a
# screen that can start them without limit is a way to spend money by holding
# down a key. The rows are inserted directly: the cap is what is under test,
# not the worker.
pg "INSERT INTO ir_ai_job (kind,state,user_id,query)
    SELECT 'lookup','running', (SELECT id FROM res_users WHERE login='admin'), '${PFX} filler'
      FROM generate_series(1,3)" > /dev/null
CAP=$(call ir.ai.settings ask_async "[{\"query\":\"${PFX} one too many\"}]")
has_error "$CAP" && ok "a fourth question in flight is refused" || no "the per-user cap did not hold"
t_contains "$CAP" "questions running" "and says what to do about it"
pg "UPDATE ir_ai_job SET state='cancelled' WHERE query='${PFX} filler'" > /dev/null

# Cancelling is the way out, and it is the caller's own job it ends.
J2=$(call ir.ai.settings ask_async "[{\"query\":\"${PFX} cancel me\"}]")
JID2=$(printf '%s' "$J2" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['job_id'])" 2>/dev/null)
call ir.ai.settings ask_cancel "[{\"job_id\":${JID2:-0}}]" > /dev/null
STATE2=$(pg "SELECT state FROM ir_ai_job WHERE id=${JID2:-0}")
case "$STATE2" in cancelled|done) ok "a job can be cancelled (ended as $STATE2)" ;;
                  *) no "cancel left it as $STATE2" ;; esac

# An empty question is not work.
EMPTY=$(call ir.ai.settings ask_async '[{"query":""}]')
has_error "$EMPTY" && ok "an empty question is refused before anything is queued" || no "an empty query was queued"

verdict

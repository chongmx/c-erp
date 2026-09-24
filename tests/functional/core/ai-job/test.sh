#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# FUNCTIONAL — asking the agent without waiting on the line.  (CERP-10)
#
# Reported: the part-lookup assist timed out on a model that takes longer to
# think, while the setup page said the same agent was connected and replying.
#
# It was three timeouts stacked — browser 45 s, nginx 60 s, server 60/180 s —
# and the shortest decided. Rather than raise all three (and lose anyway to
# the CDN, which cuts an origin response at 100 s), asking became a job: the
# request returns at once and the screen polls a row.
#
# The behaviour worth proving on screen is the one that a held-open request
# can never have: RELOAD THE PAGE AND THE QUESTION IS STILL THERE. That is
# what tests/lib/render_ai_job.mjs drives, by clicking.
#
# The screen over `part.lookup`; the jobs are `ir.ai.settings` ask_async /
# ask_status / ask_cancel.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
PFX='ZZAJF'

cleanup(){
    pg "DELETE FROM part_lookup_result WHERE query LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM ir_ai_job WHERE query LIKE '${PFX}%'" >/dev/null 2>&1
}
cleanup; trap cleanup EXIT
auth_or_die

# -------------------------------------------------------------------------
sec "1. the premise, and the tooling"
# -------------------------------------------------------------------------
t_eq "t" "$(pg "SELECT enabled FROM ir_ai_settings WHERE id=1")" \
     "the agent is enabled, so the screen has something to ask"
t_nonempty "$(pg "SELECT to_regclass('ir_ai_job')")" "questions have somewhere to live"

CHROME=${CHROME_PATH:-/usr/bin/google-chrome}
if [ ! -x "$CHROME" ]; then
    echo "    NOTE  no Chrome at $CHROME — skipping the on-screen journey"
    verdict; exit $?
fi
if [ ! -d node_modules/puppeteer-core ]; then
    echo "    NOTE  puppeteer-core is not installed — skipping the on-screen journey"
    verdict; exit $?
fi
ok "Chrome and puppeteer-core are present"

# -------------------------------------------------------------------------
sec "2. the journey, on screen"
# -------------------------------------------------------------------------
OUT=$(SHOTDIR=/tmp/ai_job BASE="$BASE" DBN="$DBN" \
      timeout 340 node tests/lib/render_ai_job.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "a question survives a reload, and answers when it is ready"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the rows behind it, checked independently"
# -------------------------------------------------------------------------
t_ge "$(pg "SELECT count(*) FROM ir_ai_job WHERE query LIKE '${PFX}%'")" 3 \
     "the questions were recorded as jobs"
t_eq "0" "$(pg "SELECT count(*) FROM ir_ai_job WHERE query LIKE '${PFX}%' AND state IN ('queued','running')")" \
     "none is left running — every one reached a terminal state"
t_eq "1" "$(pg "SELECT count(*) FROM ir_ai_job WHERE query='${PFX} cancel me' AND state='cancelled'")" \
     "'Stop waiting' cancelled the job on the server"
# The answer is stored, not merely rendered: that is what a second browser,
# or the same one tomorrow, would read.
t_contains "$(pgv "SELECT COALESCE(result::text,'') FROM ir_ai_job
                    WHERE query='${PFX} slow question'")" \
           "${PFX}-LATE-1" "the finished answer is in the job, not only on the screen"
t_eq "$(pg "SELECT id FROM res_users WHERE login='admin'")" \
     "$(pg "SELECT DISTINCT user_id FROM ir_ai_job WHERE query LIKE '${PFX}%'")" \
     "and every job belongs to the person who asked it"

verdict

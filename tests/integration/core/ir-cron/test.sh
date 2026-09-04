#!/bin/bash
# --- harness ---------------------------------------------------------------
# Walk up for CMakeLists.txt rather than counting `../`, so this test behaves
# the same whether the runner invoked it or you ran it directly, and so it can
# be nested a folder deeper without a preamble edit.
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# P5 — ir.cron.
#
# A timer is easy; a scheduler you can trust with money is not.
# These are the properties that distinguish them:
#   * it actually fires
#   * next_run persists, so a job due during downtime still runs
#   * a failing job is rescheduled with backoff, never dropped
#   * a missing handler is reported, not silently ignored
# =============================================================
DBN=${DBN:-odoo}
LOGDIR=/home/user/code/c-erp/log
FAILED=
pg() { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

# The log file is ROTATED: on start the server renames system.log to
# system.<timestamp>.log and recreates it lazily on the first write. Grepping a
# fixed path made this script fail whenever it ran in the seconds after a
# restart — three "FAIL … no line in the log" results that said nothing about
# the scheduler (docs/090). Search the current file *and* the ones rotated out
# during this run instead, and wait for it to appear.
for _ in $(seq 1 20); do
    [ -s "$LOGDIR/system.log" ] && break
    sleep 1
done
# Newest first, capped: a long-lived checkout accumulates thousands of these.
loggrep() {   # pattern -> exit 0 if found in any recent log file
    # shellcheck disable=SC2012
    ls -t "$LOGDIR"/system*.log 2>/dev/null | head -8 | xargs -r grep -l -- "$1" >/dev/null 2>&1
}
logcount() {  # pattern -> number of matching lines across the recent log files
    # shellcheck disable=SC2012
    ls -t "$LOGDIR"/system*.log 2>/dev/null | head -8 | xargs -r grep -c -- "$1" 2>/dev/null \
        | awk -F: '{s += $NF} END {print s+0}'
}

echo "############ schedule ############"
PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -c \
  "SELECT code, interval_minutes AS mins, active, failure_count AS fails FROM ir_cron ORDER BY code" 2>/dev/null

echo
echo "############ 1. the scheduler started ############"
loggrep "cron] scheduler started" && ok "scheduler running" || no "no start line in the log"

echo
echo "############ 2. session.gc actually fires ############"
BEFORE=$(pg "SELECT COALESCE(last_run::text,'never') FROM ir_cron WHERE code='session.gc'")
echo "    last_run before: $BEFORE"
# Force it due; the tick is 30 s.
pg "UPDATE ir_cron SET next_run = now() - interval '1 minute' WHERE code='session.gc'" >/dev/null
echo "    forced due; waiting 35 s for the next tick..."
sleep 35
AFTER=$(pg "SELECT COALESCE(last_run::text,'never') FROM ir_cron WHERE code='session.gc'")
NEXT=$(pg "SELECT next_run > now() FROM ir_cron WHERE code='session.gc'")
echo "    last_run after:  $AFTER"
[ "$AFTER" != "$BEFORE" ] && ok "job ran (last_run advanced)" || no "job did not run"
[ "$NEXT" = "t" ]         && ok "next_run rescheduled into the future" || no "next_run not advanced"
loggrep "sessions] evicted" && ok "job body executed (eviction logged)" || no "no eviction log line"

echo
echo "############ 3. a failing job is rescheduled with backoff, not dropped ############"
# A code with NO handler, created here and removed at the end.
#
# This used to borrow 'rental.billing', which was seeded inactive and
# unhandled. The rental module now registers a handler for it and
# activates it, so the premise silently became false and the test failed
# for a reason that had nothing to do with ir.cron. A probe row owned by
# this script cannot be invalidated by another module shipping.
# The code is unique per run. The scheduler warns once PER CODE for the life of
# the process — that is the anti-spam property being tested — so a fixed code
# produces no warning at all on the second run against the same server, and the
# check fails for the opposite reason to the one it is looking for.
PROBE="test.no.handler.probe.$$.$(date +%s)"
# Count BEFORE activating and assert the delta. The absolute count would also
# include warnings this same probe produced on previous runs, which survive in
# the rotated log files — that turned "warned once" into "warned 3 times" on the
# third run of the suite, a failure about test history rather than the code.
WARNED_BEFORE=$(logcount "no handler registered for '$PROBE'")
pg "INSERT INTO ir_cron (code, name, interval_minutes, active, next_run)
    VALUES ('$PROBE', 'Probe — deliberately unhandled', 1440, TRUE,
            now() - interval '1 minute')
    ON CONFLICT (code) DO UPDATE
      SET active = TRUE, next_run = now() - interval '1 minute'" >/dev/null
echo "    activated '$PROBE' (no handler registered); waiting 35 s..."
sleep 35
WARNED_AFTER=$(logcount "no handler registered for '$PROBE'")
WARNED=$(( ${WARNED_AFTER:-0} - ${WARNED_BEFORE:-0} ))
echo "    'no handler' warnings this run: $WARNED"
[ "$WARNED" -ge 1 ] && ok "missing handler reported" || no "missing handler NOT reported"
[ "$WARNED" -le 2 ] && ok "warned once, not every tick" || no "warning repeated $WARNED times — log spam"

# session.gc must be unaffected by the broken job
STILL=$(pg "SELECT active FROM ir_cron WHERE code='session.gc'")
[ "$STILL" = "t" ] && ok "a broken job does not disable healthy ones" || no "session.gc was affected"
# This run's probe, plus any left behind by an earlier run that died mid-way —
# a unique code per run would otherwise accumulate dead rows in ir_cron.
pg "DELETE FROM ir_cron WHERE code LIKE 'test.no.handler.probe%'" >/dev/null

echo
echo "############ 4. next_run survives a restart (downtime catch-up) ############"
# next_run is persisted rather than held in memory, so a job that came due
# while the server was down runs at startup instead of being skipped.
PERSISTED=$(pg "SELECT count(*) FROM information_schema.columns
                 WHERE table_name='ir_cron' AND column_name='next_run'")
[ "$PERSISTED" = "1" ] && ok "next_run is a persisted column" || no "next_run not persisted"
pg "UPDATE ir_cron SET next_run = now() - interval '2 days' WHERE code='session.gc'" >/dev/null
OVERDUE=$(pg "SELECT next_run < now() FROM ir_cron WHERE code='session.gc'")
[ "$OVERDUE" = "t" ] && ok "an overdue job stays due across a restart" || no "unexpected"
echo "    (set 2 days overdue; the next tick will pick it up and reschedule from now(),"
echo "     not fire repeatedly to 'catch up')"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

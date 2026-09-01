#!/usr/bin/env bash
# =============================================================
# tests/tools/audit_test_leaks.sh — which tests leave rows behind?
#
# Reading cleanup blocks is how the invoice leak went unnoticed for eleven days:
# every script HAD a cleanup, and one of them silently failed. So this measures
# instead — row counts of every public table before and after each script, and
# reports the delta.
#
# A test should be hermetic: whatever it creates, it removes. A non-zero delta
# is not always a bug (a test may deliberately seed reference data), but it must
# be a DECISION, and this is the list to decide from.
#
#   ./tests/tools/audit_test_leaks.sh              # audit every tests/**/test.sh
#   ./tests/tools/audit_test_leaks.sh bank_recon   # just the ones matching
# =============================================================
set -uo pipefail
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1

PGHOST="${PGHOST:-localhost}"; PGUSER="${PGUSER:-odoo}"; PGDATABASE="${PGDATABASE:-odoo}"
export PGPASSWORD="${PGPASSWORD:-odoo}"
Q(){ psql -h "$PGHOST" -U "$PGUSER" -d "$PGDATABASE" -tA -c "$1"; }

FILTER="${1:-}"
SNAP=$(mktemp -d)
trap 'rm -rf "$SNAP"' EXIT

# Count every public table in ONE query. Exact counts, unlike the planner's
# estimates in pg_class.reltuples — and a single round trip, because one psql
# per table per script is ~16,000 process spawns and takes longer than the
# suite it is auditing.
counts(){
    Q "SELECT c.relname || ' ' ||
              (xpath('/row/c/text()',
                     query_to_xml(format('SELECT count(*) AS c FROM public.%I', c.relname),
                                  false, true, '')))[1]::text
       FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
       WHERE n.nspname = 'public' AND c.relkind = 'r'
       ORDER BY c.relname" > "$1"
}

# The server must be up: these scripts drive the HTTP API.
if ! curl -s -o /dev/null --max-time 3 http://127.0.0.1:8069/; then
    echo "server is not responding on :8069 — start it first"; exit 1
fi

TOTAL_LEAK=0
LEAKY=""

echo "======================================================================"
echo " Leak audit — row deltas per verification script"
echo "======================================================================"

# Tests are folders now (docs/109), so the audit walks tests/ for test.sh
# rather than globbing scripts/verify_*.sh. The name reported is the path
# under tests/ — 'integration/account/bank-recon' — which is also what
# `tests/run.sh --only` takes, so a leaky test can be re-run directly.
for f in $(find tests -name test.sh -type f | sort); do
    name=$(dirname "${f#tests/}")
    [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]] && continue

    counts "$SNAP/before"
    # Output goes to a FILE, never a command substitution — run_tests.sh learned
    # this the hard way: `$( )` waits for every writer to close the pipe, and a
    # script that restarts the server hands the pipe to a process that never
    # exits, so the audit hangs forever with no timeout able to save it.
    # setsid + timeout + </dev/null for the same reasons it uses them.
    LOG="$SNAP/out.log"
    setsid timeout --kill-after=10 180 bash "$f" < /dev/null > "$LOG" 2>&1
    RC=$?
    VERDICT="no verdict"
    [ "$RC" -eq 124 ] && VERDICT="TIMEOUT"
    grep -q 'All checks passed.' "$LOG" && VERDICT="pass"
    grep -q '\*\*\* FAILURES'    "$LOG" && VERDICT="FAIL"
    counts "$SNAP/after"

    # Append-only bookkeeping is expected to grow — an audit trail that stayed
    # the same length after a test ran would be the actual bug.
    DELTA=$(join "$SNAP/before" "$SNAP/after" 2>/dev/null \
            | grep -Ev '^(audit_log|ir_logging|ir_cron_log|mail_message|mail_notification) ' \
            | awk '$3 != $2 {printf "%s %+d\n", $1, $3-$2}')
    # A table that did not exist before (or after) will not join; report those too.
    NEWT=$(comm -13 <(cut -d' ' -f1 "$SNAP/before") <(cut -d' ' -f1 "$SNAP/after") | sed 's/^/+table /')

    if [ -z "$DELTA" ] && [ -z "$NEWT" ]; then
        printf '  %-42s %-5s clean\n' "$name" "$VERDICT"
    else
        N=$(echo "$DELTA" | grep -c . || true)
        printf '  %-42s %-5s LEAKS in %s table(s)\n' "$name" "$VERDICT" "$N"
        echo "$DELTA" | sed 's/^/          /'
        [ -n "$NEWT" ] && echo "$NEWT" | sed 's/^/          /'
        LEAKY="$LEAKY $name"
        TOTAL_LEAK=$((TOTAL_LEAK + N))
    fi
done

echo "======================================================================"
if [ -z "$LEAKY" ]; then
    echo "  Every script audited is hermetic."
else
    echo "  Leaking scripts:$LEAKY"
    echo "  $TOTAL_LEAK table(s) left changed in total."
fi
echo "======================================================================"

#!/usr/bin/env bash
# =============================================================
# tests/run.sh — the single entry point for the whole suite. (docs/109)
#
#   ./tests/run.sh                    everything: unit, then the ordered groups
#   ./tests/run.sh --unit             unit only — no server, no database
#   ./tests/run.sh --group functional just the daily-usage journeys
#   ./tests/run.sh --only bank        every test whose path matches 'bank'
#   ./tests/run.sh --list             what would run, in order, and why
#
# Exit status is 0 only if everything passed, so this is what CI runs.
#
# WHAT CHANGED FROM scripts/run_tests.sh
# --------------------------------------
# Order and database state used to be implied — by filename, and by one global
# baseline load. Both are now declared, per test, in a `meta` file beside it:
#
#     group=integration     which phase it belongs to
#     order=50              position within the phase
#     scenario=baseline     the database state it demands
#     needs=fixtures        the canonical data set, or nothing
#     timeout=300           seconds before it is killed as wedged
#
# A test is a FOLDER, so it can carry whatever it needs alongside those two
# files — a .sql seed, a C++ helper, golden output to diff against.
#
# Groups run in a fixed order, and the order is the point:
#
#     setup       create the canonical fixtures, and assert the creation
#     integration per-area technical checks
#     functional  end-to-end journeys through the daily work
#     security    penetration tests, which assert that things FAIL
#     teardown    delete the fixtures, and assert the deletion
#
# Tests are never run concurrently. Several restart the server and rewrite
# shared settings; one that does so underneath another produces a failure with
# nothing to do with the code, which is a genuinely expensive thing to debug.
# =============================================================
set -uo pipefail

R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
export ERP_ROOT="$R"
source tests/lib/db.sh

BASE=${BASE:-http://127.0.0.1:8069}
UNIT=1
INTEGRATION=1
FILTER=""
ONLY=""
WANT_GROUPS=""
LIST=0
RESTORE=1
SNAPSHOT="log/pretest.dump"
DEFAULT_SCENARIO="baseline"
LOGDIR="log/tests"

while [ $# -gt 0 ]; do
    case "$1" in
        --unit)         INTEGRATION=0; shift ;;
        --no-unit)      UNIT=0; shift ;;
        --filter)       FILTER="${2:-}"; shift 2 ;;      # passed to erp_tests
        --only)         ONLY="${2:-}"; shift 2 ;;        # substring of the test path
        --group)        WANT_GROUPS="$WANT_GROUPS ${2:-}"; shift 2 ;;
        --list)         LIST=1; shift ;;
        --keep-db)      RESTORE=0; shift ;;
        --no-baseline)  DEFAULT_SCENARIO="current"; shift ;;
        --baseline)     DEFAULT_SCENARIO="${2:-}"; shift 2 ;;
        --scenario)     DEFAULT_SCENARIO="${2:-}"; shift 2 ;;
        -h|--help)      sed -n '2,40p' "$0"; exit 0 ;;
        *)              echo "unknown option: $1"; exit 2 ;;
    esac
done

mkdir -p "$LOGDIR"
PASSED=0; FAILED=0; SKIPPED=0; FAILED_NAMES=""

hdr()  { printf '\n\033[1m======== %s ========\033[0m\n' "$1"; }
pass() { PASSED=$((PASSED+1)); printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
fail() { FAILED=$((FAILED+1)); FAILED_NAMES="$FAILED_NAMES $1"; printf '  \033[31mFAIL\033[0m  %s\n' "$1"; }
skip() { SKIPPED=$((SKIPPED+1)); printf '  \033[33mSKIP\033[0m  %s\n' "$1"; }

# ---------------------------------------------------------------
# Discovery — every directory holding a `meta` is a test.
#
# Nesting is free: tests/integration/account/bank-recon/ and
# tests/security/injection/domain-fields/ are found the same way, so a group
# can grow subfolders for organisation without the runner learning about them.
# ---------------------------------------------------------------
group_rank() {
    case "$1" in
        setup)       echo 00 ;;
        integration) echo 20 ;;
        functional)  echo 30 ;;
        security)    echo 40 ;;
        teardown)    echo 90 ;;
        *)           echo 50 ;;
    esac
}

meta_get() {  # meta_get <file> <key> <default>
    local v
    v=$(sed -n "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*//p" "$1" 2>/dev/null | head -1 | sed 's/[[:space:]]*$//')
    [ -n "$v" ] && echo "$v" || echo "$3"
}

# One line per test: "<rank> <order> <dir> <group> <scenario> <needs> <timeout>"
# Sorting this text is what makes the run order explicit and inspectable —
# `--list` prints exactly what the loop below will do.
collect() {
    local m d g o s n t p
    for m in $(find tests -name meta -type f | sort); do
        d=$(dirname "$m")
        g=$(meta_get "$m" group integration)
        o=$(meta_get "$m" order 50)
        s=$(meta_get "$m" scenario "$DEFAULT_SCENARIO")
        n=$(meta_get "$m" needs none)
        t=$(meta_get "$m" timeout 300)
        p=$(meta_get "$m" provides none)
        [ "$(meta_get "$m" skip no)" = "yes" ] && continue
        if [ -n "$WANT_GROUPS" ]; then
            case " $WANT_GROUPS " in *" $g "*) ;; *) continue ;; esac
        fi
        [ -n "$ONLY" ] && case "$d" in *"$ONLY"*) ;; *) continue ;; esac
        printf '%s %04d %s %s %s %s %s %s\n' "$(group_rank "$g")" "$o" "$d" "$g" "$s" "$n" "$t" "$p"
    done | sort -k1,1 -k2,2n -k3,3
}

if [ "$LIST" -eq 1 ]; then
    printf '%-4s %-12s %-46s %-12s %s\n' 'ord' 'group' 'test' 'scenario' 'needs'
    while read -r rank order dir group scen needs timeout prov; do
        [ -z "${dir:-}" ] && continue
        printf '%-4s %-12s %-46s %-12s %s\n' "$order" "$group" "$dir" "$scen" "$needs"
    done <<< "$(collect)"
    exit 0
fi

# ---------------------------------------------------------------
hdr "unit tests"
# ---------------------------------------------------------------
# C++ only, no database, milliseconds. This tier must never be allowed to
# acquire a database dependency — the moment it does, the fast feedback loop
# that makes it worth having is gone.
# ---------------------------------------------------------------
if [ "$UNIT" -eq 1 ]; then
    if ! cmake --build ./build --target erp_tests > "$LOGDIR/unit_build.log" 2>&1; then
        echo "  build FAILED — see $LOGDIR/unit_build.log"
        grep -E "error" "$LOGDIR/unit_build.log" | head -20 | sed 's/^/    /'
        fail "erp_tests (build)"
    elif ./build/erp_tests $FILTER > "$LOGDIR/unit.log" 2>&1; then
        grep -E "cases? run|All tests passed" "$LOGDIR/unit.log" | sed 's/^/  /'
        pass "erp_tests"
    else
        grep -E "FAILED|ABORTED|FAIL " "$LOGDIR/unit.log" | head -30 | sed 's/^/  /'
        fail "erp_tests"
    fi
else
    echo "  skipped (--no-unit)"
fi

if [ "$INTEGRATION" -eq 0 ]; then
    hdr "summary"
    echo "  $PASSED passed, $FAILED failed  (unit only)"
    [ "$FAILED" -eq 0 ] || { echo "  failed:$FAILED_NAMES"; exit 1; }
    exit 0
fi

# ---------------------------------------------------------------
hdr "server"
# ---------------------------------------------------------------
# Started here rather than assumed, so a green run cannot be a run that
# silently skipped everything.
#
# `pkill -x` matches the executable name exactly. Never `pkill -f` on the test
# path here: that pattern also matches this script's own command line, and
# killing yourself reads as a mysterious exit 143.
for stale in $(pgrep -f 'bash tests/.*/test.sh' 2>/dev/null); do
    [ "$stale" = "$$" ] && continue
    kill -9 "$stale" 2>/dev/null && echo "  cleaned up stale test process $stale"
done

if ! curl -sf -o /dev/null --max-time 3 "$BASE/healthz"; then
    echo "  not responding at $BASE — starting it"
    pkill -x c-erp 2>/dev/null
    sleep 1
    # setsid so the server is not a child of this script; otherwise every
    # command substitution in every test waits for it to close the pipe.
    (setsid ./build/c-erp > /tmp/cerp_run.log 2>&1 < /dev/null &)
    for _ in $(seq 1 20); do
        curl -sf -o /dev/null --max-time 2 "$BASE/healthz" && break
        sleep 1
    done
    if ! curl -sf -o /dev/null --max-time 3 "$BASE/healthz"; then
        echo "  could not start the server — see /tmp/cerp_run.log"
        tail -20 /tmp/cerp_run.log | sed 's/^/    /'
        fail "server startup"
        hdr "summary"; echo "  $PASSED passed, $FAILED failed"; exit 1
    fi
fi
echo "  up at $BASE"

# ---------------------------------------------------------------
hdr "snapshot"
# ---------------------------------------------------------------
# Taken AFTER the server is confirmed up, so the migrations a fresh boot runs
# are already in the snapshot — restoring must not undo them.
#
# It is taken even with --keep-db: it costs a second and it is the only way
# back if a test corrupts something. Only the RESTORE is optional.
SNAP_OK=0
if bash scripts/db_snapshot.sh take "$SNAPSHOT"; then
    # Verified now, not at restore time. A snapshot that turns out to be
    # corrupt only when it is needed is not a snapshot.
    bash scripts/db_snapshot.sh verify "$SNAPSHOT" && SNAP_OK=1
fi
[ "$SNAP_OK" -eq 1 ] || echo "  unusable — the suite will run WITHOUT a restore afterwards"

# ---------------------------------------------------------------
# Scenario switching.
#
# Restores happen only when the requested scenario NAME CHANGES, so twenty
# tests that all want `baseline` restore once, not twenty times.
#
# Note what this does and does not promise: it guarantees the same STARTING
# state for a run, not isolation between tests within it — tests still see
# each other's rows, which is why the fixture set is canonical and prefixed.
# ---------------------------------------------------------------
CUR_SCENARIO=""
FIXTURES_ACTIVE=0

# Wait until the server can actually SERVE A LOGIN, not merely answer a socket.
#
# Loading a scenario stops the server, drops the schema, restores and starts a
# fresh process — and db_snapshot.sh calls that done as soon as `GET /` answers.
# It answers too early: the website route catches its own database error and
# redirects to /login either way, so a 302 arrives while the pool is still
# warming. `/healthz` is no better — it is a static 200 that touches nothing.
#
# The result was the first test or two after a restore failing at
# `auth_or_die` with "cannot authenticate", having tested nothing at all. Both
# passed standalone a minute later, which is the signature of a race and the
# most expensive kind of red: it looks like the code and it is not.
#
# A login is the cheapest request that proves the DB pool, the session manager
# and res_users are all up, so poll that.
wait_for_login() {
    local i
    for i in $(seq 1 30); do
        curl -s -X POST "$BASE/web/session/authenticate" \
             -H 'Content-Type: application/json' --max-time 3 \
             --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"${ERP_LOGIN:-admin}\",\"password\":\"${ERP_PASSWORD:-admin}\"}}" \
          | grep -q '"session_id"' && return 0
        sleep 1
    done
    echo "    NOTE: the server is up but will not accept a login after 30s"
    return 1
}

switch_scenario() {  # switch_scenario <name>
    local want="$1"
    [ "$want" = "$CUR_SCENARIO" ] && return 0
    if [ "$want" = "current" ]; then CUR_SCENARIO="current"; return 0; fi
    if [ "$SNAP_OK" -ne 1 ]; then
        # Loading a scenario is destructive. Doing it with no way back would
        # trade reproducibility for the user's data, which is a bad trade.
        echo "    scenario '$want' skipped — no snapshot of the working database"
        CUR_SCENARIO="current"
        return 1
    fi
    printf '    \033[2mscenario -> %s\033[0m\n' "$want"
    if scenario_load "$want" > "$LOGDIR/scenario.log" 2>&1; then
        CUR_SCENARIO="$want"
        wait_for_login
        # A restore wipes the fixture set with everything else. Anything that
        # already asked for it needs it back, or every later test reading "the
        # first product" fails for a reason that is ours, not the code's.
        if [ "$FIXTURES_ACTIVE" -eq 1 ]; then
            ( source tests/lib/fixtures.sh && fx_create ) > /dev/null 2>&1
        fi
        return 0
    fi
    echo "    could not load scenario '$want' — see $LOGDIR/scenario.log"
    sed 's/^/      /' "$LOGDIR/scenario.log" | tail -5
    CUR_SCENARIO="current"
    return 1
}

# ---------------------------------------------------------------
hdr "tests"
# ---------------------------------------------------------------
LAST_GROUP=""
while read -r rank order dir group scen needs timeout prov; do
    [ -z "${dir:-}" ] && continue
    name="${dir#tests/}"
    log="$LOGDIR/$(echo "$name" | tr '/' '_').log"

    if [ "$group" != "$LAST_GROUP" ]; then
        printf '\n  \033[1m— %s —\033[0m\n' "$group"
        LAST_GROUP="$group"
    fi

    if [ ! -f "$dir/test.sh" ]; then
        skip "$name (no test.sh)"
        continue
    fi

    switch_scenario "$scen"

    # needs=fixtures     the runner seeds them if nobody has yet
    # provides=fixtures  the test creates them itself (the setup group), so it
    #                    is left to do its own job — but the runner now knows
    #                    they exist and must be re-seeded after any scenario
    #                    switch that wipes them.
    case "$needs" in
        *fixtures*)
            if [ "$FIXTURES_ACTIVE" -eq 0 ]; then
                ( source tests/lib/fixtures.sh && fx_create ) > /dev/null 2>&1
                FIXTURES_ACTIVE=1
            fi ;;
    esac
    case "$prov" in *fixtures*) FIXTURES_ACTIVE=1 ;; esac

    # Output goes to a FILE, never `out=$(...)`. Several tests restart the
    # server; a command substitution waits for every writer to close the pipe,
    # and a server that stays up by design never does — that hung the suite
    # indefinitely on the first test that restarted it.
    #
    # </dev/null so nothing can block on a prompt. setsid + --kill-after so a
    # wedged test that spawned children dies as a whole process group rather
    # than surviving as an orphan that rewrites settings under the next test.
    setsid timeout --kill-after=10 "$timeout" bash "$dir/test.sh" < /dev/null > "$log" 2>&1
    rc=$?

    checks=$(grep -c 'PASS' "$log" 2>/dev/null)
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        fail "$name (timed out after ${timeout}s)"
        tail -5 "$log" | sed 's/^/        /'
    elif grep -q "FAILURES" "$log"; then
        fail "$name"
        grep "FAIL" "$log" | head -8 | sed 's/^/        /'
    elif grep -q "All checks passed" "$log"; then
        pass "$name ($checks checks)"
    else
        # No verdict line at all: the test died early. That must never be
        # scored as a pass — a suite that silently drops a broken test is
        # worse than one that fails.
        fail "$name (no verdict — did not complete)"
        tail -5 "$log" | sed 's/^/        /'
    fi
done <<< "$(collect)"

# ---------------------------------------------------------------
hdr "restore"
# ---------------------------------------------------------------
# The last thing the suite does, whether it passed or failed. It runs before
# the summary so a restore problem cannot hide under a wall of PASS lines, and
# the exit code still reflects the TESTS: a failed restore is reported loudly
# but does not turn a green run red, because the two say different things.
if [ "$RESTORE" -eq 0 ]; then
    echo "  skipped (--keep-db). Snapshot kept for a manual restore:"
    echo "      ./scripts/db_snapshot.sh restore $SNAPSHOT"
elif [ "$SNAP_OK" -ne 1 ]; then
    echo "  skipped — no usable snapshot was taken. Test data from this run is still present."
elif bash scripts/db_snapshot.sh restore "$SNAPSHOT"; then
    echo "  the database is back to its pre-test state"
else
    echo "  *** THE DATABASE WAS NOT RESTORED ***"
    echo "  The snapshot is at $SNAPSHOT — restore it before running the suite again:"
    echo "      ./scripts/db_snapshot.sh restore $SNAPSHOT"
fi

# ---------------------------------------------------------------
hdr "summary"
# ---------------------------------------------------------------
echo "  $PASSED passed, $FAILED failed, $SKIPPED skipped"
echo "  logs in $LOGDIR/"
if [ "$FAILED" -ne 0 ]; then
    echo "  failed:$FAILED_NAMES"
    exit 1
fi
echo "  Everything green."

#!/usr/bin/env bash
# =============================================================
# assert.sh — the verdict protocol every test speaks.
#
# The runner scores a test by reading its OUTPUT, not its exit code, because a
# bash script that dies at line 40 exits 0 more often than anyone expects. So
# the contract is:
#
#     "  All checks passed."   -> pass
#     "  *** FAILURES ***"     -> fail
#     neither                  -> FAIL (the script did not complete)
#
# That last rule is the load-bearing one. A missing verdict can never be scored
# as a pass, so a test that crashes early is reported as broken rather than
# silently disappearing from the suite.
# =============================================================

# Guard against double-sourcing: a test may source harness.sh while a helper it
# calls does the same, and re-running this would zero the counters mid-run.
[ -n "${ERP_ASSERT_LOADED:-}" ] && return 0
ERP_ASSERT_LOADED=1

FAILED=
T_PASS=0
T_FAIL=0

ok(){ T_PASS=$((T_PASS+1)); echo "    PASS  $1"; }
no(){ T_FAIL=$((T_FAIL+1)); FAILED=1; echo "    FAIL  $1"; }

# Section heading. Purely cosmetic, but it is what makes a 60-check log
# readable when it fails at check 47.
sec(){ echo "############ $1 ############"; }

# t_eq <expected> <actual> <label>
#
# Prints what it actually got on failure. An assertion that says only
# "totals wrong" costs a debugging round-trip that "expected 100, got 0" does
# not.
t_eq(){
    if [ "$1" = "$2" ]; then ok "$3"
    else no "$3 (expected '$1', got '$2')"; fi
}

t_ne(){
    if [ "$1" != "$2" ]; then ok "$3"
    else no "$3 (got the forbidden value '$1')"; fi
}

# t_nonempty <value> <label>
t_nonempty(){
    if [ -n "$1" ]; then ok "$2"; else no "$2 (empty)"; fi
}

# t_contains <haystack> <needle> <label>
t_contains(){
    case "$1" in *"$2"*) ok "$3" ;; *) no "$3 (no '$2' in: $(echo "$1" | head -c 160))" ;; esac
}

# t_lacks <haystack> <needle> <label>  — for disclosure tests, where the point
# is that something is ABSENT.
t_lacks(){
    case "$1" in *"$2"*) no "$3 (found '$2' in: $(echo "$1" | head -c 160))" ;; *) ok "$3" ;; esac
}

# t_ge <actual> <minimum> <label>
t_ge(){
    if [ -n "$1" ] && [ "$1" -ge "$2" ] 2>/dev/null; then ok "$3 ($1)"
    else no "$3 (wanted >= $2, got '$1')"; fi
}

# verdict — the last line of every test. Exits non-zero as well as printing,
# so a test invoked directly from a shell also behaves like a normal command.
verdict(){
    if [ -z "$FAILED" ]; then
        echo "  All checks passed."
        return 0
    fi
    echo "  *** FAILURES ***"
    return 1
}

# Called via `trap` by tests that want the verdict emitted even on an early
# `exit`. Without this, `exit 1` halfway through prints no verdict and the
# runner reports "did not complete" — which is correct, but less useful than
# saying which check failed.
verdict_on_exit(){ trap 'verdict' EXIT; }

#!/bin/bash
# =============================================================
# Negative control for run_tests.sh.
#
# A green suite is only meaningful if a red one is actually reported as
# red. This plants two deliberately broken scripts and asserts that the
# runner notices BOTH failure shapes and exits non-zero:
#
#   1. a script that declares failure    -> "*** FAILURES ***"
#   2. a script that dies with no verdict -> must NOT be scored as a pass
#
# Case 2 is the one that matters. Two verifications earlier in this
# project passed while proving nothing, because a script exited before
# reaching its assertions and the absence of "FAIL" was read as success.
# =============================================================
cd "$(dirname "$0")/.." || exit 1

BAD1=scripts/verify_zzz_negctl_declared.sh
BAD2=scripts/verify_zzz_negctl_silent.sh
cleanup() { rm -f "$BAD1" "$BAD2"; }
trap cleanup EXIT

cat > "$BAD1" <<'EOF'
#!/bin/bash
echo "    FAIL  deliberately broken"
echo "  *** FAILURES ***"
EOF

cat > "$BAD2" <<'EOF'
#!/bin/bash
echo "    PASS  looks fine so far"
exit 0        # dies before any verdict — must not count as a pass
EOF
chmod +x "$BAD1" "$BAD2"

echo "############ running the suite with two broken scripts planted ############"
bash scripts/run_tests.sh < /dev/null > /tmp/negctl.log 2>&1
RC=$?
echo "    runner exit code: $RC"

FAILED=
grep -q "verify_zzz_negctl_declared" /tmp/negctl.log \
    && echo "    PASS  declared failure was reported" \
    || { echo "    FAIL  declared failure went unreported"; FAILED=1; }

grep -q "verify_zzz_negctl_silent (no verdict" /tmp/negctl.log \
    && echo "    PASS  silent early exit was reported, not scored as a pass" \
    || { echo "    FAIL  a script with no verdict was not flagged"; FAILED=1; }

[ "$RC" -ne 0 ] \
    && echo "    PASS  runner exited non-zero ($RC) — CI would catch this" \
    || { echo "    FAIL  runner exited 0 despite failures"; FAILED=1; }

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

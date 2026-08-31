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
# S-40 proof: rate-limit buckets must be per-CLIENT, not shared.
#
# Pre-fix, every request behind nginx looked like 127.0.0.1, so exhausting
# the limiter from one client locked out everyone. This drives client A
# past kMaxAttempts (10) and then checks client B is still served.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

bad_login() {   # $1 = X-Real-IP value ; echoes the JSON-RPC error name
    curl -s -X POST "$BASE/web/session/authenticate" \
         -H 'Content-Type: application/json' \
         -H "X-Real-IP: $1" \
         --data "{\"jsonrpc\":\"2.0\",\"params\":{\"db\":\"$DBN\",\"login\":\"nosuch\",\"password\":\"wrong\"}}" \
    | grep -o '"message":"[^"]*"' | head -1
}

A=203.0.113.10
B=198.51.100.20

echo "############ S-40 — bucket separation ############"
echo "  driving client A ($A) past the 10-attempt window..."
for i in $(seq 1 13); do
    r=$(bad_login "$A")
    printf "    A attempt %-2s -> %s\n" "$i" "$r"
done

echo
echo "  client A should now be throttled:"
RA=$(bad_login "$A")
echo "    A -> $RA"

echo
echo "  client B ($B) must be UNAFFECTED:"
RB=$(bad_login "$B")
echo "    B -> $RB"

echo
echo "############ RESULT ############"
FAILED=
if echo "$RA" | grep -qi 'too many'; then
    echo "    PASS  client A is rate-limited"
else
    echo "    FAIL  client A was NOT limited (expected 'Too many requests')"; FAILED=1
fi
if echo "$RB" | grep -qi 'too many'; then
    echo "    FAIL  client B was locked out by A's attempts — buckets are SHARED"; FAILED=1
else
    echo "    PASS  client B still served — buckets are per-client"
fi

echo
echo "  XFF last-element check (client sends a forged prefix):"
RC=$(curl -s -X POST "$BASE/web/session/authenticate" \
      -H 'Content-Type: application/json' \
      -H "X-Forwarded-For: $A, 198.51.100.77" \
      --data "{\"jsonrpc\":\"2.0\",\"params\":{\"db\":\"$DBN\",\"login\":\"nosuch\",\"password\":\"wrong\"}}" \
      | grep -o '"message":"[^"]*"' | head -1)
echo "    XFF='$A, 198.51.100.77' -> $RC"
if echo "$RC" | grep -qi 'too many'; then
    echo "    FAIL  bucketed on the FIRST (forgeable) element"; FAILED=1
else
    echo "    PASS  bucketed on the LAST element — a forged prefix cannot"
    echo "          borrow, or poison, another client's bucket"
fi

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
# ---------------------------------------------------------------------------
# =============================================================
# The decisive S-40 test, through a real nginx.
#
# Threat: an attacker rotates X-Real-IP / X-Forwarded-For on every
# request, hoping each forged value lands in a fresh rate-limit bucket
# and brute-force proceeds unthrottled.
#
# Correct behaviour: nginx OVERWRITES X-Real-IP from $remote_addr and
# APPENDS the true peer to X-Forwarded-For, and the app reads the last
# element — so all 14 requests below must share ONE bucket and the
# limiter must engage, regardless of the forged headers.
#
# Requires the test proxy from ../nginx-proxy/test.sh to be running, so it is
# NOT in the automated run (meta: skip=yes). Run the pair by hand:
#
#   bash tests/security/hardening/nginx-proxy/test.sh
#   bash tests/security/hardening/nginx-forge/test.sh
# =============================================================
PREFIX=/tmp/nginx-cerp-test
HTTPS_PORT=8443
FAILED=

ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

if [ ! -f "$PREFIX/nginx.pid" ]; then
    echo "test proxy not running — start it with tests/security/hardening/nginx-proxy/test.sh"; exit 1
fi

# Throttling can come from either layer, and they look different:
#   nginx limit_req  -> HTTP 503 with an HTML body (no JSON at all)
#   app  limiter     -> HTTP 200 with {"message":"Too many failed login..."}
# Counting only the JSON form silently misses the edge limiter and reads as
# "not limited", which is the opposite of the truth.
attempt() {  # $1 = forged ip ; echoes "<http_code>|<message>"
    local out
    out=$(curl -sk -w '\n%{http_code}' \
        -X POST "https://127.0.0.1:$HTTPS_PORT/web/session/authenticate" \
        -H 'Content-Type: application/json' \
        -H "X-Real-IP: $1" -H "X-Forwarded-For: $1" \
        --data '{"jsonrpc":"2.0","params":{"db":"odoo","login":"nosuch","password":"bad"}}')
    local code=${out##*$'\n'}
    local body=${out%$'\n'*}
    local msg
    msg=$(printf '%s' "$body" | grep -o '"message":"[^"]*"' | head -1)
    printf '%s|%s' "$code" "${msg:-<no json>}"
}

echo "############ attacker rotates a fresh forged IP every request ############"
EDGE=0; APP=0; ALLOWED=0
for i in $(seq 1 14); do
    FORGED="203.0.113.$i"
    R=$(attempt "$FORGED")
    CODE=${R%%|*}; MSG=${R#*|}
    TAG="allowed"
    if [ "$CODE" = "503" ]; then TAG="THROTTLED (nginx edge)"; EDGE=$((EDGE+1))
    elif echo "$MSG" | grep -qi 'too many'; then TAG="THROTTLED (app)"; APP=$((APP+1))
    else ALLOWED=$((ALLOWED+1)); fi
    printf "    %-2s forged=%-16s http=%-3s  %s\n" "$i" "$FORGED" "$CODE" "$TAG"
done

echo
echo "############ RESULT ############"
echo "    allowed=$ALLOWED  throttled_by_nginx=$EDGE  throttled_by_app=$APP"
if [ $((EDGE + APP)) -gt 0 ]; then
    ok "limiter engaged despite 14 distinct forged IPs"
    ok "forged headers cannot buy a fresh bucket"
else
    no "*** every forged IP got its own bucket — rate limiting is bypassable ***"
fi
[ "$EDGE" -gt 0 ] && echo "    (nginx's limit_req fired first — defence in depth working as designed)"

echo
echo "############ control: same test bypassing nginx ############"
echo "    (direct to the app, where the forged header IS trusted because"
echo "     the peer is loopback — this is why nginx must overwrite it)"
LIMITED_DIRECT=0
for i in $(seq 1 14); do
    R=$(curl -s -X POST "http://127.0.0.1:8069/web/session/authenticate" \
        -H 'Content-Type: application/json' \
        -H "X-Real-IP: 198.51.100.$i" \
        --data '{"jsonrpc":"2.0","params":{"db":"odoo","login":"nosuch","password":"bad"}}' \
        | grep -o '"message":"[^"]*"' | head -1)
    echo "$R" | grep -qi 'too many' && LIMITED_DIRECT=$((LIMITED_DIRECT+1))
done
echo "    throttled: $LIMITED_DIRECT/14"
if [ "$LIMITED_DIRECT" -eq 0 ]; then
    echo "    NOTE  direct access lets a client pick its bucket — expected, and"
    echo "          exactly why the app must bind 127.0.0.1 and nginx must be"
    echo "          the only ingress (deploy/README.md §2)."
fi

echo
[ -n "$FAILED" ] && echo "  *** FAILED ***" || echo "  All checks passed."

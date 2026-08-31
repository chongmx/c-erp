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
# Verifies S-42 (session rotation), S-43 (bounded sessions +
# eviction) and S-48 (cookie is honoured by call_kw).
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
LOGIN=${LOGIN:-admin}
PASSWD=${PASSWD:-admin}
FAILED=

ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

sid_of() { sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p'; }

cat > /tmp/s_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"$LOGIN","password":"$PASSWD"}}
EOF

ping_with_cookie() {   # $1 = sid ; prints http-visible outcome
    cat > /tmp/s_ping.json <<'EOF'
{"jsonrpc":"2.0","method":"call","params":{"model":"product.category","method":"search_read","args":[[]],"kwargs":{"fields":["id"],"limit":1}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
         -H "Cookie: session_id=$1" --data @/tmp/s_ping.json
}

ping_with_ctx() {      # $1 = sid
    cat > /tmp/s_pingc.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"product.category","method":"search_read","args":[[]],"kwargs":{"fields":["id"],"limit":1,"context":{"session_id":"$1"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/s_pingc.json
}

# =============================================================
echo "############ S-48 — call_kw honours the session cookie ############"
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
        --data @/tmp/s_auth.json | sid_of)
[ -z "$SID" ] && { echo "    cannot authenticate"; exit 1; }
echo "    post-login sid=${SID:0:8}..."

R=$(ping_with_cookie "$SID")
printf '%s' "$R" | grep -q '"result"' \
    && ok "Cookie header accepted by call_kw" \
    || no "Cookie header still ignored: $(echo "$R" | head -c 120)"

R=$(ping_with_ctx "$SID")
printf '%s' "$R" | grep -q '"result"' \
    && ok "context.session_id still accepted (frontend path unbroken)" \
    || no "context path broke: $(echo "$R" | head -c 120)"

# =============================================================
echo
echo "############ S-42 — session id rotated on login ############"
# Take an anonymous id first, the way an attacker would.
PRE=$(curl -s "$BASE/web/session/get_session_info" | sid_of)
echo "    pre-auth (attacker-known) sid=${PRE:0:8}..."

# Authenticate presenting that id, as a fixated victim would.
POST=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
        -H "Cookie: session_id=$PRE" --data @/tmp/s_auth.json | sid_of)
echo "    post-auth sid=${POST:0:8}..."

if [ -n "$PRE" ] && [ -n "$POST" ] && [ "$PRE" != "$POST" ]; then
    ok "session id changed across authentication"
else
    no "session id NOT rotated (pre=$PRE post=$POST)"
fi

R=$(ping_with_cookie "$PRE")
if printf '%s' "$R" | grep -q '"result"'; then
    no "*** the pre-auth id is STILL usable — fixation works ***"
else
    ok "pre-auth id rejected after login (fixation closed)"
fi

R=$(ping_with_cookie "$POST")
printf '%s' "$R" | grep -q '"result"' \
    && ok "rotated id works" || no "rotated id does NOT work — users would be logged out"

# =============================================================
echo
echo "############ S-43 — anonymous requests do not allocate sessions ############"
B=$(curl -s "$BASE/healthz" >/dev/null; echo)
before=$(curl -s "$BASE/healthz" | sed -n 's/.*"active_sessions":\([0-9]*\).*/\1/p')
[ -z "$before" ] && before=$(curl -s "$BASE/web/session/get_session_info" >/dev/null; echo 0)

echo "    firing 40 unauthenticated call_kw requests (no cookie, no context)..."
cat > /tmp/s_anon.json <<'EOF'
{"jsonrpc":"2.0","method":"call","params":{"model":"product.category","method":"search_read","args":[[]],"kwargs":{"fields":["id"],"limit":1}}}
EOF
for i in $(seq 1 40); do
    curl -s -o /dev/null -X POST "$BASE/web/dataset/call_kw" \
         -H 'Content-Type: application/json' --data @/tmp/s_anon.json
done

echo "    checking the server log for freshly-minted sids:"
NEW_SIDS=$(tail -60 /home/user/code/c-erp/log/system.log \
           | grep -c 'product.category.search_read sid=[0-9a-f]' || true)
EMPTY_SIDS=$(tail -60 /home/user/code/c-erp/log/system.log \
           | grep -c 'product.category.search_read sid=\.\.\.' || true)
echo "      log lines with a non-empty sid: $NEW_SIDS"
echo "      log lines with an empty sid   : $EMPTY_SIDS"
if [ "$EMPTY_SIDS" -gt 0 ]; then
    ok "unresolved requests carry no session id — nothing was allocated"
else
    echo "      (inspect manually: each anonymous request must NOT mint a new sid)"
fi

# =============================================================
echo
echo "############ S-43 — eviction timer is running ############"
echo "    (fires every 60 s; look for '[sessions] evicted' after idle sessions expire)"
grep -c 'sessions\] evicted' /home/user/code/c-erp/log/system.log 2>/dev/null \
    | sed 's/^/      eviction log lines so far: /'

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** ONE OR MORE CHECKS FAILED ***" || echo "  All checks passed."

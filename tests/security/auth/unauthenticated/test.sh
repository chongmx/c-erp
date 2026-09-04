#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# SECURITY — the front door.
#
# THE CONTROL: every data endpoint requires a live session. Nothing here is
# subtle; it is the check that stops being true by accident, when a new route
# is added and the session lookup is forgotten.
#
# What is attacked:
#   * no session at all
#   * a forged session id (right shape, never issued)
#   * a session id that was issued and then destroyed
#   * a wrong password, and whether the response distinguishes "no such user"
#     from "wrong password" (it must not — that is a user enumeration oracle)
#   * the cookie flags on the session cookie
#
# Every check asserts a REFUSAL. A test in this group that starts passing
# because a call succeeded has failed at its job.
# =============================================================

refused() {  # refused <label> <response>
    if has_error "$2"; then ok "$1 is refused"
    else no "$1 was ACCEPTED — $(echo "$2" | head -c 160)"; fi
}

sec "1. no session"
refused "an unauthenticated read"  "$(call_as '' res.partner search_read '[[],["name"]]')"
refused "an unauthenticated write" "$(call_as '' res.partner create '[{"name":"PENTEST"}]')"
t_eq "0" "$(pg "SELECT count(*) FROM res_partner WHERE name='PENTEST'")" "and it wrote nothing"

sec "2. a forged session id"
# The right shape, never issued. A server that only checks the FORMAT of a
# session id, rather than looking it up, passes the first check and fails this.
refused "a well-formed but unissued session" \
        "$(call_as 'deadbeefdeadbeefdeadbeefdeadbeef' res.partner search_read '[[],["name"]]')"
refused "a session id of the wrong shape" \
        "$(call_as '../../etc/passwd' res.partner search_read '[[],["name"]]')"

sec "3. a destroyed session"
# Logout is a JSON-RPC method on res.users, not an HTTP route — it is on the
# public (pre-auth) method list in JsonRpcDispatcher. Getting this wrong is
# instructive: POSTing to a route that does not exist "logs out" nothing, the
# session keeps working, and the test reads that as a critical vulnerability.
# Always confirm the logout actually happened before concluding it failed.
VICTIM=$(login)
t_nonempty "$VICTIM" "a real session was issued"
if [ -n "$VICTIM" ]; then
    OK=$(call_as "$VICTIM" res.partner search_read '[[],["name"]]')
    has_error "$OK" && no "a freshly issued session did not work" || ok "it works before logout"

    OUT=$(call_as "$VICTIM" res.users logout '[]')
    has_error "$OUT" && no "logout itself failed: $(echo "$OUT" | head -c 160)" \
                     || ok "logout was accepted"

    AFTER=$(call_as "$VICTIM" res.partner search_read '[[],["name"]]')
    # Logging out must invalidate the id SERVER-SIDE. A logout that only drops
    # the cookie leaves a working credential in every proxy log it passed
    # through, and in the shell history of whoever was debugging with curl.
    refused "the same session after logout" "$AFTER"
fi

sec "4. bad credentials"
BAD1=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
       --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"wrong-password\"}}")
BAD2=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
       --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"no-such-user-here\",\"password\":\"wrong-password\"}}")
case "$BAD1" in *'"session_id"'*) no "a wrong password was accepted" ;; *) ok "a wrong password is refused" ;; esac
case "$BAD2" in *'"session_id"'*) no "an unknown user was accepted"  ;; *) ok "an unknown user is refused" ;; esac

# User enumeration: the two failures must be indistinguishable. If "wrong
# password" and "no such user" read differently, an attacker can harvest valid
# logins without ever guessing one right.
M1=$(jfield "$BAD1" message); M2=$(jfield "$BAD2" message)
echo "    wrong password: '$M1'"
echo "    unknown user:   '$M2'"
t_eq "$M1" "$M2" "both failures give the same message (no user enumeration)"

sec "5. the session cookie"
HDRS=$(curl -s -D - -o /dev/null -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
       --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}")
COOKIE=$(echo "$HDRS" | grep -i '^set-cookie' | head -1)
echo "    $COOKIE"
if [ -z "$COOKIE" ]; then
    no "no Set-Cookie on a successful authenticate"
else
    case "$COOKIE" in *[Hh]ttp[Oo]nly*) ok "HttpOnly is set — script cannot read the session" ;;
                      *) no "HttpOnly is MISSING: any XSS becomes session theft" ;; esac
    case "$COOKIE" in *[Ss]ame[Ss]ite*) ok "SameSite is set" ;;
                      *) no "SameSite is MISSING: the session rides cross-site requests" ;; esac
    # Secure depends on deployment (secureCookies in the config) and is a
    # NOTE over plain HTTP rather than a failure, because this suite runs on
    # 127.0.0.1 where the flag would break every other test.
    case "$COOKIE" in *[Ss]ecure*) ok "Secure is set" ;;
                      *) echo "    NOTE  Secure is not set — expected over http://127.0.0.1; it MUST be on in production" ;; esac
fi

verdict

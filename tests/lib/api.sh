#!/usr/bin/env bash
# =============================================================
# api.sh — talking to the running server.
#
# Every integration and functional test drives the real HTTP API rather than
# calling into C++, because that is the only way to exercise what unit tests
# structurally cannot: routing, session handling, JSON coercion, field
# registration and the SQL underneath.
#
# Authentication is lazy and cached in $SID. A test that never calls the API
# never pays for a login.
# =============================================================
[ -n "${ERP_API_LOADED:-}" ] && return 0
ERP_API_LOADED=1

BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
ERP_LOGIN=${ERP_LOGIN:-admin}
ERP_PASSWORD=${ERP_PASSWORD:-admin}

# api_up — is anything listening? Used by the runner before it starts a group,
# and by tests that want to skip cleanly rather than emit 40 curl failures.
api_up(){ curl -sf -o /dev/null --max-time 3 "$BASE/healthz"; }

# login [user] [password] -> prints a session id
#
# Returns empty on failure; callers must check. Every test that cannot
# authenticate should say so and emit a FAILURES verdict, never carry on
# making unauthenticated calls that fail for the wrong reason.
login(){
    curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"${1:-$ERP_LOGIN}\",\"password\":\"${2:-$ERP_PASSWORD}\"}}" \
        | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p'
}

# auth — logs in once, exports SID and CTX. Idempotent.
auth(){
    [ -n "${SID:-}" ] && return 0
    SID=$(login)
    [ -z "$SID" ] && return 1
    CTX="\"context\":{\"session_id\":\"$SID\"}"
    export SID CTX
    return 0
}

# auth_or_die — the standard opening move. A test that cannot log in has
# nothing to say about the code, so it fails loudly and stops.
auth_or_die(){
    if ! auth; then
        echo "    FAIL  cannot authenticate at $BASE as ${ERP_LOGIN}"
        echo "  *** FAILURES ***"
        exit 1
    fi
}

# call <model> <method> <args-json>  -> raw JSON-RPC response
#
# NOTE the shape: args is the ARRAY, so callers pass '[{...}]' not '{...}'.
# Double-wrapping it is the single most common mistake writing these tests.
call(){
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":{${CTX:-}}}}"
}

# call_k <model> <method> <args-json> <extra-kwargs-json>
#
# For the methods that read kwargs rather than positional args —
# action_register_payment takes payment_date / journal_id / amount that way.
# The extra fragment is inlined next to the context, so pass it WITHOUT the
# surrounding braces: '"amount":250,"memo":"x"'.
call_k(){
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":{${CTX:-}${4:+,$4}}}}"
}

# call_as <session> <model> <method> <args-json> — for the security group,
# which needs to make calls as somebody else (or as nobody).
call_as(){
    local sid="$1"; shift
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":{\"context\":{\"session_id\":\"$sid\"}}}}"
}

# rid — filter: pull a bare integer result out of a create() response.
rid(){ sed -n 's/.*"result":\([0-9][0-9]*\).*/\1/p'; }

# jfield <json> <key> — first value of a scalar key, without a JSON parser.
# Good enough for assertions; anything structural should use python3/jq.
jfield(){ echo "$1" | sed -n "s/.*\"$2\":\"\{0,1\}\([^,\"}]*\)\"\{0,1\}.*/\1/p" | head -1; }

# has_error <json> — did the server return a JSON-RPC error?
#
# Matches the error OBJECT, not the bare word. A payload of its own may well
# contain an "error" key: the BOM importer returns
# {"result":{"counts":{"error":0,"ok":3}}} on a completely successful import,
# and a looser match reads that as a failure — a test that fails when the
# thing it tests worked perfectly.
has_error(){
    case "$1" in
        *'"error":{'*|*'"error": {'*) return 0 ;;
        *) return 1 ;;
    esac
}

# GET/POST an HTTP route with the session cookie, for the routes that are not
# JSON-RPC (reports, portal pages, uploads).
http_get(){ curl -s -H "Cookie: session_id=${SID:-}" "$BASE$1"; }
http_code(){ curl -s -o /dev/null -w '%{http_code}' -H "Cookie: session_id=${SID:-}" "$BASE$1"; }

# =============================================================
# The customer portal — a SEPARATE authentication surface.
#
# Different login route, different cookie (`portal_sid`), different session
# store, and a partner id rather than a user id. A staff `session_id` buys
# nothing on /portal/api/*, and a portal cookie buys nothing on
# /web/dataset/call_kw — which is the entire point, and why these are their own
# helpers rather than a flag on call().
#
# Every helper takes the session EXPLICITLY. A portal test is mostly about what
# customer B cannot see, so it juggles two live sessions at once; a cached
# global like $SID would quietly make half those assertions test the wrong
# customer and pass.
# =============================================================
PORTAL_COOKIE=${PORTAL_COOKIE:-portal_sid}

# portal_login <email> <password> -> portal session id, empty when refused.
# Read off Set-Cookie rather than the body: the cookie IS the credential, so a
# route that answers 200 without setting one has not logged anybody in.
portal_login(){
    curl -s -i -X POST "$BASE/portal/api/login" -H 'Content-Type: application/json' \
        --data "{\"email\":\"$1\",\"password\":\"$2\"}" \
    | tr -d '\r' \
    | sed -n "s/^[Ss]et-[Cc]ookie: *$PORTAL_COOKIE=\([^;]*\).*/\1/p" | head -1
}

# portal_get  <psid> <path>  -> body
# portal_code <psid> <path>  -> HTTP status only
# portal_post <psid> <path> <json-body> -> body
#
# Pass an empty psid to make the call as an anonymous visitor.
portal_get(){  curl -s               -H "Cookie: $PORTAL_COOKIE=${1:-}" "$BASE$2"; }
portal_code(){ curl -s -o /dev/null -w '%{http_code}' \
                                     -H "Cookie: $PORTAL_COOKIE=${1:-}" "$BASE$2"; }
portal_post(){ curl -s -X POST -H 'Content-Type: application/json' \
                                     -H "Cookie: $PORTAL_COOKIE=${1:-}" \
                   --data "${3:-{\}}" "$BASE$2"; }
portal_post_code(){ curl -s -o /dev/null -w '%{http_code}' -X POST \
                   -H 'Content-Type: application/json' \
                   -H "Cookie: $PORTAL_COOKIE=${1:-}" \
                   --data "${3:-{\}}" "$BASE$2"; }

# portal_upload <psid> <path> <file> -> body   (multipart, for payment proofs)
portal_upload(){ curl -s -X POST -H "Cookie: $PORTAL_COOKIE=${1:-}" \
                     -F "file=@$3" "$BASE$2"; }

export PYTHONIOENCODING=utf-8

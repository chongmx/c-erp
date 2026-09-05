#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# FUNCTIONAL JOURNEY 10 — THE ACCOUNT LIFECYCLE, ADMIN-ONLY.
#
#   an admin creates the account -> it works -> the admin issues a reset link ->
#   the user completes it -> the new password takes over
#
# The policy this journey pins down (docs/111):
#
#   * There is exactly ONE way to get an account: an administrator creates it.
#     Self-registration (/web/signup) is closed, unconditionally — not gated by
#     a config flag that a stray row could flip back on.
#
#   * There is NO self-service password reset. A user cannot ask the server for
#     a reset token for their own login. The ONLY reset is one an administrator
#     mints deliberately (res.users.action_generate_reset_link) and hands over
#     out of band; the /web/reset_password route does nothing but COMPLETE such
#     a reset with a token the admin already generated.
#
# The load-bearing assertions are the two closed doors (signup, self-service
# reset) and the one that must stay open exactly as far as intended: an
# admin-issued token completes a reset, is single-use, is time-boxed, and only
# an admin can mint one.
#
# Everything is prefixed AJ / aj_ and removed on the way out, on failure too.
# =============================================================
auth_or_die

B="$BASE"
NEW='aj_newuser@acct.test'
PW='First-Pass-1'
PW2='Second-Pass-2'

# The signup/reset routes are plain JSON, not JSON-RPC. body → the body,
# code → the HTTP status alone.
su_body(){ curl -s               -X POST "$B$1" -H 'Content-Type: application/json' --data "$2"; }
su_code(){ curl -s -o /dev/null -w '%{http_code}' -X POST "$B$1" -H 'Content-Type: application/json' --data "$2"; }
# jval — pull one key out of an RPC result object, no JSON parser optional.
jval(){ python3 -c 'import sys,json
try: print(json.loads(sys.stdin.read()).get("result",{}).get(sys.argv[1],""))
except Exception: print("")' "$1"; }

cleanup(){
    pg "DELETE FROM res_users   WHERE login LIKE 'aj_%'" >/dev/null
    pg "DELETE FROM res_partner WHERE email LIKE 'aj_%' OR name LIKE 'AJ %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

# ------------------------------------------------------------------
sec "1. self-registration is closed"
# ------------------------------------------------------------------
# /web/signup used to create a full internal user for anyone who could reach
# it. It now refuses every request, and — the point of the redesign — it does
# so unconditionally, so no leftover config row can quietly re-open it.
t_eq "403" "$(su_code /web/signup "{\"login\":\"$NEW\",\"password\":\"$PW\"}")" "a signup attempt is refused (403)"
t_eq "0" "$(pg "SELECT count(*) FROM res_users WHERE login='$NEW'")" "and creates no account"
# Even if somebody re-adds the old enable flag, the door stays shut.
pg "INSERT INTO ir_config_parameter (key,value) VALUES ('auth_signup.allow','True')
      ON CONFLICT (key) DO UPDATE SET value='True'" >/dev/null
t_eq "403" "$(su_code /web/signup "{\"login\":\"$NEW\",\"password\":\"$PW\"}")" "still refused with the legacy flag set to True"
pg "DELETE FROM ir_config_parameter WHERE key='auth_signup.allow'" >/dev/null

# ------------------------------------------------------------------
sec "2. the one way in — an administrator creates the account"
# ------------------------------------------------------------------
# res.users.create is admin-gated; this is the sole path to a new login.
PART=$(call res.partner create "[{\"name\":\"AJ New User\",\"email\":\"$NEW\"}]" | rid)
t_nonempty "$PART" "a contact is created for the user"
USERID=$(call res.users create "[{\"login\":\"$NEW\",\"password\":\"$PW\",\"partner_id\":$PART,\"active\":true}]" | rid)
t_nonempty "$USERID" "the admin creates the user"
[ -z "$USERID" ] && { verdict; exit 1; }
t_eq "1" "$(pg "SELECT count(*) FROM res_users WHERE login='$NEW'")" "the account exists"
t_eq "0" "$(pg "SELECT (password='$PW')::int FROM res_users WHERE id=$USERID")" "its password is stored hashed, not in clear"

# A non-admin cannot create users — otherwise "admin-only" is a label, not a
# control. Make a plain user and have them try.
call res.users create "[{\"login\":\"aj_plain@acct.test\",\"password\":\"$PW\",\"partner_id\":$PART}]" >/dev/null
PLAIN_SID=$(login 'aj_plain@acct.test' "$PW")
if [ -n "$PLAIN_SID" ]; then
    RESP=$(call_as "$PLAIN_SID" res.users create "[{\"login\":\"aj_sneaky@acct.test\",\"password\":\"$PW\"}]")
    has_error "$RESP" && ok "a non-admin cannot create a user" || no "a non-admin created a user"
    t_eq "0" "$(pg "SELECT count(*) FROM res_users WHERE login='aj_sneaky@acct.test'")" "and none was written"
else
    no "could not sign in as the plain user to test the negative case"
fi

# ------------------------------------------------------------------
sec "3. the account actually works"
# ------------------------------------------------------------------
# A row in res_users is not the same as a usable login. The proof is signing in
# through the ordinary front door, which validates the password against the
# stored hash.
t_nonempty "$(login "$NEW" "$PW")" "the new user can sign in with the password the admin set"
t_eq "" "$(login "$NEW" 'not-the-password')" "the wrong password is refused"

# ------------------------------------------------------------------
sec "4. self-service password reset is closed"
# ------------------------------------------------------------------
# A user submitting their own login must NOT be handed a token. This is the
# automated path the redesign removed; it is the whole point of the change.
REQ=$(su_body /web/reset_password "{\"login\":\"$NEW\"}")
t_eq "403" "$(su_code /web/reset_password "{\"login\":\"$NEW\"}")" "asking for a reset is refused (403)"
t_lacks "$REQ" '"token"' "and no token is issued"
t_eq "1" "$(pg "SELECT (signup_token IS NULL)::int FROM res_partner WHERE id=$PART")" "nothing was stored against the account"

# ------------------------------------------------------------------
sec "5. the admin issues a reset link"
# ------------------------------------------------------------------
# The only way a token comes into existence: an administrator mints one.
GEN=$(call res.users action_generate_reset_link "[[$USERID]]")
has_error "$GEN" && no "generating a reset link failed: $(echo "$GEN" | head -c 200)"
TOK=$(printf '%s' "$GEN" | jval token)
URL=$(printf '%s' "$GEN" | jval reset_url)
t_nonempty "$TOK" "a token is minted"
t_contains "$URL" "reset_token=$TOK" "the returned link carries the token"
t_contains "$URL" 'reset_login=' "and the login the user opens it with"
t_eq "1" "$(pg "SELECT (signup_token='$TOK')::int FROM res_partner WHERE id=$PART")" "the token is stored against the account"
t_eq "1" "$(pg "SELECT (signup_expiration > now())::int FROM res_partner WHERE id=$PART")" "with an expiry in the future"

# A non-admin cannot mint one.
if [ -n "$PLAIN_SID" ]; then
    SNEAK=$(call_as "$PLAIN_SID" res.users action_generate_reset_link "[[$USERID]]")
    has_error "$SNEAK" && ok "a non-admin cannot issue a reset link" || no "a non-admin issued a reset link"
fi

# ------------------------------------------------------------------
sec "6. a wrong token changes nothing"
# ------------------------------------------------------------------
BADR=$(su_code /web/reset_password "{\"login\":\"$NEW\",\"token\":\"0000deadbeef0000\",\"password\":\"$PW2\"}")
t_ne "200" "$BADR" "completing with the wrong token is refused"
t_nonempty "$(login "$NEW" "$PW")" "the original password still works after a bad token"

# A too-short new password is refused even WITH the right token, and — checked
# before the token is spent — does not burn it.
SHORT=$(su_code /web/reset_password "{\"login\":\"$NEW\",\"token\":\"$TOK\",\"password\":\"short\"}")
t_ne "200" "$SHORT" "the right token with a too-short password is refused"
t_eq "1" "$(pg "SELECT (signup_token='$TOK')::int FROM res_partner WHERE id=$PART")" "the token was not consumed by the rejected attempt"

# ------------------------------------------------------------------
sec "7. the admin-issued link resets the password"
# ------------------------------------------------------------------
OK=$(su_code /web/reset_password "{\"login\":\"$NEW\",\"token\":\"$TOK\",\"password\":\"$PW2\"}")
t_eq "200" "$OK" "the reset completes with the admin's token"
t_eq "" "$(login "$NEW" "$PW")"     "the OLD password no longer signs in"
t_nonempty "$(login "$NEW" "$PW2")" "the NEW password does"

# ------------------------------------------------------------------
sec "8. the link is single-use"
# ------------------------------------------------------------------
# Anyone who saw the link once must not be able to reset again with it.
REPLAY=$(su_code /web/reset_password "{\"login\":\"$NEW\",\"token\":\"$TOK\",\"password\":\"Third-Pass-3\"}")
t_ne "200" "$REPLAY" "the same token cannot be used a second time"
t_eq "1" "$(pg "SELECT (signup_token IS NULL)::int FROM res_partner WHERE id=$PART")" "the token was cleared once spent"
t_nonempty "$(login "$NEW" "$PW2")" "the password from step 7 still stands"

# ------------------------------------------------------------------
sec "9. an expired link is worthless"
# ------------------------------------------------------------------
GEN2=$(call res.users action_generate_reset_link "[[$USERID]]")
FTOK=$(printf '%s' "$GEN2" | jval token)
t_nonempty "$FTOK" "the admin issues a fresh link"
pg "UPDATE res_partner SET signup_expiration = now() - INTERVAL '1 hour' WHERE id=$PART" >/dev/null
t_ne "200" "$(su_code /web/reset_password "{\"login\":\"$NEW\",\"token\":\"$FTOK\",\"password\":\"Expired-Pass-9\"}")" \
     "an expired token is refused"
t_nonempty "$(login "$NEW" "$PW2")" "the password is unchanged by an expired-token attempt"
pg "UPDATE res_partner SET signup_token=NULL, signup_expiration=NULL WHERE id=$PART" >/dev/null

# ------------------------------------------------------------------
sec "10. failures do not leak, and do not crash"
# ------------------------------------------------------------------
# Completing a reset for a login that does not exist must not carry SQL, a
# stack, or the internal exception text (SEC-28).
UNK=$(su_body /web/reset_password '{"login":"aj_does_not_exist@acct.test","token":"abc","password":"whatever-8"}')
t_lacks "$UNK" 'SELECT'      "the error body carries no SQL"
t_lacks "$UNK" 'pqxx'        "nor any driver internals"
t_lacks "$UNK" 'res_partner' "nor a table name"
# Malformed JSON is a clean error, not a crash — the server is still up after.
su_code /web/reset_password 'this is not json' >/dev/null
t_eq "200" "$(http_code /healthz)" "the server survives a malformed reset body"

# ------------------------------------------------------------------
sec "11. the reset LINK, driven through a real browser"
# ------------------------------------------------------------------
# The API above proved the completion route works; it cannot prove the panel a
# user actually opens does. The login page reads the token from the URL and
# renders a "set a new password" form — an OWL template parsed in the client,
# so a mistake in it is silent server-side. This drives the whole loop in
# Chrome: admin mints a link → open it anonymously → the panel renders → set a
# password → success → the new password signs in. Skips cleanly without Chrome.
DRIVE="$R/tests/functional/account/accounting/drive.mjs"
if command -v node >/dev/null 2>&1 && [ -f "$DRIVE" ]; then
    REP=$(RESET_LOGIN="$NEW" SHOT=/tmp/account-reset.png node "$DRIVE" 2>/tmp/aj_drive.err | tail -1)
    SKIP=$(printf '%s' "$REP" | python3 -c 'import sys,json;print(json.loads(sys.stdin.read() or "{}").get("skipped",""))' 2>/dev/null)
    if [ -n "$SKIP" ]; then
        echo "    SKIP  browser reset check: $SKIP"
    else
        get(){ printf '%s' "$REP" | python3 -c "import sys,json;d=json.loads(sys.stdin.read() or '{}');print(d.get('steps',{}).get('$1',''))" 2>/dev/null; }
        t_eq "True" "$(get panelRendered)"    "the reset panel renders from the link (no OWL error)"
        t_eq "True" "$(get successShown)"     "submitting shows the success state"
        t_eq "True" "$(get newPasswordWorks)" "the browser-set password signs in"
        ERRS=$(printf '%s' "$REP" | python3 -c 'import sys,json;print(len(json.loads(sys.stdin.read() or "{}").get("errors",[])))' 2>/dev/null)
        t_eq "0" "$ERRS" "no console/page errors during the reset flow"
        [ "$ERRS" != "0" ] && echo "    $REP"
    fi
else
    echo "    SKIP  browser reset check: node or drive.mjs unavailable"
fi

verdict

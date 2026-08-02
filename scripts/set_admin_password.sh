#!/bin/bash
# =============================================================
# Set a user's password directly in the database.
#
# Writes the same hash format AuthService::hashPassword() produces
# (modules/auth/AuthService.hpp):
#
#     $pbkdf2-sha512$<rounds>$<base64-salt>$<base64-hash>
#
#     rounds = 600000        (AuthService.hpp default)
#     salt   = 16 random bytes
#     hash   = 64 bytes, PBKDF2-HMAC-SHA512
#     base64 = STANDARD alphabet WITH '=' padding
#
# The base64 detail matters. The header comment in AuthService.hpp calls
# this "passlib format", but base64Encode_() is an OpenSSL BIO chain using
# the standard alphabet, NOT passlib's adapted alphabet. A hash produced by
# python-passlib or real Odoo will NOT verify against this app, and vice
# versa. Total length is always 135 for a 600000-round hash.
#
# Why the database and not the RPC API:
#   - works when you do NOT know the current password (change_password
#     requires it), which is the case you actually need a script for
#   - works when the app is stopped
#   - no session juggling; call_kw takes the session from
#     kwargs.context.session_id, not the Cookie header (docs/042 S-48)
#
# Usage:
#   ./scripts/set_admin_password.sh                      # prompt for password
#   ./scripts/set_admin_password.sh --generate           # generate a strong one
#   ./scripts/set_admin_password.sh --login alice        # a different user
#   ./scripts/set_admin_password.sh --selftest           # verify, change nothing
#
# Run from the project root (it reads config/system.cfg).
# =============================================================
set -euo pipefail

LOGIN=admin
MODE=prompt
NEWPW=

while [ $# -gt 0 ]; do
    case "$1" in
        --login)    LOGIN=${2:?--login needs a value}; shift 2 ;;
        --generate) MODE=generate; shift ;;
        --password) MODE=given; NEWPW=${2:?--password needs a value}; shift 2 ;;
        --selftest) MODE=selftest; shift ;;
        -h|--help)  sed -n '2,36p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

# ---- locate the project root -------------------------------------------
here=$(cd "$(dirname "$0")/.." && pwd)
CFG="$here/config/system.cfg"
[ -f "$CFG" ] || { echo "ERROR: cannot find $CFG — run from the project root." >&2; exit 1; }

cfg() { sed -nE "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*(.*[^[:space:]])[[:space:]]*$/\1/p" "$CFG" | head -1; }

DB_HOST=$(cfg db_host); DB_PORT=$(cfg db_port); DB_NAME=$(cfg db_name)
DB_USER=$(cfg db_user); DB_PASS=$(cfg db_password)
: "${DB_HOST:=localhost}" ; : "${DB_PORT:=5432}"
[ -n "$DB_NAME" ] && [ -n "$DB_USER" ] || { echo "ERROR: could not read db settings from $CFG" >&2; exit 1; }

command -v python3 >/dev/null || { echo "ERROR: python3 is required." >&2; exit 1; }
command -v psql    >/dev/null || { echo "ERROR: psql is required." >&2; exit 1; }

psql_q() { PGPASSWORD="$DB_PASS" psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" -tAc "$1"; }

echo "database : $DB_USER@$DB_HOST:$DB_PORT/$DB_NAME"
echo "user     : $LOGIN"

# ---- the user must exist -----------------------------------------------
uid=$(psql_q "SELECT id FROM res_users WHERE login = '$(printf '%s' "$LOGIN" | sed "s/'/''/g")'" || true)
[ -n "$uid" ] || { echo "ERROR: no res_users row with login '$LOGIN'." >&2; exit 1; }
echo "uid      : $uid"

# ---- selftest: prove our derivation matches the stored hash ------------
if [ "$MODE" = selftest ]; then
    echo
    echo "Self-test: re-deriving the STORED hash for '$LOGIN' using a candidate"
    echo "password, to confirm this script's format matches the C++ exactly."
    printf 'candidate password: '
    read -rs probe || { echo; echo "ERROR: no input on stdin (run this interactively)." >&2; exit 1; }
    echo
    stored=$(psql_q "SELECT password FROM res_users WHERE id = $uid")
    python3 - "$probe" "$stored" <<'PY'
import base64, hashlib, hmac, sys
probe, stored = sys.argv[1], sys.argv[2]
try:
    _, scheme, rounds, salt_b64, hash_b64 = stored.split('$')
except ValueError:
    print("stored hash is not in the expected 4-field format"); sys.exit(1)
print("  scheme      :", scheme)
print("  rounds      :", rounds)
print("  stored len  :", len(stored))
salt, expected = base64.b64decode(salt_b64), base64.b64decode(hash_b64)
print("  salt bytes  :", len(salt), " hash bytes:", len(expected))
derived = hashlib.pbkdf2_hmac('sha512', probe.encode(), salt, int(rounds), dklen=len(expected))
print("  MATCH" if hmac.compare_digest(derived, expected)
      else "  NO MATCH (wrong candidate password, or format mismatch)")
PY
    exit 0
fi

# ---- obtain the new password -------------------------------------------
case "$MODE" in
  generate)
    # Excludes '$'. AuthViewModel.hpp:318 stores a '$'-leading password
    # VERBATIM as if it were already a hash, which locks the account out.
    # This script hashes correctly either way, but a password you can also
    # safely retype into the UI is the more useful thing to hand you.
    NEWPW=$(python3 -c "
import secrets, string
a = string.ascii_letters + string.digits + '!#%&*+-=?@^_'
while True:
    p = ''.join(secrets.choice(a) for _ in range(24))
    if p[0].isalnum() and any(c.isdigit() for c in p) \
       and any(c.isupper() for c in p) and any(c.islower() for c in p):
        print(p); break
")
    ;;
  prompt)
    printf 'new password (hidden): '
    read -rs NEWPW || { echo; echo "ERROR: no input on stdin — use --generate or --password for non-interactive use." >&2; exit 1; }
    echo
    printf 'confirm             : '
    read -rs confirm || { echo; echo "ERROR: no input on stdin." >&2; exit 1; }
    echo
    [ "$NEWPW" = "$confirm" ] || { echo "ERROR: passwords do not match." >&2; exit 1; }
    ;;
esac

# Minimum length for this script. Note this is INTENTIONALLY looser than the
# app: change_password (AuthViewModel.hpp:487) rejects anything under 8, so a
# password set here with 4-7 characters logs in fine, but the user cannot
# later rotate it through the UI without choosing a longer one.
[ ${#NEWPW} -ge 4 ] || { echo "ERROR: password must be at least 4 characters." >&2; exit 1; }
case "$NEWPW" in
  '$'*) echo "WARNING: password starts with '\$'. This script stores it correctly," >&2
        echo "         but do NOT set the same value via the UI — AuthViewModel.hpp:318" >&2
        echo "         would store it verbatim and lock the account out." >&2 ;;
esac

# ---- derive and write ---------------------------------------------------
HASH=$(python3 - "$NEWPW" <<'PY'
import base64, hashlib, os, sys
ROUNDS = 600000                      # AuthService::hashPassword default
salt = os.urandom(16)                # 16 bytes, as in the C++
dk = hashlib.pbkdf2_hmac('sha512', sys.argv[1].encode(), salt, ROUNDS, dklen=64)
# Standard base64 WITH padding — matches OpenSSL BIO_f_base64 + NO_NL.
print("$pbkdf2-sha512$%d$%s$%s" % (
    ROUNDS,
    base64.b64encode(salt).decode(),
    base64.b64encode(dk).decode()))
PY
)

case "$HASH" in
  '$pbkdf2-sha512$600000$'*) ;;
  *) echo "ERROR: derived hash has an unexpected shape, refusing to write." >&2; exit 1 ;;
esac

psql_q "UPDATE res_users SET password = '$HASH', write_date = now() WHERE id = $uid" >/dev/null
echo "updated  : res_users.id=$uid"

# ---- verify what actually landed in the database -----------------------
stored=$(psql_q "SELECT password FROM res_users WHERE id = $uid")
python3 - "$NEWPW" "$stored" <<'PY'
import base64, hashlib, hmac, sys
pw, stored = sys.argv[1], sys.argv[2]
_, _, rounds, salt_b64, hash_b64 = stored.split('$')
salt, expected = base64.b64decode(salt_b64), base64.b64decode(hash_b64)
derived = hashlib.pbkdf2_hmac('sha512', pw.encode(), salt, int(rounds), dklen=len(expected))
if hmac.compare_digest(derived, expected):
    print("verified : the new password re-derives the stored hash (len=%d)" % len(stored))
else:
    print("FAILED   : stored hash does not verify — do not rely on this change"); sys.exit(1)
PY

if [ "$MODE" = generate ]; then
    echo
    echo "=================================================="
    echo "  NEW PASSWORD for '$LOGIN':  $NEWPW"
    echo "  Save it now — it is not stored anywhere else."
    echo "=================================================="
fi

echo
echo "Sessions are unaffected by this change; existing logins stay valid"
echo "until they expire (session_ttl_minutes in config/system.cfg)."

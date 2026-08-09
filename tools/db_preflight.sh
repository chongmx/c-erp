#!/usr/bin/env bash
#
# tools/db_preflight.sh — pre-start database health check (read-only).
#
# Run this BEFORE starting ./build/c-erp on a server. It answers, in order:
#   1. Can we reach PostgreSQL with the configured credentials?
#   2. Does the target database exist?
#   3. Is it empty (a fresh box) or already provisioned?
#   4. Are the critical infrastructure tables present?
#   5. Is the DB schema in step with the code — i.e. are there migrations the
#      running code declares that this database has NOT applied yet?
#
# It NEVER writes to the database. It only reads catalog/schema_migrations, so
# it is safe to run against production at any time.
#
# Connection parameters come from config/system.cfg ([options] section) and can
# be overridden by the same env vars scripts/setup_db.sh uses:
#   DB_HOST DB_PORT DB_NAME DB_USER DB_PASSWORD
#
# Exit codes:
#   0  ready to start (provisioned & up to date, OR empty and ready to provision)
#   1  cannot connect / database missing / auth failure  (fix before starting)
#   2  connected, but the code declares migrations this DB has not applied
#      (informational — a normal server start will apply them)
#
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CFG="${CFG:-$REPO/config/system.cfg}"

c_red=$'\033[31m'; c_grn=$'\033[32m'; c_yel=$'\033[33m'; c_dim=$'\033[2m'; c_rst=$'\033[0m'
ok()   { printf '  %s✓%s %s\n' "$c_grn" "$c_rst" "$*"; }
warn() { printf '  %s!%s %s\n' "$c_yel" "$c_rst" "$*"; }
bad()  { printf '  %s✗%s %s\n' "$c_red" "$c_rst" "$*"; }
hdr()  { printf '\n%s\n' "$*"; }

# ── read a key from the [options] section of the INI config ──────────────────
cfg_get() {
    [ -f "$CFG" ] || return 0
    grep -E "^[[:space:]]*$1[[:space:]]*=" "$CFG" | head -1 \
        | sed -E "s/^[^=]*=[[:space:]]*//; s/[[:space:]]*(;.*)?$//"
}

DB_HOST="${DB_HOST:-$(cfg_get db_host)}";     DB_HOST="${DB_HOST:-localhost}"
DB_PORT="${DB_PORT:-$(cfg_get db_port)}";     DB_PORT="${DB_PORT:-5432}"
DB_NAME="${DB_NAME:-$(cfg_get db_name)}";     DB_NAME="${DB_NAME:-odoo}"
DB_USER="${DB_USER:-$(cfg_get db_user)}";     DB_USER="${DB_USER:-odoo}"
DB_PASSWORD="${DB_PASSWORD:-$(cfg_get db_password)}"

echo "c-erp database preflight"
echo "${c_dim}config: $CFG${c_rst}"
echo "${c_dim}target: postgresql://$DB_USER@$DB_HOST:$DB_PORT/$DB_NAME${c_rst}"

export PGPASSWORD="$DB_PASSWORD"
Q() { psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" -tAqc "$1" 2>/tmp/.pf_err; }

# ── 1. connectivity / database existence ─────────────────────────────────────
hdr "1. Connection"
if ! Q "SELECT 1" >/dev/null; then
    err="$(cat /tmp/.pf_err 2>/dev/null)"
    if   echo "$err" | grep -qi "does not exist"; then
        bad "database \"$DB_NAME\" does not exist"
        echo "     → create it first:  ./scripts/setup_db.sh"
    elif echo "$err" | grep -qiE "password|authentication"; then
        bad "authentication failed for user \"$DB_USER\""
        echo "     → check db_user / db_password in $CFG"
    elif echo "$err" | grep -qiE "could not connect|refused|no such host|timeout"; then
        bad "cannot reach PostgreSQL at $DB_HOST:$DB_PORT"
        echo "     → is the server running / is db_host correct?"
    else
        bad "connection failed"
        echo "     ${c_dim}${err}${c_rst}"
    fi
    exit 1
fi
ok "connected to \"$DB_NAME\" as \"$DB_USER\""

# ── 2. provisioned or empty? ─────────────────────────────────────────────────
hdr "2. Schema state"
TABLES="$(Q "SELECT count(*) FROM pg_tables WHERE schemaname = current_schema()")"
HAS_MIG="$(Q "SELECT to_regclass('public.schema_migrations') IS NOT NULL")"

if [ "${TABLES:-0}" = "0" ]; then
    warn "database is EMPTY (0 tables)"
    echo "     → this is fine: start ./build/c-erp once and it provisions the"
    echo "       full schema automatically (ensureSchema + migrations, no demo data)."
    hdr "Verdict"
    ok "empty database, ready to be provisioned on first start"
    exit 0
fi
ok "$TABLES tables present"

if [ "$HAS_MIG" != "t" ]; then
    bad "schema_migrations table is missing but other tables exist"
    echo "     → partial/foreign schema. Start the server to let it reconcile,"
    echo "       or restore from a known-good backup."
    exit 1
fi

# ── 3. critical infrastructure tables ────────────────────────────────────────
hdr "3. Critical tables"
crit_missing=0
for t in schema_migrations res_company res_users ir_sequence ir_cron \
         account_move account_move_line stock_move ir_ui_menu; do
    if [ "$(Q "SELECT to_regclass('public.$t') IS NOT NULL")" = "t" ]; then
        ok "$t"
    else
        bad "$t  (MISSING)"
        crit_missing=$((crit_missing + 1))
    fi
done

# ── 4. migrations: code vs database ──────────────────────────────────────────
hdr "4. Migrations (code vs database)"
# Every version the running code declares via registerMigration({<n>, ...}).
EXPECTED="$(grep -rhoE "registerMigration\(\{[0-9]+" --include=*.cpp \
              "$REPO/core" "$REPO/modules" 2>/dev/null \
            | grep -oE "[0-9]+" | sort -n -u)"
APPLIED="$(Q "SELECT version FROM schema_migrations ORDER BY version")"

exp_n=$(printf '%s\n' "$EXPECTED" | grep -c . || true)
app_n=$(printf '%s\n' "$APPLIED"  | grep -c . || true)
# Versions the code knows about that the DB has NOT applied. awk set-difference
# (not comm): version numbers sort numerically, not lexically, and comm demands
# a matching collation — feeding it numeric-sorted input prints spurious
# "not in sorted order" warnings. Display numerically sorted.
PENDING="$(awk 'NR==FNR { seen[$1]; next } !($1 in seen)' \
             <(printf '%s\n' "$APPLIED") <(printf '%s\n' "$EXPECTED") | sort -n)"
pend_n=$(printf '%s\n' "$PENDING" | grep -c . || true)

echo "     code declares $exp_n migrations; database has applied $app_n"
if [ "$pend_n" -gt 0 ]; then
    warn "$pend_n migration(s) declared in code but NOT yet applied:"
    printf '%s\n' "$PENDING" | sed 's/^/       v/'
    echo "     → a normal server start will apply these automatically."
fi

# ── verdict ──────────────────────────────────────────────────────────────────
hdr "Verdict"
if [ "$crit_missing" -gt 0 ]; then
    bad "$crit_missing critical table(s) missing — start the server to provision,"
    echo "     then re-run this check. If it persists, inspect the boot log."
    exit 1
fi
if [ "$pend_n" -gt 0 ]; then
    warn "provisioned, but $pend_n migration(s) pending — start the server to apply them"
    exit 2
fi
ok "database is provisioned and up to date — safe to start ./build/c-erp"
exit 0

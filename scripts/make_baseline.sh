#!/usr/bin/env bash
# =============================================================
# make_baseline.sh — build the canonical clean-database snapshot.
#
# The baseline is what "reset" and "a clean database" mean from now on. It is
# NOT a dump of whatever the working database happens to contain: it is built
# from an empty PostgreSQL database that the application provisions itself, so
# it holds exactly:
#
#   * the schema every module creates on first boot,
#   * the reference data they seed — chart of accounts, journals, units,
#     footprints, task stages, menus, help articles,
#   * one company and the admin user.
#
# and nothing else. No invoices, no orders, no demo parts, no test debris.
#
# Building it on a SCRATCH database on a SPARE PORT is the point: the working
# database is never touched, and the result cannot inherit anything that was
# lying around in it.
#
#   ./scripts/make_baseline.sh              # build db/snapshots/baseline.dump
#   ./scripts/make_baseline.sh out.dump     # build somewhere else
# =============================================================
set -uo pipefail
cd "$(dirname "$0")/.."

OUT="${1:-db/snapshots/baseline.dump}"
SCRATCH_DB="erp_baseline_build"
SCRATCH_PORT=8079
CFG=/tmp/baseline_build.cfg

PGHOST="${PGHOST:-localhost}"; PGUSER="${PGUSER:-odoo}"
export PGPASSWORD="${PGPASSWORD:-odoo}"

mkdir -p "$(dirname "$OUT")"
[ -x ./build/c-erp ] || { echo "  build/c-erp not found — run: cmake --build ./build"; exit 1; }

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    sleep 1
    dropdb -h "$PGHOST" -U "$PGUSER" --if-exists "$SCRATCH_DB" 2>/dev/null
    rm -f "$CFG"
}
trap cleanup EXIT

echo "======================================================================"
echo " Building a clean baseline from an empty database"
echo "======================================================================"

echo "  1. creating scratch database $SCRATCH_DB"
dropdb -h "$PGHOST" -U "$PGUSER" --if-exists "$SCRATCH_DB" 2>/dev/null
if ! createdb -h "$PGHOST" -U "$PGUSER" "$SCRATCH_DB" 2>/tmp/createdb_err; then
    # The ideal baseline is provisioned by the application into an EMPTY
    # database, so it cannot inherit anything. That needs CREATEDB, which the
    # odoo role does not have here. Rather than refuse, derive the baseline from
    # the working database instead — see derive_baseline.sh for exactly what
    # that does and does not guarantee.
    echo "      cannot create a database as '$PGUSER':"
    sed 's/^/        /' /tmp/createdb_err
    echo "      falling back to deriving the baseline from the working database."
    echo "      To get the stronger, provisioned-from-empty baseline, grant the"
    echo "      right once as a superuser and re-run this script:"
    echo "          ALTER ROLE $PGUSER CREATEDB;"
    echo
    trap - EXIT
    exec bash scripts/derive_baseline.sh "$OUT"
fi

# A copy of the real config pointed at the scratch database and a spare port, so
# a server already running on 8069 keeps running and is never written to.
echo "  2. writing $CFG (db=$SCRATCH_DB port=$SCRATCH_PORT)"
sed -e "s/^db_name .*/db_name     = $SCRATCH_DB/" \
    -e "s/^http_port .*/http_port      = $SCRATCH_PORT/" \
    config/system.cfg > "$CFG"
grep -qE "^db_name +=" "$CFG" || { echo "  could not rewrite db_name in the config"; exit 1; }

echo "  3. booting the application so it provisions the schema"
setsid ./build/c-erp --config "$CFG" > /tmp/baseline_boot.log 2>&1 < /dev/null &
SRV_PID=$!
UP=0
for _ in $(seq 1 60); do
    curl -sf -o /dev/null --max-time 2 "http://127.0.0.1:$SCRATCH_PORT/healthz" && { UP=1; break; }
    sleep 1
done
if [ "$UP" -ne 1 ]; then
    echo "  the application did not start against the scratch database:"
    tail -25 /tmp/baseline_boot.log | sed 's/^/    /'
    exit 1
fi

# Give the seeders a moment to finish after healthz starts answering — the HTTP
# port opens before every module has run its initialize().
sleep 4
Q(){ psql -h "$PGHOST" -U "$PGUSER" -d "$SCRATCH_DB" -tAc "$1" 2>/dev/null | tr -d ' '; }

TABLES=$(Q "SELECT count(*) FROM pg_tables WHERE schemaname='public'")
USERS=$(Q  "SELECT count(*) FROM res_users")
COMPANIES=$(Q "SELECT count(*) FROM res_company")
MENUS=$(Q  "SELECT count(*) FROM ir_ui_menu")
HELP=$(Q   "SELECT count(*) FROM help_article")
ACCOUNTS=$(Q "SELECT count(*) FROM account_account")
UNITS=$(Q  "SELECT count(*) FROM part_unit")
MOVES=$(Q  "SELECT count(*) FROM account_move")
PRODUCTS=$(Q "SELECT count(*) FROM product_product")

echo "  4. what the fresh database contains"
printf '        %-22s %s\n' "tables"        "$TABLES"
printf '        %-22s %s\n' "users"         "$USERS"
printf '        %-22s %s\n' "companies"     "$COMPANIES"
printf '        %-22s %s\n' "menus"         "$MENUS"
printf '        %-22s %s\n' "help articles" "$HELP"
printf '        %-22s %s\n' "accounts"      "$ACCOUNTS"
printf '        %-22s %s\n' "part units"    "$UNITS"
printf '        %-22s %s\n' "journal entries" "$MOVES"
printf '        %-22s %s\n' "products"      "$PRODUCTS"

# A baseline is only useful if it is both COMPLETE (the app can start and
# someone can log in) and CLEAN (no transactions). Refuse to write one that
# fails either test rather than hand over a snapshot that quietly breaks later.
BAD=
[ "${TABLES:-0}"    -lt 50 ] && BAD="$BAD schema-incomplete($TABLES-tables)"
[ "${USERS:-0}"     -lt 1  ] && BAD="$BAD no-admin-user"
[ "${COMPANIES:-0}" -lt 1  ] && BAD="$BAD no-company"
[ "${MENUS:-0}"     -lt 20 ] && BAD="$BAD menus-not-seeded"
[ "${ACCOUNTS:-0}"  -lt 1  ] && BAD="$BAD no-chart-of-accounts"
[ "${MOVES:-0}"     -gt 0  ] && BAD="$BAD not-clean($MOVES-journal-entries)"
if [ -n "$BAD" ]; then
    echo "  REFUSING to write the baseline:$BAD"
    exit 1
fi

echo "  5. stopping the scratch server"
kill "$SRV_PID" 2>/dev/null; SRV_PID=""
sleep 2

echo "  6. dumping to $OUT"
pg_dump -h "$PGHOST" -U "$PGUSER" -d "$SCRATCH_DB" -Fc -f "$OUT" || { echo "  pg_dump failed"; exit 1; }
pg_restore -l "$OUT" > /dev/null 2>&1 || { echo "  the dump is not readable"; exit 1; }

echo "======================================================================"
echo "  baseline written: $OUT ($(du -h "$OUT" | cut -f1))"
echo "  load it with:  ./scripts/db_snapshot.sh restore $OUT"
echo "======================================================================"

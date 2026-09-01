#!/usr/bin/env bash
# =============================================================
# derive_baseline.sh — build the clean baseline from the working database.
#
# The stronger way to build a baseline is to let the application provision an
# EMPTY database (make_baseline.sh). That needs CREATEDB, which the odoo role
# does not have on this machine, so this is the fallback and make_baseline.sh
# hands over to it automatically.
#
# What it does:
#   1. snapshot the working database  (nothing here is destructive to it)
#   2. clear it with the same reset the Database Tools screen uses, at the
#      widest scope — transactions AND master data — which by construction
#      cannot touch configuration (it refuses rather than widen)
#   3. remove the demo and QA remnants the reset deliberately keeps
#   4. dump the result as the baseline
#   5. restore the snapshot, putting the working database back
#
# What it guarantees: no transactions, no products, no test debris, and every
# configuration table intact.
#
# What it does NOT guarantee: that the configuration is *pristine*. Anything
# already customised in the working database — an extra company, an added
# journal, a renamed menu — is inherited. Grant CREATEDB and use
# make_baseline.sh when that matters.
# =============================================================
set -uo pipefail
cd "$(dirname "$0")/.."

OUT="${1:-db/snapshots/baseline.dump}"
SAFETY="/tmp/derive_safety_$$.dump"
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
export PGPASSWORD="${PGPASSWORD:-odoo}"
Q(){ psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }

mkdir -p "$(dirname "$OUT")"

RESTORED=0
finish() {
    if [ "$RESTORED" -eq 0 ] && [ -s "$SAFETY" ]; then
        echo "  restoring the working database from the safety snapshot"
        bash scripts/db_snapshot.sh restore "$SAFETY" | sed 's/^/    /'
    fi
    rm -f "$SAFETY" /tmp/derive_cookie_$$
}
trap finish EXIT

echo "======================================================================"
echo " Deriving a clean baseline from the working database"
echo "======================================================================"

echo "  1. safety snapshot of the working database"
bash scripts/db_snapshot.sh take "$SAFETY" | sed 's/^/    /'
bash scripts/db_snapshot.sh verify "$SAFETY" >/dev/null 2>&1 \
  || { echo "  the safety snapshot is unusable — refusing to continue"; exit 1; }

curl -sf -o /dev/null --max-time 3 "$BASE/healthz" || {
    echo "  the server is not responding at $BASE — start it first"; exit 1; }

echo "  2. clearing transactions and master data via the reset endpoint"
CK=/tmp/derive_cookie_$$
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
  --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" >/dev/null
RESP=$(curl -s -b "$CK" -X POST "$BASE/web/dbtool" -H 'Content-Type: application/json' \
       --data '{"params":{"op":"reset","scope":"master","confirm":"RESET"}}')
echo "$RESP" | grep -q '"ok":true' || { echo "  reset failed: $(echo "$RESP" | head -c 300)"; exit 1; }
echo "    cleared $(echo "$RESP" | sed -n 's/.*"total_rows":\([0-9]*\).*/\1/p') row(s)"

echo "  3. removing demo and QA remnants the reset keeps"
# The reset deliberately leaves contacts and projects alone (clearing them would
# drag configuration out). Demo and QA rows are ours to remove by name.
Q "DELETE FROM project_project WHERE name LIKE 'DEMO %' OR name LIKE 'QA-%'" >/dev/null
Q "DELETE FROM res_partner WHERE name LIKE 'QA-%'" >/dev/null
Q "DELETE FROM part_lookup_result" >/dev/null
Q "DELETE FROM help_article WHERE slug LIKE 'qa-%'" >/dev/null

echo "  4. checking the result is clean AND complete"
TABLES=$(Q "SELECT count(*) FROM pg_tables WHERE schemaname='public'")
MOVES=$(Q  "SELECT count(*) FROM account_move")
PRODS=$(Q  "SELECT count(*) FROM product_product")
USERS=$(Q  "SELECT count(*) FROM res_users")
MENUS=$(Q  "SELECT count(*) FROM ir_ui_menu")
ACCTS=$(Q  "SELECT count(*) FROM account_account")
HELP=$(Q   "SELECT count(*) FROM help_article")
UNITS=$(Q  "SELECT count(*) FROM part_unit")
printf '        %-18s %s\n' tables "$TABLES" users "$USERS" menus "$MENUS" \
       accounts "$ACCTS" "help articles" "$HELP" "part units" "$UNITS" \
       "journal entries" "$MOVES" products "$PRODS"

BAD=
[ "${TABLES:-0}" -lt 50 ] && BAD="$BAD schema-incomplete"
[ "${USERS:-0}"  -lt 1  ] && BAD="$BAD no-admin-user"
[ "${MENUS:-0}"  -lt 20 ] && BAD="$BAD menus-missing"
[ "${ACCTS:-0}"  -lt 1  ] && BAD="$BAD no-chart-of-accounts"
[ "${HELP:-0}"   -lt 10 ] && BAD="$BAD help-missing"
[ "${MOVES:-0}"  -gt 0  ] && BAD="$BAD not-clean($MOVES-entries)"
[ "${PRODS:-0}"  -gt 0  ] && BAD="$BAD not-clean($PRODS-products)"
if [ -n "$BAD" ]; then
    echo "  REFUSING to write the baseline:$BAD"
    exit 1
fi

echo "  5. dumping the clean state to $OUT"
pg_dump -h localhost -U "$DBN" -d "$DBN" -Fc -f "$OUT" || { echo "  pg_dump failed"; exit 1; }
pg_restore -l "$OUT" > /dev/null 2>&1 || { echo "  the dump is not readable"; exit 1; }

echo "  6. restoring the working database"
bash scripts/db_snapshot.sh restore "$SAFETY" | sed 's/^/    /'
RESTORED=1

echo "======================================================================"
echo "  baseline written: $OUT ($(du -h "$OUT" | cut -f1))"
echo "  working database restored — nothing you had was lost"
echo "  load the baseline with:  ./scripts/db_snapshot.sh restore $OUT"
echo "======================================================================"

#!/bin/bash
# =============================================================
# tools/provision_tenant.sh <db_name> [subdomain] [email_domains_csv]
#
# Create a new tenant COMPANY database (docs/072), register it in
# config/tenants.json, and provision its schema + migrations.
#
# Requires a role that can CREATE DATABASE. By default it uses db_user from
# config/system.cfg; if that role lacks CREATEDB, point it at a superuser:
#
#     PGSU=postgres PGSUPASS=secret tools/provision_tenant.sh acme acme acme.com
#
# The new database is owned by db_user so the app (which connects as db_user)
# can create its tables.
# =============================================================
set -euo pipefail
cd "$(dirname "$0")/.."

DB="${1:?usage: provision_tenant.sh <db_name> [subdomain] [email_domains_csv]}"
SUB="${2:-}"
EMAILS="${3:-}"

# ERP_CONFIG lets you point at an alternate config (its sibling tenants.json is
# updated, and c-erp is run with --config). Defaults to the standard config.
CFG="${ERP_CONFIG:-config/system.cfg}"
CFGDIR="$(dirname "$CFG")"
TJSON="$CFGDIR/tenants.json"
val() { grep -E "^[[:space:]]*$1[[:space:]]*=" "$CFG" 2>/dev/null | head -1 | sed -E "s/^[^=]*=[[:space:]]*//" | tr -d "\r"; }
HOST=$(val db_host);     HOST=${HOST:-localhost}
PORT=$(val db_port);     PORT=${PORT:-5432}
DBUSER=$(val db_user);   DBUSER=${DBUSER:-odoo}
DBPASS=$(val db_password)
SU=${PGSU:-$DBUSER}
SUPASS=${PGSUPASS:-$DBPASS}
export PGCONNECT_TIMEOUT=8

echo "[provision] ensuring database '$DB' exists (connecting as '$SU') ..."
if PGPASSWORD="$SUPASS" psql -w -h "$HOST" -p "$PORT" -U "$SU" -d postgres -tAc \
     "SELECT 1 FROM pg_database WHERE datname='$DB'" 2>/dev/null | grep -q 1; then
    echo "[provision] '$DB' already exists — will (re)provision schema."
else
    if ! PGPASSWORD="$SUPASS" createdb -w -h "$HOST" -p "$PORT" -U "$SU" -O "$DBUSER" "$DB" 2>/tmp/provision_err; then
        echo "[provision] ERROR: could not create '$DB':"
        sed 's/^/    /' /tmp/provision_err
        echo "[provision] The role '$SU' likely lacks CREATEDB. Either:"
        echo "             GRANT it:   ALTER ROLE $DBUSER CREATEDB;   (run as a superuser)"
        echo "             or provide: PGSU=<superuser> PGSUPASS=<pw> tools/provision_tenant.sh $DB ..."
        exit 1
    fi
    echo "[provision] created '$DB' (owner '$DBUSER')."
fi

echo "[provision] registering '$DB' in $TJSON ..."
python3 - "$DB" "$SUB" "$EMAILS" "$TJSON" <<'PY'
import json, os, sys
db, sub, emails, path = sys.argv[1:5]
data = {"tenants": []}
if os.path.exists(path):
    try:
        data = json.load(open(path))
    except Exception:
        data = {"tenants": []}
if isinstance(data, list):
    data = {"tenants": data}
ts = [t for t in data.get("tenants", []) if t.get("name") != db]
entry = {"name": db, "active": True}
if sub:
    entry["subdomain"] = sub
if emails:
    entry["email_domains"] = [x.strip() for x in emails.split(",") if x.strip()]
ts.append(entry)
data["tenants"] = ts
json.dump(data, open(path, "w"), indent=2)
print("  registered: " + json.dumps(entry))
PY

echo "[provision] provisioning schema + migrations for all tenants ..."
./build/c-erp --config "$CFG" --provision

echo "[provision] DONE. Tenant '$DB' is ready (subdomain='${SUB:-none}', emails='${EMAILS:-none}')."

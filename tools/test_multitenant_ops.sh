#!/bin/bash
# Items 1 + 2: provision_tenant.sh end-to-end + per-tenant crons.
# Own throwaway PG (we are superuser, odoo has CREATEDB) so provision_tenant.sh
# and the cron scheduler run against real separate databases.
set -uo pipefail
cd ~/code/c-erp || exit 1
PGBIN=/usr/lib/postgresql/16/bin
PGDATA=/tmp/ops_pg; PGPORT=5435; APPPORT=8171
BASE="http://127.0.0.1:$APPPORT"; CFGDIR=/tmp/ops_test; CFG="$CFGDIR/system.cfg"
export ERP_CONFIG="$CFG"
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
q(){ "$PGBIN/psql" -h 127.0.0.1 -p $PGPORT -U odoo -d "$1" -tAc "$2" 2>/dev/null | tr -d ' ' | head -1; }

teardown(){
  echo "############ teardown ############"
  pkill -f "c-erp --config $CFGDIR" 2>/dev/null; sleep 1
  "$PGBIN/pg_ctl" -D "$PGDATA" -m fast stop >/dev/null 2>&1
  rm -rf "$PGDATA" "$CFGDIR" /tmp/ops_*.log
  echo "    cleaned up"
}
trap teardown EXIT

echo "############ throwaway PG (odoo = superuser, HAS createdb) ############"
rm -rf "$PGDATA"; mkdir -p "$PGDATA" "$CFGDIR"
"$PGBIN/initdb" -D "$PGDATA" -U odoo -A trust >/tmp/ops_initdb.log 2>&1 || { no initdb; exit 1; }
"$PGBIN/pg_ctl" -D "$PGDATA" -o "-p $PGPORT -k /tmp -c listen_addresses=127.0.0.1" -l /tmp/ops_pg.log -w start >/dev/null 2>&1 || { no pgstart; exit 1; }
for i in $(seq 1 20); do "$PGBIN/pg_isready" -h 127.0.0.1 -p $PGPORT -q && break; sleep 0.5; done
q postgres "ALTER ROLE odoo WITH PASSWORD 'odoo' SUPERUSER CREATEDB;" >/dev/null
"$PGBIN/createdb" -h 127.0.0.1 -p $PGPORT -U odoo ops_primary || { no "createdb primary"; exit 1; }
ok "throwaway PG up; ops_primary created"

cat > "$CFG" <<EOF
[options]
db_host = 127.0.0.1
db_port = $PGPORT
db_name = ops_primary
db_user = odoo
db_password = odoo
db_maxconn = 5
http_interface = 127.0.0.1
http_port = $APPPORT
workers = 2
http_doc_root = web/static
http_index = index.html
secure_cookies = False
log_level = warn
logfile =
EOF
./build/c-erp --config "$CFG" --provision >/tmp/ops_prov0.log 2>&1
grep -q "Provisioning + migration complete" /tmp/ops_prov0.log && ok "primary provisioned" || { no "primary provision failed"; tail -6 /tmp/ops_prov0.log; exit 1; }

echo "############ Item 2: provision_tenant.sh end-to-end ############"
# createdb needs the PG bin dir on PATH (provision_tenant.sh calls createdb/psql)
export PATH="$PGBIN:$PATH"
bash tools/provision_tenant.sh ops_tenant tenantx tenantx.com >/tmp/ops_prov.log 2>&1
RC=$?
[ $RC -eq 0 ] && ok "provision_tenant.sh exited 0" || { no "provision_tenant.sh failed (rc=$RC)"; tail -12 /tmp/ops_prov.log; }
q postgres "SELECT 1 FROM pg_database WHERE datname='ops_tenant'" | grep -q 1 && ok "provision_tenant.sh created the database" || no "ops_tenant db not created"
grep -q "ops_tenant" "$CFGDIR/tenants.json" 2>/dev/null && ok "tenant registered in tenants.json" || no "tenant not registered"
q ops_tenant "SELECT count(*) FROM res_users" | grep -qE "[1-9]" && ok "ops_tenant provisioned (res_users seeded)" || no "ops_tenant not provisioned"

echo "############ Item 1: per-tenant crons ############"
( setsid ./build/c-erp --config "$CFG" >/tmp/ops_server.log 2>&1 & )
for i in $(seq 1 25); do curl -sf -o /dev/null --max-time 2 "$BASE/healthz" && break; sleep 1; done
curl -sf -o /dev/null "$BASE/healthz" && ok "server up (both tenants)" || { no "server down"; tail -6 /tmp/ops_server.log; exit 1; }
# make the NON-DEFAULT tenant's cron due; if only the default tenant were ticked
# it would never fire. A registered handler (stock.reorder) marks last_run on success.
BEFORE=$(q ops_tenant "SELECT COALESCE(to_char(last_run,'YYYYMMDDHH24MISS'),'never') FROM ir_cron WHERE code='stock.reorder'")
q ops_tenant "UPDATE ir_cron SET active=TRUE, next_run = now() - interval '1 hour' WHERE code='stock.reorder'" >/dev/null
echo "    waiting up to 40s for a scheduler tick (tick=30s) ..."
FIRED=no
for i in $(seq 1 40); do
  AFTER=$(q ops_tenant "SELECT COALESCE(to_char(last_run,'YYYYMMDDHH24MISS'),'never') FROM ir_cron WHERE code='stock.reorder'")
  if [ "$AFTER" != "$BEFORE" ] && [ "$AFTER" != "never" ]; then FIRED=yes; break; fi
  sleep 1
done
[ "$FIRED" = "yes" ] && ok "cron in the NON-default tenant fired (last_run advanced: $BEFORE -> $AFTER)" || no "tenant cron did not fire (last_run still $BEFORE)"
# and prove it ran pinned to that tenant: next_run was rescheduled into the future
NR=$(q ops_tenant "SELECT CASE WHEN next_run > now() THEN 'future' ELSE 'past' END FROM ir_cron WHERE code='stock.reorder'")
[ "$NR" = "future" ] && ok "tenant cron rescheduled (next_run in the future)" || no "next_run not rescheduled ($NR)"

echo
if [ -n "$FAILED" ]; then echo '*** FAILURES ***'; else echo 'All checks passed.'; fi

#!/bin/bash
# Item 3: browser click-test of the multi-company login chooser + top-bar
# switcher (cross-tenant SSO), driven by headless Chrome (Puppeteer). Spins up
# its own throwaway PG + control plane + two companies, runs the UI flow, and
# tears everything down.
set -uo pipefail
cd ~/code/c-erp || exit 1
PGBIN=/usr/lib/postgresql/16/bin
PGDATA=/tmp/mcb_pg; PGPORT=5436; APPPORT=8172
CFGDIR=/tmp/mcb_test; FAILED=
BASE="http://127.0.0.1:$APPPORT"
q(){ "$PGBIN/psql" -h 127.0.0.1 -p $PGPORT -U odoo -d "$1" -tAc "$2" 2>/dev/null; }
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }

teardown(){
  echo "############ teardown ############"
  pkill -f "c-erp --config $CFGDIR" 2>/dev/null; sleep 1
  "$PGBIN/pg_ctl" -D "$PGDATA" -m fast stop >/dev/null 2>&1
  rm -rf "$PGDATA" "$CFGDIR" /tmp/mcb_*.log
  echo "    cleaned up"
}
trap teardown EXIT

echo "############ setup: throwaway PG + 2 companies + control plane ############"
rm -rf "$PGDATA"; mkdir -p "$PGDATA" "$CFGDIR"
"$PGBIN/initdb" -D "$PGDATA" -U odoo -A trust >/tmp/mcb_initdb.log 2>&1 || { no initdb; exit 1; }
"$PGBIN/pg_ctl" -D "$PGDATA" -o "-p $PGPORT -k /tmp -c listen_addresses=127.0.0.1" -l /tmp/mcb_pg.log -w start >/dev/null 2>&1 || { no pgstart; exit 1; }
for i in $(seq 1 20); do "$PGBIN/pg_isready" -h 127.0.0.1 -p $PGPORT -q && break; sleep 0.5; done
q postgres "ALTER ROLE odoo WITH PASSWORD 'odoo' SUPERUSER CREATEDB;" >/dev/null
for d in mc_primary mc_tenant_b mc_control; do "$PGBIN/createdb" -h 127.0.0.1 -p $PGPORT -U odoo "$d" || { no "createdb $d"; exit 1; }; done
cat > "$CFGDIR/system.cfg" <<EOF
[options]
db_host = 127.0.0.1
db_port = $PGPORT
db_name = mc_primary
db_user = odoo
db_password = odoo
db_maxconn = 5
control_db = mc_control
http_interface = 127.0.0.1
http_port = $APPPORT
workers = 2
http_doc_root = web/static
http_index = index.html
secure_cookies = False
log_level = warn
logfile =
EOF
cat > "$CFGDIR/tenants.json" <<EOF
{"tenants":[{"name":"mc_tenant_b","active":true,"subdomain":"tenantb"}]}
EOF
./build/c-erp --config "$CFGDIR/system.cfg" --provision >/tmp/mcb_prov.log 2>&1
grep -q "Provisioning + migration complete" /tmp/mcb_prov.log && ok "two companies + control plane provisioned" || { no "provision failed"; tail -6 /tmp/mcb_prov.log; exit 1; }
# distinct company names so the chooser/switcher labels differ
q mc_primary  "UPDATE res_company SET name='Company A' WHERE id=(SELECT MIN(id) FROM res_company)" >/dev/null
q mc_tenant_b "UPDATE res_company SET name='Company B' WHERE id=(SELECT MIN(id) FROM res_company)" >/dev/null
# one identity owns admin in both
q mc_control "INSERT INTO mc_membership(identity,tenant_db,local_login) VALUES
  ('chief@corp.example','mc_primary','admin'),('chief@corp.example','mc_tenant_b','admin')
  ON CONFLICT DO NOTHING;" >/dev/null
ok "named companies A/B; seeded identity chief@corp.example in both"

echo "############ start server ############"
( setsid ./build/c-erp --config "$CFGDIR/system.cfg" >/tmp/mcb_server.log 2>&1 & )
for i in $(seq 1 25); do curl -sf -o /dev/null --max-time 2 "$BASE/healthz" && break; sleep 1; done
curl -sf -o /dev/null "$BASE/healthz" && ok "server up on $APPPORT" || { no "server down"; tail -6 /tmp/mcb_server.log; exit 1; }

echo "############ browser: login chooser + switcher (Puppeteer) ############"
# The canonical UI script lives in the repo (tools/mt_ui.js); run it from
# ~/browsertest where puppeteer-core is installed (ESM import() ignores NODE_PATH).
cp tools/mt_ui.js "$HOME/browsertest/mt_ui.js"
( cd "$HOME/browsertest" && BASE="$BASE" node mt_ui.js ) 2>&1 | tee /tmp/mcb_ui.log
grep -q "All checks passed." /tmp/mcb_ui.log || FAILED=1

echo
if [ -n "$FAILED" ]; then echo '*** FAILURES ***'; else echo 'All checks passed.'; fi

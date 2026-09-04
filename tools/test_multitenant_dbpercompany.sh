#!/bin/bash
# Self-contained DB-per-company test: spins up its OWN throwaway PostgreSQL
# (where we are superuser), provisions two real tenant databases, and proves
# routing + isolation. Fully isolated from the system PG (5432) and the main
# server (8069). Tears everything down on exit.
set -uo pipefail
cd ~/code/c-erp || exit 1

PGBIN=/usr/lib/postgresql/16/bin
PGDATA=/tmp/mc_pg
PGPORT=5433
APPPORT=8169
BASE="http://127.0.0.1:$APPPORT"
CFGDIR=/tmp/mc_test
FAILED=

ok()  { echo "    PASS  $1"; }
no()  { echo "    FAIL  $1"; FAILED=1; }

teardown() {
  echo "############ teardown ############"
  pkill -f "c-erp --config $CFGDIR" 2>/dev/null
  sleep 1
  "$PGBIN/pg_ctl" -D "$PGDATA" -m fast stop >/dev/null 2>&1
  rm -rf "$PGDATA" "$CFGDIR" /tmp/mc_pg.log /tmp/mc_server.log
  echo "    cleaned up throwaway PG + config"
}
trap teardown EXIT

echo "############ throwaway PostgreSQL (port $PGPORT, we are superuser) ############"
rm -rf "$PGDATA"; mkdir -p "$PGDATA"
"$PGBIN/initdb" -D "$PGDATA" -U odoo -A trust >/tmp/mc_initdb.log 2>&1 || { no "initdb failed"; tail -5 /tmp/mc_initdb.log; exit 1; }
"$PGBIN/pg_ctl" -D "$PGDATA" -o "-p $PGPORT -k /tmp -c listen_addresses=127.0.0.1" -l /tmp/mc_pg.log -w start >/dev/null 2>&1 \
  || { no "pg_ctl start failed"; tail -8 /tmp/mc_pg.log; exit 1; }
for i in $(seq 1 20); do "$PGBIN/pg_isready" -h 127.0.0.1 -p $PGPORT -q && break; sleep 0.5; done
"$PGBIN/psql" -h 127.0.0.1 -p $PGPORT -U odoo -d postgres -qc "ALTER ROLE odoo WITH PASSWORD 'odoo' SUPERUSER CREATEDB;" >/dev/null 2>&1
"$PGBIN/createdb" -h 127.0.0.1 -p $PGPORT -U odoo mc_primary  || { no "createdb mc_primary"; exit 1; }
"$PGBIN/createdb" -h 127.0.0.1 -p $PGPORT -U odoo mc_tenant_b || { no "createdb mc_tenant_b"; exit 1; }
ok "throwaway PG up; created databases mc_primary + mc_tenant_b"

echo "############ test config (primary=mc_primary, tenant=mc_tenant_b) ############"
mkdir -p "$CFGDIR"
cat > "$CFGDIR/system.cfg" <<EOF
[options]
db_host = 127.0.0.1
db_port = $PGPORT
db_name = mc_primary
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
cat > "$CFGDIR/tenants.json" <<EOF
{"tenants":[{"name":"mc_tenant_b","active":true,"subdomain":"tenantb","email_domains":["tenantb.test"]}]}
EOF
ok "wrote test config + tenants.json"

echo "############ provision both tenants (--provision) ############"
./build/c-erp --config "$CFGDIR/system.cfg" --provision >/tmp/mc_provision.log 2>&1
if grep -q "Provisioning + migration complete" /tmp/mc_provision.log; then
  ok "provisioned mc_primary + mc_tenant_b"
  PA=$("$PGBIN/psql" -h 127.0.0.1 -p $PGPORT -U odoo -d mc_primary  -tAc "SELECT count(*) FROM res_users" 2>/dev/null | tr -d ' ')
  PB=$("$PGBIN/psql" -h 127.0.0.1 -p $PGPORT -U odoo -d mc_tenant_b -tAc "SELECT count(*) FROM res_users" 2>/dev/null | tr -d ' ')
  if [ "${PA:-0}" -ge 1 ] && [ "${PB:-0}" -ge 1 ]; then
    ok "boot loop provisioned BOTH tenant DBs (res_users seeded: A=$PA B=$PB)"
  else
    no "a tenant DB was not provisioned (A=${PA:-0} B=${PB:-0})"
  fi
else
  no "provisioning failed"; tail -10 /tmp/mc_provision.log; exit 1
fi

echo "############ start test server on $APPPORT ############"
( setsid ./build/c-erp --config "$CFGDIR/system.cfg" >/tmp/mc_server.log 2>&1 & )
for i in $(seq 1 25); do curl -sf -o /dev/null --max-time 2 "$BASE/healthz" && break; sleep 1; done
curl -sf -o /dev/null --max-time 3 "$BASE/healthz" && ok "test server healthy on $APPPORT" || { no "server did not start"; tail -10 /tmp/mc_server.log; exit 1; }

auth() {  # auth <db> <login> <pass> [hosthdr] -> sid
  cat > /tmp/mc_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$1","login":"$2","password":"$3"}}
EOF
  if [ -n "${4:-}" ]; then
    curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' -H "Host: $4" --data @/tmp/mc_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p'
  else
    curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' --data @/tmp/mc_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p'
  fi
}
callas() {  # callas <sid> <model> <method> <args>
  cat > /tmp/mc_call.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$2","method":"$3","args":$4,"kwargs":{"context":{"session_id":"$1"}}}}
EOF
  curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/mc_call.json
}
qA() { "$PGBIN/psql" -h 127.0.0.1 -p $PGPORT -U odoo -d mc_primary  -tAc "$1" | tr -d ' ' | head -1; }
qB() { "$PGBIN/psql" -h 127.0.0.1 -p $PGPORT -U odoo -d mc_tenant_b -tAc "$1" | tr -d ' ' | head -1; }

echo "############ routing + ISOLATION ############"
SID_A=$(auth mc_primary  admin admin)
SID_B=$(auth mc_tenant_b admin admin)
[ -n "$SID_A" ] && ok "auth into tenant A (mc_primary) via db param" || no "cannot auth tenant A"
[ -n "$SID_B" ] && ok "auth into tenant B (mc_tenant_b) via db param" || no "cannot auth tenant B"

callas "$SID_A" res.partner create '[{"name":"MT-A-ONLY"}]' >/dev/null
callas "$SID_B" res.partner create '[{"name":"MT-B-ONLY"}]' >/dev/null

RA=$(callas "$SID_A" res.partner search_read '[[["name","like","MT-"]]]')
RB=$(callas "$SID_B" res.partner search_read '[[["name","like","MT-"]]]')
echo "$RA" | grep -q 'MT-A-ONLY' && ok "A sees its own partner"                   || no "A missing own partner"
echo "$RA" | grep -q 'MT-B-ONLY' && no "LEAK: A sees B's partner"                  || ok "A does NOT see B's partner"
echo "$RB" | grep -q 'MT-B-ONLY' && ok "B sees its own partner"                   || no "B missing own partner"
echo "$RB" | grep -q 'MT-A-ONLY' && no "LEAK: B sees A's partner"                  || ok "B does NOT see A's partner"

CA=$(qA "SELECT count(*) FROM res_partner WHERE name='MT-B-ONLY'")
CB=$(qB "SELECT count(*) FROM res_partner WHERE name='MT-B-ONLY'")
[ "$CB" = "1" ] && ok "MT-B-ONLY physically in tenant B database" || no "MT-B not in B (count=$CB)"
[ "$CA" = "0" ] && ok "MT-B-ONLY absent from tenant A database"   || no "MT-B leaked into A (count=$CA)"

echo "############ subdomain routing (Host=tenantb.*, no db param) ############"
SID_H=$(auth "" admin admin "tenantb.local")
if [ -n "$SID_H" ]; then
  RH=$(callas "$SID_H" res.partner search_read '[[["name","like","MT-"]]]')
  echo "$RH" | grep -q 'MT-B-ONLY' && ok "Host subdomain 'tenantb' routed session to tenant B" || no "subdomain did not route to B"
  echo "$RH" | grep -q 'MT-A-ONLY' && no "subdomain session leaked A data"                       || ok "subdomain session isolated to B"
else
  no "subdomain-host auth failed"
fi

echo "############ email-domain routing (login @tenantb.test, no db, no Host) ############"
# A user that exists ONLY in tenant B. If '@tenantb.test' routes to B the login
# succeeds; if it wrongly routed to A the user would not exist and auth fails —
# so a successful login is itself proof the email domain routed to tenant B.
callas "$SID_B" res.users create '[{"login":"boss@tenantb.test","name":"B Boss","password":"Boss!pw123"}]' >/dev/null
SID_E=$(auth "" "boss@tenantb.test" "Boss!pw123")
[ -n "$SID_E" ] && ok "email domain '@tenantb.test' routed the login to tenant B" \
               || no "email-domain routing failed (login not resolved to tenant B)"
SID_EN=$(auth "mc_primary" "boss@tenantb.test" "Boss!pw123")
[ -z "$SID_EN" ] && ok "control: boss@tenantb.test does not exist in tenant A (proof is real)" \
                 || no "control: boss@tenantb.test unexpectedly authenticated in tenant A"

echo
if [ -n "$FAILED" ]; then echo '*** FAILURES ***'; else echo 'All checks passed.'; fi

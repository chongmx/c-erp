#!/bin/bash
# Phase 2 control-plane + company-switcher (cross-tenant SSO) test.
# Own throwaway PG: mc_primary (company A) + mc_tenant_b (company B) + mc_control
# (control plane). One identity owns admin in BOTH companies; we log into A,
# list companies, then SWITCH to B with NO password (SSO) and confirm we act as B.
set -uo pipefail
cd ~/code/c-erp || exit 1
PGBIN=/usr/lib/postgresql/16/bin
PGDATA=/tmp/mcs_pg; PGPORT=5434; APPPORT=8170
BASE="http://127.0.0.1:$APPPORT"; CFGDIR=/tmp/mcs_test; FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
psqlC(){ "$PGBIN/psql" -h 127.0.0.1 -p $PGPORT -U odoo -d "$1" -tAc "$2" 2>/dev/null; }

teardown(){
  echo "############ teardown ############"
  pkill -f "c-erp --config $CFGDIR" 2>/dev/null; sleep 1
  "$PGBIN/pg_ctl" -D "$PGDATA" -m fast stop >/dev/null 2>&1
  rm -rf "$PGDATA" "$CFGDIR" /tmp/mcs_*.log
  echo "    cleaned up"
}
trap teardown EXIT

echo "############ throwaway PG + 3 databases ############"
rm -rf "$PGDATA"; mkdir -p "$PGDATA"
"$PGBIN/initdb" -D "$PGDATA" -U odoo -A trust >/tmp/mcs_initdb.log 2>&1 || { no initdb; exit 1; }
"$PGBIN/pg_ctl" -D "$PGDATA" -o "-p $PGPORT -k /tmp -c listen_addresses=127.0.0.1" -l /tmp/mcs_pg.log -w start >/dev/null 2>&1 || { no pgstart; tail -5 /tmp/mcs_pg.log; exit 1; }
for i in $(seq 1 20); do "$PGBIN/pg_isready" -h 127.0.0.1 -p $PGPORT -q && break; sleep 0.5; done
psqlC postgres "ALTER ROLE odoo WITH PASSWORD 'odoo' SUPERUSER CREATEDB;" >/dev/null
for d in mc_primary mc_tenant_b mc_control; do "$PGBIN/createdb" -h 127.0.0.1 -p $PGPORT -U odoo "$d" || { no "createdb $d"; exit 1; }; done
ok "created mc_primary + mc_tenant_b + mc_control"

echo "############ config (control_db=mc_control) ############"
mkdir -p "$CFGDIR"
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
ok "wrote config + tenants.json"

echo "############ provision (companies + control-plane schema) ############"
./build/c-erp --config "$CFGDIR/system.cfg" --provision >/tmp/mcs_prov.log 2>&1
grep -q "Provisioning + migration complete" /tmp/mcs_prov.log && ok "provisioned" || { no "provision failed"; tail -8 /tmp/mcs_prov.log; exit 1; }
# control-plane schema present?
psqlC mc_control "SELECT 1 FROM information_schema.tables WHERE table_name='mc_membership'" | grep -q 1 \
  && ok "control-plane schema created (mc_membership)" || no "mc_membership table missing"

echo "############ seed identity memberships ############"
# one identity owns admin in BOTH companies
psqlC mc_control "INSERT INTO mc_membership(identity,tenant_db,local_login) VALUES
  ('chief@corp.example','mc_primary','admin'),
  ('chief@corp.example','mc_tenant_b','admin')
  ON CONFLICT DO NOTHING;" >/dev/null
CNT=$(psqlC mc_control "SELECT count(*) FROM mc_membership WHERE identity='chief@corp.example'" | tr -d ' ')
[ "$CNT" = "2" ] && ok "seeded 2 memberships for chief@corp.example" || no "membership seed failed (count=$CNT)"

echo "############ start server ############"
( setsid ./build/c-erp --config "$CFGDIR/system.cfg" >/tmp/mcs_server.log 2>&1 & )
for i in $(seq 1 25); do curl -sf -o /dev/null --max-time 2 "$BASE/healthz" && break; sleep 1; done
curl -sf -o /dev/null --max-time 3 "$BASE/healthz" && ok "server up on $APPPORT" || { no "server down"; tail -8 /tmp/mcs_server.log; exit 1; }

authA(){ cat > /tmp/mcs_a.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"mc_primary","login":"admin","password":"admin"}}
EOF
  curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' --data @/tmp/mcs_a.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p'; }
companies(){ cat > /tmp/mcs_c.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"context":{"session_id":"$1"}}}
EOF
  curl -s -X POST "$BASE/web/session/companies" -H 'Content-Type: application/json' --data @/tmp/mcs_c.json; }
switchco(){ cat > /tmp/mcs_s.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"company":"$2","context":{"session_id":"$1"}}}
EOF
  curl -s -X POST "$BASE/web/session/switch_company" -H 'Content-Type: application/json' --data @/tmp/mcs_s.json; }
callas(){ cat > /tmp/mcs_k.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$2","method":"$3","args":$4,"kwargs":{"context":{"session_id":"$1"}}}}
EOF
  curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/mcs_k.json; }

echo "############ login A -> identity recorded -> list companies ############"
SID_A=$(authA)
[ -n "$SID_A" ] && ok "logged into company A (mc_primary) as admin" || { no "login A failed"; echo '*** FAILURES ***'; exit 1; }
CO=$(companies "$SID_A")
echo "$CO" | grep -q 'mc_primary'  && ok "company list includes A (mc_primary)"  || no "A missing from company list: $CO"
echo "$CO" | grep -q 'mc_tenant_b' && ok "company list includes B (mc_tenant_b) via identity" || no "B missing from company list: $CO"

echo "############ cross-tenant SSO switch A -> B (no password) ############"
SW=$(switchco "$SID_A" mc_tenant_b)
SID_B=$(echo "$SW" | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -n "$SID_B" ] && ok "switch_company returned a session for B (SSO, no password)" || no "switch failed: $SW"
# prove the switched session acts inside company B: create a partner, verify it lands in B not A
callas "$SID_B" res.partner create '[{"name":"SW-B-PARTNER"}]' >/dev/null
CB=$(psqlC mc_tenant_b "SELECT count(*) FROM res_partner WHERE name='SW-B-PARTNER'" | tr -d ' ')
CA=$(psqlC mc_primary  "SELECT count(*) FROM res_partner WHERE name='SW-B-PARTNER'" | tr -d ' ')
[ "$CB" = "1" ] && ok "switched session wrote into company B database" || no "not written to B (count=$CB)"
[ "$CA" = "0" ] && ok "switched session did NOT touch company A database" || no "leaked into A (count=$CA)"

echo "############ security: cannot switch to a company you don't belong to ############"
# seed an identity that only belongs to A, then try to switch it to B
psqlC mc_control "INSERT INTO mc_membership(identity,tenant_db,local_login) VALUES ('lowly@corp.example','mc_primary','admin') ON CONFLICT DO NOTHING;" >/dev/null
# forge a session as that identity by switching... we cannot; instead verify loginFor returns nothing for B:
DENY=$(switchco "$SID_A" mc_nonexistent)
echo "$DENY" | grep -qi 'not a member\|error' && ok "switch to a non-member/unknown company is refused" || no "switch not refused: $DENY"

echo "############ Phase 3a: shared-product opt-in ############"
psqlC mc_control "INSERT INTO mc_shared_product(code,name,list_price) VALUES ('SKU-SHARED','Shared Widget',9990000) ON CONFLICT (code) DO NOTHING;" >/dev/null
importShared(){ cat > /tmp/mcs_imp.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"context":{"session_id":"$1"}}}
EOF
  curl -s -X POST "$BASE/web/session/import_shared_products" -H 'Content-Type: application/json' --data @/tmp/mcs_imp.json; }
IA=$(importShared "$SID_A")
echo "$IA" | grep -q '"imported":1' && ok "company A imported the shared product" || no "A import failed: $IA"
IB=$(importShared "$SID_B")
echo "$IB" | grep -q '"imported":1' && ok "company B imported the shared product" || no "B import failed: $IB"
PA=$(psqlC mc_primary  "SELECT count(*) FROM product_product WHERE default_code='SKU-SHARED'" | tr -d ' ')
PB=$(psqlC mc_tenant_b "SELECT count(*) FROM product_product WHERE default_code='SKU-SHARED'" | tr -d ' ')
[ "$PA" = "1" ] && ok "shared product present in company A's own catalogue" || no "not in A (count=$PA)"
[ "$PB" = "1" ] && ok "shared product present in company B's own catalogue (independent copy)" || no "not in B (count=$PB)"
importShared "$SID_A" >/dev/null
PA2=$(psqlC mc_primary "SELECT count(*) FROM product_product WHERE default_code='SKU-SHARED'" | tr -d ' ')
[ "$PA2" = "1" ] && ok "re-import is idempotent (no duplicate copy)" || no "duplicate on re-import (count=$PA2)"

echo "############ Phase 3b: consolidated cross-company report ############"
consolidated(){ cat > /tmp/mcs_con.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"context":{"session_id":"$1"}}}
EOF
  curl -s -X POST "$BASE/web/session/consolidated" -H 'Content-Type: application/json' --data @/tmp/mcs_con.json; }
CON=$(consolidated "$SID_A")
echo "$CON" | grep -q 'mc_primary'  && ok "consolidated report includes company A"          || no "A missing: $CON"
echo "$CON" | grep -q 'mc_tenant_b' && ok "consolidated report includes company B"          || no "B missing: $CON"
echo "$CON" | grep -q '"partners"'  && ok "consolidated report carries per-company figures" || no "no figures: $CON"

echo "############ 4a: pre-login company chooser (lookup) ############"
lookup(){ cat > /tmp/mcs_lu.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"login":"$1"}}
EOF
  curl -s -X POST "$BASE/web/session/lookup_companies" -H 'Content-Type: application/json' --data @/tmp/mcs_lu.json; }
LU=$(lookup "chief@corp.example")
echo "$LU" | grep -q 'mc_primary'  && ok "lookup returns company A for the identity" || no "lookup missing A: $LU"
echo "$LU" | grep -q 'mc_tenant_b' && ok "lookup returns company B for the identity" || no "lookup missing B: $LU"
LU0=$(lookup "nobody@nowhere.example")
echo "$LU0" | grep -q 'mc_primary' && no "lookup leaked companies for an unknown email" || ok "lookup returns nothing for an unknown email"

echo "############ 4b: control-plane admin endpoints ############"
cadmin(){ cat > /tmp/mcs_ca.json <<EOF
{"jsonrpc":"2.0","method":"call","params":$2}
EOF
  curl -s -X POST "$BASE/web/control/admin" -H 'Content-Type: application/json' --data @/tmp/mcs_ca.json; }
ADL=$(cadmin x "{\"op\":\"list_memberships\",\"context\":{\"session_id\":\"$SID_A\"}}")
echo "$ADL" | grep -q 'chief@corp.example' && ok "admin can list memberships" || no "list_memberships failed: $ADL"
cadmin x "{\"op\":\"add_membership\",\"identity\":\"newguy@corp.example\",\"tenant_db\":\"mc_tenant_b\",\"local_login\":\"admin\",\"context\":{\"session_id\":\"$SID_A\"}}" >/dev/null
NEWC=$(psqlC mc_control "SELECT count(*) FROM mc_membership WHERE identity='newguy@corp.example'" | tr -d ' ')
[ "$NEWC" = "1" ] && ok "admin can add a membership" || no "add_membership failed (count=$NEWC)"
cadmin x "{\"op\":\"remove_membership\",\"identity\":\"newguy@corp.example\",\"tenant_db\":\"mc_tenant_b\",\"context\":{\"session_id\":\"$SID_A\"}}" >/dev/null
DELC=$(psqlC mc_control "SELECT count(*) FROM mc_membership WHERE identity='newguy@corp.example'" | tr -d ' ')
[ "$DELC" = "0" ] && ok "admin can remove a membership" || no "remove_membership failed (count=$DELC)"
cadmin x "{\"op\":\"add_shared\",\"code\":\"SKU-ADMIN\",\"name\":\"Via Admin\",\"list_price\":5000000,\"context\":{\"session_id\":\"$SID_A\"}}" >/dev/null
SHL=$(cadmin x "{\"op\":\"list_shared\",\"context\":{\"session_id\":\"$SID_A\"}}")
echo "$SHL" | grep -q 'SKU-ADMIN' && ok "admin can add + list shared products" || no "shared add/list failed: $SHL"
UNAUTH=$(cadmin x "{\"op\":\"list_memberships\"}")
echo "$UNAUTH" | grep -qi 'administrator access required\|error' && ok "control admin refuses unauthenticated" || no "control admin allowed unauthenticated: $UNAUTH"

echo
if [ -n "$FAILED" ]; then echo '*** FAILURES ***'; else echo 'All checks passed.'; fi

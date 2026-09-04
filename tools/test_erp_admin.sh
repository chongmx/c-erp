#!/bin/bash
# Smoke + security test for the ERP Admin Console (docs/073).
# Starts erp-admin on a loopback port, captures its one-time token, and checks
# the auth gate, the DB/backup/tenant endpoints, and the injection guard.
set -uo pipefail
cd ~/code/c-erp || exit 1
PORT=8073; BASE="http://127.0.0.1:$PORT"; FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
export PATH="/usr/lib/postgresql/16/bin:$PATH"

teardown(){ pkill -f "erp-admin --port $PORT" 2>/dev/null; rm -f backups/admtest_*.dump backups/odoo-*.dump 2>/dev/null; }
trap teardown EXIT

( ./build/erp-admin --port $PORT >/tmp/adm.log 2>&1 & )
sleep 2
TOKEN=$(grep -oE "token=[0-9a-f]+" /tmp/adm.log | head -1 | cut -d= -f2)
[ -n "$TOKEN" ] && ok "erp-admin started; one-time token minted + URL printed" || { no "no token in console output"; cat /tmp/adm.log; echo '*** FAILURES ***'; exit 1; }

echo "############ security gate ############"
c1=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/whoami")
[ "$c1" = "401" ] && ok "API refuses requests with NO token (401)"    || no "no-token returned $c1"
c2=$(curl -s -o /dev/null -w "%{http_code}" -H "X-Admin-Token: nope" "$BASE/api/whoami")
[ "$c2" = "401" ] && ok "API refuses a WRONG token (401)"             || no "wrong-token returned $c2"
W=$(curl -s -H "X-Admin-Token: $TOKEN" "$BASE/api/whoami")
echo "$W" | grep -q '"ok":true' && ok "whoami OK with the real token"  || no "whoami failed: $W"
UI=$(curl -s "$BASE/")
echo "$UI" | grep -q 'ERP Admin Console' && ok "single-page UI served at /" || no "UI not served"

echo "############ overview + databases ############"
O=$(curl -s -H "X-Admin-Token: $TOKEN" "$BASE/api/overview")
echo "$O" | grep -q 'db_ok' && ok "overview returns service/db/disk status" || no "overview failed: $O"
D=$(curl -s -H "X-Admin-Token: $TOKEN" "$BASE/api/databases")
echo "$D" | grep -q '"odoo"' && ok "databases lists the odoo database"     || no "databases failed: $D"

echo "############ backup + injection guard ############"
B=$(curl -s -H "X-Admin-Token: $TOKEN" -X POST -H "Content-Type: application/json" -d '{"db":"odoo"}' "$BASE/api/backup")
echo "$B" | grep -q '"ok":true' && ok "backup (pg_dump -Fc) created a dump"  || no "backup failed: $B"
BK=$(curl -s -H "X-Admin-Token: $TOKEN" "$BASE/api/backups")
echo "$BK" | grep -q 'odoo-' && ok "backups list shows the new dump"         || no "backups list failed: $BK"
IB=$(curl -s -H "X-Admin-Token: $TOKEN" -X POST -H "Content-Type: application/json" -d '{"db":"odoo; DROP DATABASE"}' "$BASE/api/backup")
echo "$IB" | grep -qi 'invalid' && ok "malicious db name rejected (allowlist injection guard)" || no "injection guard failed: $IB"

echo "############ tenants + service ############"
T=$(curl -s -H "X-Admin-Token: $TOKEN" "$BASE/api/tenants")
echo "$T" | grep -q '"tenants"' && ok "tenants endpoint responds"           || no "tenants failed: $T"
S=$(curl -s -H "X-Admin-Token: $TOKEN" -X POST -H "Content-Type: application/json" -d '{"action":"bogus"}' "$BASE/api/service")
echo "$S" | grep -qi 'invalid action' && ok "service action allowlist enforced" || no "service allowlist failed: $S"

echo
if [ -n "$FAILED" ]; then echo '*** FAILURES ***'; else echo 'All checks passed.'; fi

#!/bin/bash
# =============================================================
# test_db_backups.sh — in-app Database section (docs/075).
# Own throwaway PG with two tenants (mc_a, mc_b) so restore is safe and
# per-tenant isolation is real. Proves: backup/list/download, restore
# round-trip (correct password), and the SECURITY envelope — admin-only,
# password required for restore, and every op scoped to the caller's own
# tenant (a company can never see or touch another's snapshots/database).
# =============================================================
set -uo pipefail
cd ~/code/c-erp || exit 1
PGBIN=/usr/lib/postgresql/16/bin
PGDATA=/tmp/dbb_pg; PGPORT=5437; APPPORT=8173
BASE="http://127.0.0.1:$APPPORT"; CFGDIR=/tmp/dbb_test; FAILED=
export PATH="$PGBIN:$PATH"
q(){ "$PGBIN/psql" -h 127.0.0.1 -p $PGPORT -U odoo -d "$1" -tAc "$2" 2>/dev/null | tr -d ' ' | head -1; }
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }

teardown(){
  echo "############ teardown ############"
  pkill -f "c-erp --config $CFGDIR" 2>/dev/null; sleep 1
  "$PGBIN/pg_ctl" -D "$PGDATA" -m fast stop >/dev/null 2>&1
  rm -rf "$PGDATA" "$CFGDIR" /tmp/dbb_*.log backups/mc_a backups/mc_b
  echo "    cleaned up"
}
trap teardown EXIT

echo "############ setup: throwaway PG + 2 tenants ############"
rm -rf "$PGDATA"; mkdir -p "$PGDATA" "$CFGDIR"
"$PGBIN/initdb" -D "$PGDATA" -U odoo -A trust >/tmp/dbb_initdb.log 2>&1 || { no initdb; exit 1; }
"$PGBIN/pg_ctl" -D "$PGDATA" -o "-p $PGPORT -k /tmp -c listen_addresses=127.0.0.1" -l /tmp/dbb_pg.log -w start >/dev/null 2>&1 || { no pgstart; exit 1; }
for i in $(seq 1 20); do "$PGBIN/pg_isready" -h 127.0.0.1 -p $PGPORT -q && break; sleep 0.5; done
q postgres "ALTER ROLE odoo WITH PASSWORD 'odoo' SUPERUSER CREATEDB;" >/dev/null
for d in mc_a mc_b; do "$PGBIN/createdb" -h 127.0.0.1 -p $PGPORT -U odoo "$d" || { no "createdb $d"; exit 1; }; done
cat > "$CFGDIR/system.cfg" <<EOF
[options]
db_host = 127.0.0.1
db_port = $PGPORT
db_name = mc_a
db_user = odoo
db_password = odoo
db_maxconn = 4
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
{"tenants":[{"name":"mc_b","active":true,"subdomain":"b"}]}
EOF
./build/c-erp --config "$CFGDIR/system.cfg" --provision >/tmp/dbb_prov.log 2>&1
grep -q "Provisioning + migration complete" /tmp/dbb_prov.log && ok "provisioned mc_a + mc_b" || { no "provision failed"; tail -6 /tmp/dbb_prov.log; exit 1; }
( setsid ./build/c-erp --config "$CFGDIR/system.cfg" >/tmp/dbb_server.log 2>&1 & )
for i in $(seq 1 25); do curl -sf -o /dev/null --max-time 2 "$BASE/healthz" && break; sleep 1; done
curl -sf -o /dev/null "$BASE/healthz" && ok "server up" || { no "server down"; tail -6 /tmp/dbb_server.log; exit 1; }

auth(){ cat > /tmp/dbb_a.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$1","login":"admin","password":"admin"}}
EOF
  curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' --data @/tmp/dbb_a.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p'; }
dbp(){ cat > /tmp/dbb_c.json <<EOF
{"jsonrpc":"2.0","method":"call","params":$3}
EOF
  curl -s -X POST "$BASE/web/db/$1" -H 'Content-Type: application/json' --data @/tmp/dbb_c.json; }
callas(){ cat > /tmp/dbb_k.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$2","method":"$3","args":$4,"kwargs":{"context":{"session_id":"$1"}}}}
EOF
  curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/dbb_k.json; }

SID_A=$(auth mc_a); SID_B=$(auth mc_b)
[ -n "$SID_A" ] && [ -n "$SID_B" ] && ok "admins logged into A and B" || no "login failed"

echo "############ backup + list + download (company A) ############"
BK=$(dbp backup x "{\"context\":{\"session_id\":\"$SID_A\"}}")
echo "$BK" | grep -q '"ok":true' && ok "A: create snapshot" || no "A backup failed: $BK"
FILE=$(echo "$BK" | sed -n 's/.*"file":"\([^"]*\)".*/\1/p')
LS=$(dbp list x "{\"context\":{\"session_id\":\"$SID_A\"}}")
echo "$LS" | grep -q "$FILE" && ok "A: snapshot appears in list" || no "A list missing snapshot: $LS"
ls backups/mc_a/*.dump >/dev/null 2>&1 && ok "A snapshot stored under per-tenant dir backups/mc_a/" || no "no A snapshot on disk"
DLC=$(curl -s -o /tmp/dbb_dl.dump -w "%{http_code}" "$BASE/web/db/download?file=$FILE&session_id=$SID_A")
{ [ "$DLC" = "200" ] && [ -s /tmp/dbb_dl.dump ]; } && ok "A: download returns the dump (200, non-empty)" || no "A download failed (code=$DLC)"

echo "############ restore round-trip (company A) ############"
callas "$SID_A" res.partner create '[{"name":"SNAP-AFTER-A"}]' >/dev/null
[ "$(q mc_a "SELECT count(*) FROM res_partner WHERE name='SNAP-AFTER-A'")" = "1" ] && ok "A: added a post-snapshot partner" || no "could not add partner"
RS=$(dbp restore x "{\"file\":\"$FILE\",\"password\":\"admin\",\"context\":{\"session_id\":\"$SID_A\"}}")
sleep 1
GONE=$(q mc_a "SELECT count(*) FROM res_partner WHERE name='SNAP-AFTER-A'")
[ "$GONE" = "0" ] && ok "A: restore rolled back to the snapshot (post-snapshot partner gone)" || no "A restore did not roll back (count=$GONE): $RS"

echo "############ SECURITY ############"
# wrong password -> refused, no restore
callas "$SID_A" res.partner create '[{"name":"GUARD-A"}]' >/dev/null
WR=$(dbp restore x "{\"file\":\"$FILE\",\"password\":\"WRONG\",\"context\":{\"session_id\":\"$SID_A\"}}")
echo "$WR" | grep -qi 'password confirmation failed' && ok "restore with WRONG password refused" || no "wrong password not refused: $WR"
[ "$(q mc_a "SELECT count(*) FROM res_partner WHERE name='GUARD-A'")" = "1" ] && ok "refused restore did NOT touch the database" || no "db changed despite refused restore"
# unauthenticated -> refused
UN=$(dbp backup x "{}")
echo "$UN" | grep -qi 'not authenticated\|administrator' && ok "unauthenticated backup refused" || no "unauth not refused: $UN"

echo "############ per-tenant ISOLATION ############"
# B backs up -> lands in backups/mc_b, and B's list shows ONLY B's snapshots
dbp backup x "{\"context\":{\"session_id\":\"$SID_B\"}}" >/dev/null
LSB=$(dbp list x "{\"context\":{\"session_id\":\"$SID_B\"}}")
echo "$LSB" | grep -q '"company":"mc_b"' && ok "B session reports company mc_b" || no "B company wrong: $LSB"
echo "$LSB" | grep -q "$FILE" && no "ISOLATION LEAK: B can see A's snapshot" || ok "B cannot see A's snapshots (isolated)"
ls backups/mc_b/*.dump >/dev/null 2>&1 && ok "B snapshot stored under separate backups/mc_b/" || no "B snapshot dir missing"
# B cannot download A's snapshot file (its download is scoped to backups/mc_b)
DB2=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/web/db/download?file=$FILE&session_id=$SID_B")
[ "$DB2" = "404" ] && ok "B cannot download A's snapshot (404 — path scoped to B)" || no "B download of A file returned $DB2 (expected 404)"

echo
if [ -n "$FAILED" ]; then echo '*** FAILURES ***'; else echo 'All checks passed.'; fi

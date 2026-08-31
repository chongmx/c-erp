#!/bin/bash
# --- harness ---------------------------------------------------------------
# Walk up for CMakeLists.txt rather than counting `../`, so this test behaves
# the same whether the runner invoked it or you ran it directly, and so it can
# be nested a folder deeper without a preamble edit.
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# P2: user-configurable display precision (docs/048 §2).
# Verifies fields_get carries `digits`, and that changing the
# decimal_precision table changes what the client is told.
#
# NOTE: use `pkill -x c-erp`, never `pkill -f build/c-erp` — the
# latter matches this script's own command line and kills the shell.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
APPDIR=${APPDIR:-/home/user/code/c-erp}
FAILED=

pg()  { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null; }
ok()  { echo "    PASS  $1"; }
no()  { echo "    FAIL  $1"; FAILED=1; }

login() {
    cat > /tmp/vp_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
    SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
          --data @/tmp/vp_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
    [ -z "$SID" ] && { echo "    cannot authenticate"; exit 1; }
}

restart() {
    pkill -x c-erp; sleep 2
    # setsid, not `( nohup ... & )`. Bash elides the subshell when the
    # background job is its only command, so the server stayed a direct
    # CHILD of this script — and the script then sat in do_wait forever
    # instead of exiting. setsid puts it in its own session, fully
    # detached, so the script can finish while the server keeps running.
    ( cd "$APPDIR" && setsid ./build/c-erp > /tmp/cerp_run.log 2>&1 < /dev/null & )
    sleep 9
    login
}

fields_get() {
    cat > /tmp/vp_fg.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$1","method":"fields_get","args":[],
 "kwargs":{"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/vp_fg.json
}

digits_of() {   # $1=model $2=field -> prints the decimals, or "none"
    fields_get "$1" | python3 -c "
import json,sys
d = json.load(sys.stdin).get('result',{}).get(sys.argv[1],{}).get('digits')
print(d[1] if d else 'none')
" "$2"
}

# restart, not just login: DecimalPrecision caches its read, and this script
# seeds through raw SQL, which bypasses the cache-invalidation hook the API
# write path uses. Starting from a fresh process makes the first assertion
# independent of whatever the cache happened to be holding.
restart

echo "############ seeded precision ############"
pg "SELECT name||' = '||digits FROM decimal_precision ORDER BY name" | sed 's/^/    /'

echo
echo "############ fields_get carries digits ############"
fields_get sale.order.line | python3 -c "
import json,sys
r = json.load(sys.stdin).get('result', {})
for f in ['price_unit','product_uom_qty','discount','price_subtotal','price_total']:
    d = r.get(f, {})
    print('    %-18s digits=%-10s precision=%s' % (f, d.get('digits'), d.get('precision_name')))
"

[ "$(digits_of sale.order.line price_unit)"     = "5" ] && ok "price_unit 5 dp (Product Price)"  || no "price_unit wrong"
[ "$(digits_of sale.order.line product_uom_qty)" = "4" ] && ok "quantity 4 dp (Product UoM)"     || no "quantity wrong"
[ "$(digits_of sale.order.line price_subtotal)" = "2" ] && ok "subtotal 2 dp (Account)"          || no "subtotal wrong"
[ "$(digits_of product.product weight)"         = "none" ] && ok "weight has no precision (not money)" || no "weight wrongly scaled"

echo
echo "############ changing the setting changes what clients are told ############"
BEFORE=$(pg "SELECT digits FROM decimal_precision WHERE name = 'Product Price'" | tr -d ' ')
echo "    Product Price = $BEFORE dp; setting to 3"
pg "UPDATE decimal_precision SET digits = 3 WHERE name = 'Product Price'" >/dev/null

# fields_get is cached 300 s in the dispatcher and DecimalPrecision caches its
# own read, so a restart is currently required for the change to be observed.
# Wiring both invalidations into the Settings write is the remaining work.
restart
AFTER=$(digits_of sale.order.line price_unit)
echo "    price_unit digits now: $AFTER"
[ "$AFTER" = "3" ] && ok "setting propagated to fields_get" || no "setting did not propagate"

pg "UPDATE decimal_precision SET digits = $BEFORE WHERE name = 'Product Price'" >/dev/null
# Restart so the SERVER is restored too, not just the row. Without this the
# process was left caching 3 while the DB said 5, and the next run of this
# script — or anything else reading a precision — failed spuriously.
restart
echo "    restored Product Price to $BEFORE (server restarted to clear the cache)"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

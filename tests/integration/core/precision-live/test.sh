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
# P2: writing decimal.precision through the API must take effect
# IMMEDIATELY — no restart. This is what the CacheInvalidation
# hook exists for: without it, the dispatcher's 300 s fields_get
# cache and DecimalPrecision's own cache both serve stale values.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cat > /tmp/vpl_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/vpl_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }

call() {   # $1=model $2=method $3=args  [$4 = extra kwargs, with trailing comma]
    cat > /tmp/vpl.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$1","method":"$2","args":$3,
 "kwargs":{${4:-}"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/vpl.json
}

digits_of() {
    call "$1" fields_get '[]' | python3 -c "
import json,sys
d=json.load(sys.stdin).get('result',{}).get(sys.argv[1],{}).get('digits')
print(d[1] if d else 'none')
" "$2"
}

echo "############ decimal.precision is readable over RPC ############"
R=$(call decimal.precision search_read '[[]]' '"fields":["id","name","digits"],')
echo "    $(echo "$R" | head -c 240)"
echo "$R" | grep -q '"Product Price"' && ok "model exposed and readable" || no "not readable: $R"

PREC_ID=$(pg "SELECT id FROM decimal_precision WHERE name = 'Product Price'")
BEFORE=$(pg "SELECT digits FROM decimal_precision WHERE name = 'Product Price'")
echo "    Product Price id=$PREC_ID currently $BEFORE dp"

echo
echo "############ write via RPC takes effect WITHOUT a restart ############"
echo "    fields_get before: $(digits_of sale.order.line price_unit)"
R=$(call decimal.precision write "[[$PREC_ID],{\"digits\":3}]")
echo "    write -> $(echo "$R" | head -c 80)"
AFTER=$(digits_of sale.order.line price_unit)
echo "    fields_get after:  $AFTER"
[ "$AFTER" = "3" ] && ok "change visible immediately (caches invalidated)" \
                   || no "still $AFTER — cache not invalidated"

echo
echo "############ validation ############"
R=$(call decimal.precision write "[[$PREC_ID],{\"digits\":9}]")
echo "$R" | grep -qi "between 0 and 6" && ok "digits > Money::SCALE rejected with a clear message" \
                                       || no "9 dp was not rejected: $(echo "$R"|head -c 140)"

R=$(call decimal.precision write "[[$PREC_ID],{\"digits\":-1}]")
echo "$R" | grep -qi "between 0 and 6" && ok "negative digits rejected" \
                                       || no "-1 was not rejected"

# restore
call decimal.precision write "[[$PREC_ID],{\"digits\":$BEFORE}]" >/dev/null
echo "    restored to $(digits_of sale.order.line price_unit) dp"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

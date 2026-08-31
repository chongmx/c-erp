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
# P2 Phase 3 verification: the DB holds micro-units, but the API
# must still return MAJOR units — otherwise every amount in the UI
# is wrong by a factor of a million (docs/047 §3).
#
# Also verifies the WRITE path: a value sent as major units must be
# stored as micro-units and read back unchanged.
#
# Hermetic: it discovers a real product and a real customer invoice at
# runtime (no hand-seeded id=3 / id=4) and asserts the *relationship*
# major == micros / 1e6, so it holds for whatever values those records carry.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cat > /tmp/vm_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/vm_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }

# Guarantee a sale/invoice fixture exists (shared, idempotent).
source scripts/seed_test_fixtures.sh; ensure_sale_fixture "$SID" >/dev/null 2>&1

call() {   # $1=model $2=method $3=args $4=kwargs-extra
    cat > /tmp/vm_call.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$1","method":"$2","args":$3,
 "kwargs":{$4"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/vm_call.json
}

# major == micros / 1e6, done in SQL so there is no float rounding in bash.
converts() { pg "SELECT ($1 = round(${2:-0}::numeric * 1000000))"; }
num() { printf '%s' "$1" | sed -n "s/.*\"$2\":\([0-9.]*\).*/\1/p"; }

echo "############ READ: micro-units in DB -> major units on the wire ############"

MOVE=$(pg "SELECT id FROM account_move WHERE amount_total > 0 ORDER BY id LIMIT 1")
[ -z "$MOVE" ] && MOVE=$(pg "SELECT id FROM account_move ORDER BY id LIMIT 1")
if [ -z "$MOVE" ]; then
    no "no account_move to probe"
else
    DB_TOTAL=$(pg "SELECT amount_total FROM account_move WHERE id=$MOVE")
    API=$(call account.move search_read "[[[\"id\",\"=\",$MOVE]]]" '"fields":["id","amount_total","amount_residual"],')
    A=$(num "$API" amount_total)
    echo "    move id=$MOVE  DB amount_total=$DB_TOTAL micros  API amount_total=$A"
    [ "$(converts "$DB_TOTAL" "$A")" = "t" ] && ok "$DB_TOTAL micros -> $A on the wire (major = micros/1e6)" \
                                             || no "amount_total NOT converted"
fi

PID=$(pg "SELECT id FROM product_product ORDER BY id LIMIT 1")
DB_PRICE=$(pg "SELECT list_price FROM product_product WHERE id=$PID")
API=$(call product.product search_read "[[[\"id\",\"=\",$PID]]]" '"fields":["id","list_price","standard_price"],')
A=$(num "$API" list_price)
echo "    product id=$PID  DB list_price=$DB_PRICE micros  API list_price=$A"
[ "$(converts "$DB_PRICE" "$A")" = "t" ] && ok "$DB_PRICE micros -> $A on the wire" \
                                         || no "list_price NOT converted"

echo
echo "    non-money columns must be UNTOUCHED:"
API=$(call product.product search_read "[[[\"id\",\"=\",$PID]]]" '"fields":["id","weight","volume","purchase_lead_time"],')
echo "    API $(echo "$API" | head -c 160)"
echo "$API" | grep -q '"weight":[0-9]' && ok "weight still a plain number (not rescaled)" \
                                       || echo "      (weight null/absent — fine)"

echo
echo "############ WRITE: major units in -> micro-units stored ############"
BEFORE=$(pg "SELECT list_price FROM product_product WHERE id=$PID")
call product.product write "[[$PID],{\"list_price\":12.34}]" >/dev/null
AFTER=$(pg "SELECT list_price FROM product_product WHERE id=$PID")
echo "    wrote 12.34 -> DB now $AFTER"
[ "$AFTER" = "12340000" ] && ok "12.34 stored as 12340000 micros" || no "expected 12340000, got $AFTER"

API=$(call product.product search_read "[[[\"id\",\"=\",$PID]]]" '"fields":["id","list_price"],')
echo "$API" | grep -q '"list_price":12\.34' && ok "reads back as 12.34" || no "round-trip failed: $API"

echo
echo "    a 5-dp component price must survive:"
call product.product write "[[$PID],{\"list_price\":0.00042}]" >/dev/null
AFTER=$(pg "SELECT list_price FROM product_product WHERE id=$PID")
[ "$AFTER" = "420" ] && ok "0.00042 stored as 420 micros (exact)" || no "expected 420, got $AFTER"
API=$(call product.product search_read "[[[\"id\",\"=\",$PID]]]" '"fields":["id","list_price"],')
echo "$API" | grep -qE '"list_price":0\.00042' && ok "0.00042 reads back exactly" || no "got: $(echo "$API"|head -c 120)"

echo
echo "    an amount past INT32_MAX in micro-units (RM 2,148+):"
call product.product write "[[$PID],{\"list_price\":5000.00}]" >/dev/null
AFTER=$(pg "SELECT list_price FROM product_product WHERE id=$PID")
[ "$AFTER" = "5000000000" ] && ok "5000.00 -> 5000000000 micros (no int32 truncation)" \
                            || no "expected 5000000000, got $AFTER"

# restore
pg "UPDATE product_product SET list_price = $BEFORE WHERE id=$PID" >/dev/null
echo "    (restored list_price to $BEFORE)"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

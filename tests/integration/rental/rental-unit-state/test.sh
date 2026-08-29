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
# Rental phase 3 — unit state derivation (docs/054).
#
# rental_unit.state is derived from the contract lines by a database
# TRIGGER, so that it cannot drift no matter which code path wrote the
# line. That claim is only worth something if it is tested through
# several different paths, so this drives it through raw SQL as well as
# the API — raw SQL being precisely the path a C++ helper would miss.
#
# Also asserts the two things a naive derivation gets wrong:
#   * maintenance/retired are operator facts and must survive
#   * returning from maintenance must RE-DERIVE, not assume 'available'
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
M=1000000

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cleanup() {
    pg "DELETE FROM rental_contract_line WHERE contract_id IN (SELECT id FROM rental_contract WHERE name LIKE 'USTEST/%')" >/dev/null
    pg "DELETE FROM rental_contract      WHERE name LIKE 'USTEST/%'" >/dev/null
    pg "DELETE FROM rental_event         WHERE unit_id IN (SELECT id FROM rental_unit WHERE code LIKE 'UST-%')" >/dev/null
    pg "DELETE FROM rental_unit          WHERE code LIKE 'UST-%'" >/dev/null
}
cleanup

echo "############ 0. the trigger exists ############"
TRG=$(pg "SELECT tgname FROM pg_trigger WHERE tgname = 'rental_contract_line_state'")
FN=$(pg "SELECT proname FROM pg_proc WHERE proname = 'rental_unit_derive_state'")
MIG=$(pg "SELECT count(*) FROM schema_migrations WHERE version = 811")
[ "$MIG" = "1" ] && ok "migration 811 applied"              || no "811 not applied"
[ -n "$FN" ]     && ok "rental_unit_derive_state() exists"  || no "derive function missing"
[ -n "$TRG" ]    && ok "trigger on rental_contract_line"    || no "trigger missing"

PARTNER=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
UNIT=$(pg "INSERT INTO rental_unit (code,name,state,company_id)
           VALUES ('UST-01','State probe','available',1) RETURNING id")
CON=$(pg "INSERT INTO rental_contract (name,partner_id,state,date_start,company_id)
          VALUES ('USTEST/1',$PARTNER,'active',CURRENT_DATE,1) RETURNING id")
echo "    unit=$UNIT contract=$CON  initial state=$(pg "SELECT state FROM rental_unit WHERE id=$UNIT")"

echo
echo "############ 1. a PENDING line reserves the unit ############"
# Raw SQL on purpose: this is the path a C++-only derivation would miss.
# partner_id became NOT NULL in migration 812 — a walk-in has no contract
# to carry the customer, so the line carries it directly.
LINE=$(pg "INSERT INTO rental_contract_line (contract_id,partner_id,unit_id,date_start,unit_price,state,company_id)
           VALUES ($CON,$PARTNER,$UNIT,CURRENT_DATE + 30,$((120*M)),'pending',1) RETURNING id")
S=$(pg "SELECT state FROM rental_unit WHERE id=$UNIT")
echo "    after inserting a pending line: $S"
[ "$S" = "reserved" ] && ok "unit reserved by a future line (via raw SQL)" || no "state is '$S', expected reserved"

echo
echo "############ 2. activating the line occupies the unit ############"
pg "UPDATE rental_contract_line SET state='active' WHERE id=$LINE" >/dev/null
S=$(pg "SELECT state FROM rental_unit WHERE id=$UNIT")
echo "    after activating: $S"
[ "$S" = "occupied" ] && ok "unit occupied" || no "state is '$S', expected occupied"

echo
echo "############ 3. ending the line releases the unit ############"
pg "UPDATE rental_contract_line SET state='ended' WHERE id=$LINE" >/dev/null
S=$(pg "SELECT state FROM rental_unit WHERE id=$UNIT")
echo "    after ending: $S"
[ "$S" = "available" ] && ok "unit released" || no "state is '$S', expected available"

echo
echo "############ 4. DELETING a line releases the unit ############"
# The DELETE branch of the trigger — easy to omit, and the symptom is a
# permanently unlettable locker.
L2=$(pg "INSERT INTO rental_contract_line (contract_id,partner_id,unit_id,date_start,unit_price,state,company_id)
         VALUES ($CON,$PARTNER,$UNIT,CURRENT_DATE,$((120*M)),'active',1) RETURNING id")
S1=$(pg "SELECT state FROM rental_unit WHERE id=$UNIT")
pg "DELETE FROM rental_contract_line WHERE id=$L2" >/dev/null
S2=$(pg "SELECT state FROM rental_unit WHERE id=$UNIT")
echo "    occupied=$S1 -> after delete=$S2"
[ "$S1" = "occupied" ]  && ok "line re-occupied the unit"      || no "state was '$S1'"
[ "$S2" = "available" ] && ok "deleting the line released it"  || no "state is '$S2' after delete"

echo
echo "############ 5. MOVING a line releases the old unit and claims the new ############"
UNIT2=$(pg "INSERT INTO rental_unit (code,name,state,company_id)
            VALUES ('UST-02','Second probe','available',1) RETURNING id")
L3=$(pg "INSERT INTO rental_contract_line (contract_id,partner_id,unit_id,date_start,unit_price,state,company_id)
         VALUES ($CON,$PARTNER,$UNIT,CURRENT_DATE,$((120*M)),'active',1) RETURNING id")
pg "UPDATE rental_contract_line SET unit_id=$UNIT2 WHERE id=$L3" >/dev/null
A=$(pg "SELECT state FROM rental_unit WHERE id=$UNIT")
B=$(pg "SELECT state FROM rental_unit WHERE id=$UNIT2")
echo "    old unit=$A  new unit=$B"
[ "$A" = "available" ] && ok "old unit released on move" || no "old unit is '$A' — OLD.unit_id branch missing"
[ "$B" = "occupied" ]  && ok "new unit claimed on move"  || no "new unit is '$B'"

echo
echo "############ 6. maintenance is an OPERATOR fact and survives ############"
# The trigger must never overwrite it — otherwise a contract line silently
# puts a broken locker back into service.
pg "UPDATE rental_unit SET state='maintenance' WHERE id=$UNIT2" >/dev/null
pg "UPDATE rental_contract_line SET state='ended' WHERE id=$L3" >/dev/null
S=$(pg "SELECT state FROM rental_unit WHERE id=$UNIT2")
echo "    unit in maintenance after its line ended: $S"
[ "$S" = "maintenance" ] && ok "maintenance survived a line change" || no "state is '$S' — operator fact overwritten"

pg "UPDATE rental_contract_line SET state='active' WHERE id=$L3" >/dev/null
S=$(pg "SELECT state FROM rental_unit WHERE id=$UNIT2")
[ "$S" = "maintenance" ] && ok "maintenance survived re-activation too" || no "state is '$S'"

echo
echo "############ 7. returning from maintenance RE-DERIVES ############"
# The subtle one. The unit was let while it was out of service, so coming
# back it must be 'occupied', not 'available'. Assuming 'available' here
# would offer an already-let locker to a second tenant.
pg "UPDATE rental_unit SET state='available' WHERE id=$UNIT2" >/dev/null
pg "SELECT rental_unit_derive_state($UNIT2)" >/dev/null
S=$(pg "SELECT state FROM rental_unit WHERE id=$UNIT2")
echo "    returned to service while still let: $S"
[ "$S" = "occupied" ] && ok "re-derived to occupied, not assumed available" \
                      || no "state is '$S' — a let unit would be offered again"

echo
echo "############ 8. state changes through the API too ############"
cat > /tmp/us_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/us_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
if [ -z "$SID" ]; then
    no "cannot authenticate"
else
    UNIT3=$(pg "INSERT INTO rental_unit (code,name,state,company_id)
                VALUES ('UST-03','API probe','available',1) RETURNING id")
    cat > /tmp/us_c.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"rental.contract.line","method":"create",
 "args":[{"contract_id":$CON,"partner_id":$PARTNER,"unit_id":$UNIT3,"date_start":"$(date +%F)",
          "unit_price":120.00,"state":"active"}],
 "kwargs":{"context":{"session_id":"$SID"}}}}
EOF
    R=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/us_c.json)
    S=$(pg "SELECT state FROM rental_unit WHERE id=$UNIT3")
    echo "    created a line via the API -> unit state: $S"
    [ "$S" = "occupied" ] && ok "the API path derives state identically" \
                          || no "state is '$S' after API create: $(printf '%s' "$R" | head -c 100)"
    # The rate must have gone in as micro-units, not 120.
    P=$(pg "SELECT unit_price FROM rental_contract_line WHERE unit_id=$UNIT3 LIMIT 1")
    [ "$P" = "120000000" ] && ok "unit_price stored as micro-units through the API" \
                           || no "unit_price is $P"
fi

echo
echo "############ 9. recomputeAll finds nothing to fix ############"
# If the trigger has been keeping up, a full reconcile changes zero rows.
# A non-zero result here means something wrote state behind the trigger.
DRIFT=$(pg "SELECT count(*) FROM rental_unit u
             WHERE u.state NOT IN ('maintenance','retired')
               AND u.state IS DISTINCT FROM (
                     SELECT COALESCE(CASE
                              WHEN bool_or(l.state='active')  THEN 'occupied'
                              WHEN bool_or(l.state='pending') THEN 'reserved'
                              ELSE 'available' END, 'available')
                       FROM rental_contract_line l
                      WHERE l.unit_id = u.id AND l.state IN ('pending','active'))")
echo "    units whose stored state disagrees with their lines: $DRIFT"
[ "$DRIFT" = "0" ] && ok "no drift anywhere in the table" || no "$DRIFT units have drifted"

echo
echo "############ cleanup ############"
cleanup
LEFT=$(pg "SELECT count(*) FROM rental_unit WHERE code LIKE 'UST-%'")
[ "$LEFT" = "0" ] && ok "test data removed" || no "$LEFT units leaked"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

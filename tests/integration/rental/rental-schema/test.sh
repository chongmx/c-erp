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
# Rental module — phase 1/2 (docs/054).
#
# Asserts the GUARANTEES, not just that tables exist. Specifically the
# two constraints the whole module leans on:
#
#   * UNIQUE (contract_line_id, period_start) — double-billing is
#     impossible even when the cron fires twice
#   * one live contract line per unit — the double-let guard
#
# Each is proved by attempting the violation and requiring it to FAIL.
# A constraint that is never exercised is a comment, not a guarantee.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
M=1000000

pg()  { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
# Deliberately keeps stderr: used when a statement is EXPECTED to fail.
pgE() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>&1 | head -3; }
ok()  { echo "    PASS  $1"; }
no()  { echo "    FAIL  $1"; FAILED=1; }

cleanup() {
    pg "DELETE FROM rental_invoice_link  WHERE contract_id IN (SELECT id FROM rental_contract WHERE name LIKE 'RTEST/%')" >/dev/null
    pg "DELETE FROM rental_contract_line WHERE contract_id IN (SELECT id FROM rental_contract WHERE name LIKE 'RTEST/%')" >/dev/null
    pg "DELETE FROM rental_event         WHERE summary LIKE 'RTEST %'" >/dev/null
    pg "DELETE FROM rental_contract      WHERE name LIKE 'RTEST/%'" >/dev/null
    pg "DELETE FROM rental_expense       WHERE name LIKE 'RTEST %'" >/dev/null
    pg "DELETE FROM rental_unit          WHERE code LIKE 'RTEST-%'" >/dev/null
}
cleanup   # in case an interrupted run left rows behind

echo "############ 1. migrations applied ############"
# Every 800-series migration this module registers must be applied — a
# gap means a table or constraint is silently absent. Asserted as "no
# gaps in the range" rather than a hard-coded total, which goes stale
# every time a phase adds a migration (it did, on 811).
N=$(pg "SELECT count(*) FROM schema_migrations WHERE version BETWEEN 800 AND 899")
LO=$(pg "SELECT min(version) FROM schema_migrations WHERE version BETWEEN 800 AND 899")
HI=$(pg "SELECT max(version) FROM schema_migrations WHERE version BETWEEN 800 AND 899")
echo "    800-series migrations recorded: $N (versions $LO..$HI)"
[ "$N" -ge 11 ] && ok "the phase 1-2 migrations are applied" || no "only $N applied, expected at least 11"
[ "$N" = "$((HI - LO + 1))" ] && ok "no gaps between $LO and $HI" \
                             || no "gap in the applied range — a migration failed or was skipped"
# 800-810 are numerically BELOW the already-applied 900-1010. They ran
# anyway, which is the MigrationRunner behaviour docs/054 §0 depends on.
MAXV=$(pg "SELECT max(version) FROM schema_migrations")
[ "$MAXV" -gt 810 ] && ok "applied below the existing high-water mark ($MAXV) — set membership, not max" \
                    || no "unexpected max version $MAXV"

echo
echo "############ 2. money columns are BIGINT, physical columns are not ############"
for col in "rental_unit_type:default_rate" "rental_contract:deposit_amount" \
           "rental_contract_line:unit_price" "rental_contract_line:discount_pct" \
           "rental_invoice_link:amount" "rental_expense:amount"; do
    t=${col%%:*}; c=${col##*:}
    dt=$(pg "SELECT data_type FROM information_schema.columns WHERE table_name='$t' AND column_name='$c'")
    [ "$dt" = "bigint" ] && ok "$t.$c is BIGINT (micro-units)" || no "$t.$c is $dt, expected bigint"
done
# Area and volume are physical measurements, not money. Scaling them would
# be wrong in the other direction.
dt=$(pg "SELECT data_type FROM information_schema.columns WHERE table_name='rental_unit' AND column_name='area_sqm'")
[ "$dt" = "numeric" ] && ok "rental_unit.area_sqm left NUMERIC (physical, not money)" || no "area_sqm is $dt"

echo
echo "############ 3. seed data ############"
UT=$(pg "SELECT count(*) FROM rental_unit_type")
EC=$(pg "SELECT count(*) FROM rental_expense_category")
RATE=$(pg "SELECT default_rate FROM rental_unit_type WHERE code='SL'")
echo "    unit types=$UT expense categories=$EC  Small Locker rate=$RATE"
[ "$UT" -ge 5 ]        && ok "unit types seeded"            || no "only $UT unit types"
[ "$EC" -ge 7 ]        && ok "expense categories seeded"    || no "only $EC categories"
[ "$RATE" = "120000000" ] && ok "rate stored as micro-units (120.00)" || no "rate is $RATE"

echo
echo "############ 4. sequence and cron registered ############"
SEQ=$(pg "SELECT prefix FROM ir_sequence WHERE code='rental.contract'")
CRN=$(pg "SELECT count(*) FROM ir_cron WHERE code LIKE 'rental.%'")
ACT=$(pg "SELECT count(*) FROM ir_cron WHERE code LIKE 'rental.%' AND active")
echo "    contract sequence prefix='$SEQ'  rental cron jobs=$CRN (active: $ACT)"
[ -n "$SEQ" ]    && ok "contract numbering uses ir.sequence, not COUNT(*)+1" || no "no rental.contract sequence"
[ "$CRN" = "2" ] && ok "exactly 2 rental jobs — no duplicate codes"          || no "expected 2 cron jobs, got $CRN"
# Both handlers now exist (RentalBilling and RentalExpenses register them
# at boot), so both jobs SHOULD be active. Migration 810 still seeds them
# inactive; RentalModule::initialize activates them after binding the
# handlers, so a job is never active without something to service it.
#
# This assertion was inverted until phase 5 shipped — it required them to
# be OFF, which was correct when nothing could run them.
[ "$ACT" = "2" ] && ok "both jobs active — their handlers are registered at boot" \
                 || no "$ACT of $CRN rental jobs active; expected 2"

echo
echo "############ 5. THE anti-double-billing constraint ############"
# Set up a contract with one line, then try to link two invoices to the
# SAME (line, period_start). The second must be rejected.
PARTNER=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
UNIT=$(pg "INSERT INTO rental_unit (code,name,state,company_id)
           VALUES ('RTEST-A01','Probe unit','available',1) RETURNING id")
CON=$(pg "INSERT INTO rental_contract (name,partner_id,state,date_start,company_id)
          VALUES ('RTEST/1',$PARTNER,'active',CURRENT_DATE,1) RETURNING id")
# partner_id became NOT NULL in migration 812 (a walk-in has no contract
# to carry the customer). It must also match the contract's partner, which
# a trigger enforces.
LINE=$(pg "INSERT INTO rental_contract_line
             (contract_id,partner_id,unit_id,date_start,unit_price,state,
              next_period_start,company_id)
           VALUES ($CON,$PARTNER,$UNIT,CURRENT_DATE,$((120*M)),'active',CURRENT_DATE,1)
           RETURNING id")
echo "    unit=$UNIT contract=$CON line=$LINE"

pg "INSERT INTO rental_invoice_link (move_id,contract_id,contract_line_id,period_start,period_end,amount,company_id)
    VALUES (999001,$CON,$LINE,'2026-09-01','2026-09-30',$((120*M)),1)" >/dev/null
FIRST=$(pg "SELECT count(*) FROM rental_invoice_link WHERE contract_line_id=$LINE")
[ "$FIRST" = "1" ] && ok "first invoice link accepted" || no "first link not stored"

# The duplicate: same line, same period_start, different move. This is
# exactly what a second cron run would attempt.
ERR=$(pgE "INSERT INTO rental_invoice_link (move_id,contract_id,contract_line_id,period_start,period_end,amount,company_id)
           VALUES (999002,$CON,$LINE,'2026-09-01','2026-09-30',$((120*M)),1)")
AFTER=$(pg "SELECT count(*) FROM rental_invoice_link WHERE contract_line_id=$LINE")
echo "    duplicate attempt -> $(printf '%s' "$ERR" | head -1)"
echo "    links for this line after the attempt: $AFTER"
printf '%s' "$ERR" | grep -qi "duplicate key\|unique" \
    && ok "duplicate period REJECTED by the database" \
    || no "duplicate was not rejected — double-billing is possible"
[ "$AFTER" = "1" ] && ok "still exactly one link — the cron cannot double-bill" || no "$AFTER links exist"

# A DIFFERENT period on the same line must still be allowed, or the
# constraint would block normal monthly billing.
pg "INSERT INTO rental_invoice_link (move_id,contract_id,contract_line_id,period_start,period_end,amount,company_id)
    VALUES (999003,$CON,$LINE,'2026-10-01','2026-10-31',$((120*M)),1)" >/dev/null
NEXTM=$(pg "SELECT count(*) FROM rental_invoice_link WHERE contract_line_id=$LINE")
[ "$NEXTM" = "2" ] && ok "the NEXT period is still billable (constraint is not too broad)" \
                   || no "next period was blocked — constraint too broad"

echo
echo "############ 6. the double-let guard ############"
# A second live line on the same unit is what happens when two operators
# let the same locker concurrently. The partial unique index makes it
# impossible rather than unlikely.
CON2=$(pg "INSERT INTO rental_contract (name,partner_id,state,date_start,company_id)
           VALUES ('RTEST/2',$PARTNER,'active',CURRENT_DATE,1) RETURNING id")
ERR2=$(pgE "INSERT INTO rental_contract_line
              (contract_id,partner_id,unit_id,date_start,unit_price,state,company_id)
            VALUES ($CON2,$PARTNER,$UNIT,CURRENT_DATE,$((120*M)),'active',1)")
echo "    second live line on unit $UNIT -> $(printf '%s' "$ERR2" | head -1)"
printf '%s' "$ERR2" | grep -qi "duplicate key\|unique" \
    && ok "a unit cannot be let twice concurrently" \
    || no "the same unit was let twice — double-let is possible"

# Once the first line ENDS the unit must become lettable again, or a
# locker could never be re-let after its first tenant.
pg "UPDATE rental_contract_line SET state='ended' WHERE id=$LINE" >/dev/null
RELET=$(pg "INSERT INTO rental_contract_line
              (contract_id,partner_id,unit_id,date_start,unit_price,state,company_id)
            VALUES ($CON2,$PARTNER,$UNIT,CURRENT_DATE,$((120*M)),'active',1) RETURNING id")
[ -n "$RELET" ] && ok "unit is re-lettable once the previous line ended" \
                || no "unit could not be re-let after the line ended"

echo
echo "############ 7. recurring-expense idempotency ############"
TMPL=$(pg "INSERT INTO rental_expense (date,name,amount,is_recurring,recurrence_interval,company_id)
           VALUES (CURRENT_DATE,'RTEST template',$((300*M)),TRUE,'monthly',1) RETURNING id")
pg "INSERT INTO rental_expense (date,name,amount,recurrence_parent_id,company_id)
    VALUES ('2026-09-01','RTEST child',$((300*M)),$TMPL,1)" >/dev/null
ERR3=$(pgE "INSERT INTO rental_expense (date,name,amount,recurrence_parent_id,company_id)
            VALUES ('2026-09-01','RTEST child dup',$((300*M)),$TMPL,1)")
printf '%s' "$ERR3" | grep -qi "duplicate key\|unique" \
    && ok "a template cannot generate two expenses for the same date" \
    || no "recurring expense double-generated"

echo
echo "############ 8. event log ############"
pg "INSERT INTO rental_event (event_type,contract_id,unit_id,summary,detail,company_id)
    VALUES ('contract_activated',$CON,$UNIT,'RTEST event','{\"probe\":true}'::jsonb,1)" >/dev/null
EV=$(pg "SELECT count(*) FROM rental_event WHERE summary='RTEST event'")
DET=$(pg "SELECT detail->>'probe' FROM rental_event WHERE summary='RTEST event'")
[ "$EV" = "1" ]     && ok "event recorded"                  || no "event not recorded"
[ "$DET" = "true" ] && ok "JSONB detail round-trips"         || no "detail is '$DET'"
# The event log must be SEPARATE from audit_log — conflating them gives a
# log that is bad at both (docs/054 phase 2).
AUD=$(pg "SELECT count(*) FROM audit_log WHERE model='rental.event'")
[ "$AUD" = "0" ] && ok "rental.event is not itself audit-logged (separate concerns)" \
                 || no "$AUD audit rows for rental.event"

echo
echo "############ 9. models reachable through the API ############"
cat > /tmp/rt_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/rt_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
if [ -z "$SID" ]; then
    no "cannot authenticate — API checks skipped"
else
    for model in rental.unit.type rental.unit rental.contract rental.contract.line \
                 rental.expense.category rental.expense rental.event; do
        cat > /tmp/rt_c.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$model","method":"search_read","args":[[]],
 "kwargs":{"limit":1,"context":{"session_id":"$SID"}}}}
EOF
        R=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/rt_c.json)
        if printf '%s' "$R" | grep -q '"result"'; then ok "$model responds to search_read"
        else no "$model: $(printf '%s' "$R" | head -c 120)"; fi
    done

    # Money must come back in MAJOR units. The rate is stored as
    # 120000000 micro-units; a client that sees that number would render
    # RM 120,000,000.00 for a locker.
    cat > /tmp/rt_m.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"rental.unit.type","method":"search_read",
 "args":[[["code","=","SL"]]],
 "kwargs":{"fields":["code","default_rate"],"context":{"session_id":"$SID"}}}}
EOF
    RM=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/rt_m.json)
    echo "    $RM"
    printf '%s' "$RM" | grep -q '"default_rate":120.0' \
        && ok "API reports 120.0, not the raw micro-unit integer" \
        || no "money conversion wrong on the way out"

    # A field the UI writes must be registered, or write() drops it in
    # silence — the tax_ids_json defect in docs/053.
    cat > /tmp/rt_w.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"rental.unit","method":"write",
 "args":[[$UNIT],{"zone":"Z9","notes":"RTEST note"}],
 "kwargs":{"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/rt_w.json > /dev/null
    Z=$(pg "SELECT zone FROM rental_unit WHERE id=$UNIT")
    [ "$Z" = "Z9" ] && ok "write() persists registered fields" || no "zone is '$Z' — field dropped on write"
fi

echo
echo "############ cleanup ############"
cleanup
LEFT=$(pg "SELECT count(*) FROM rental_contract WHERE name LIKE 'RTEST/%'")
echo "    probe rows remaining: $LEFT"
[ "$LEFT" = "0" ] && ok "test data removed" || no "$LEFT rows leaked"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

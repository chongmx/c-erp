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
# Recurring expenses + cashflow forecast.
#
# The forecast's failure mode is not an error — it is a NUMBER THAT LOOKS
# PLAUSIBLE AND IS WRONG. So every assertion here is against an amount
# worked out by hand, never "greater than zero".
#
# The two ways a forecast of this shape goes wrong:
#   * double counting — an invoice already raised counted BOTH as an open
#     receivable and as projected income for the same month
#   * a recurring template counted as well as the children generated from
#     it, so budgeted expense appears twice
# Both are asserted explicitly.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
M=1000000

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

# The /rental/ routes authenticate (docs/061).
CK=/tmp/vrc_cookie.txt
cat > /tmp/vrc_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/vrc_auth.json > /tmp/vrc_auth_out.json
grep -q '"session_id"' /tmp/vrc_auth_out.json || { echo "cannot authenticate"; exit 1; }

# month field from the forecast JSON: $1=month $2=field
cf() { cf_from 2026-09-01 "$1" "$2"; }

# $1=window start  $2=month  $3=field
cf_from() {
    curl -s -b /tmp/vrc_cookie.txt "$BASE/rental/cashflow?months=6&from=$1" \
    | python3 -c "
import json,sys
d = json.load(sys.stdin)
for r in d['series']:
    if r['month'] == sys.argv[1]:
        print(r[sys.argv[2]]); break
else:
    print('NOMONTH')
" "$2" "$3"
}

# Baselines, captured before this suite creates anything. Every assertion
# below is a DELTA against these, so the suite is independent of whatever
# else the database already holds.
declare -A BASE0
snapshot() {
    local wnd=$1
    for mth in "${@:2}"; do
        for fld in budgeted_expense committed_expense projected_income receivable income; do
            BASE0["$wnd|$mth|$fld"]=$(cf_from "$wnd" "$mth" "$fld")
        done
    done
}
delta_from() {   # window month field -> value now minus baseline
    local now; now=$(cf_from "$1" "$2" "$3")
    python3 -c "print(round(float('$now') - float('${BASE0["$1|$2|$3"]:-0}'), 2))"
}
delta() { delta_from 2026-09-01 "$1" "$2"; }

snapshot 2026-09-01 2026-09 2026-10 2026-11 2026-12
snapshot 2026-06-01 2026-06

cleanup() {
    # Events first, and by SUMMARY not partner_id: an expense-generated
    # event has no partner, so filtering on partner_id left them behind
    # to accumulate in the dashboard's activity feed.
    pg "DELETE FROM rental_event WHERE summary LIKE 'CFTEST%'" >/dev/null
    pg "DELETE FROM rental_expense WHERE name LIKE 'CFTEST%'" >/dev/null
    pg "DELETE FROM rental_invoice_link WHERE contract_line_id IN
          (SELECT id FROM rental_contract_line WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE 'CFTEST%'))" >/dev/null
    pg "DELETE FROM account_move_line WHERE move_id IN
          (SELECT id FROM account_move WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE 'CFTEST%'))" >/dev/null
    pg "DELETE FROM account_move WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'CFTEST%')" >/dev/null
    pg "DELETE FROM rental_contract_line WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'CFTEST%')" >/dev/null
    pg "DELETE FROM rental_event WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'CFTEST%')" >/dev/null
    pg "DELETE FROM rental_unit WHERE code LIKE 'CF-%'" >/dev/null
    pg "DELETE FROM res_partner WHERE name LIKE 'CFTEST%'" >/dev/null
}
cleanup

echo "############ 1. recurring expense generation ############"
CAT=$(pg "SELECT id FROM rental_expense_category WHERE name='Utilities' LIMIT 1")
# Wifi RM 200/month, starting 3 months in the past — the cron has been
# "down", and catching up is the behaviour that matters.
TMPL=$(pg "INSERT INTO rental_expense
             (date,name,category_id,amount,is_recurring,recurrence_interval,
              recurrence_next_date,company_id,state)
           VALUES ('2026-06-01','CFTEST Wifi',$CAT,$((200*M)),TRUE,'monthly',
                   '2026-06-01',1,'draft') RETURNING id")
echo "    template $TMPL: RM 200/month from 2026-06-01"

R=$(curl -s -b /tmp/vrc_cookie.txt -X POST "$BASE/rental/expenses/generate?date=2026-08-31")
echo "    generate as of 2026-08-31 -> $R"
KIDS=$(pg "SELECT count(*) FROM rental_expense WHERE recurrence_parent_id=$TMPL")
SUM=$(pg "SELECT COALESCE(SUM(amount),0) FROM rental_expense WHERE recurrence_parent_id=$TMPL")
echo "    children=$KIDS total=$SUM"
[ "$KIDS" = "3" ]           && ok "caught up 3 months (Jun, Jul, Aug) — a down cron loses nothing" \
                            || no "expected 3 children, got $KIDS"
[ "$SUM" = "$((600*M))" ]   && ok "3 x RM 200 = RM 600"  || no "total is $SUM"
NEXT=$(pg "SELECT to_char(recurrence_next_date,'YYYY-MM-DD') FROM rental_expense WHERE id=$TMPL")
[ "$NEXT" = "2026-09-01" ]  && ok "template advanced to 2026-09-01" || no "next date is $NEXT"

echo
echo "############ 2. re-running generates nothing ############"
R=$(curl -s -b /tmp/vrc_cookie.txt -X POST "$BASE/rental/expenses/generate?date=2026-08-31")
K2=$(pg "SELECT count(*) FROM rental_expense WHERE recurrence_parent_id=$TMPL")
echo "    second run -> $R   children still $K2"
[ "$K2" = "3" ] && ok "idempotent — no duplicate expenses" || no "children grew to $K2"

echo
echo "############ 3. NEGATIVE CONTROL — the index is what forbids a duplicate ############"
# NOT by re-running the generator with the index dropped: ON CONFLICT
# INFERS that index, so without it the statement errors instead of
# duplicating, and the run would look "safe" for the wrong reason.
#
# The honest test is a raw INSERT — the shape a concurrent generator, a
# repair script or a future code path would take — attempted with the
# index absent and again with it present.
DUP="INSERT INTO rental_expense (date,name,amount,recurrence_parent_id,company_id,state,is_recurring)
     VALUES ('2026-06-01','CFTEST dup',$((200*M)),$TMPL,1,'draft',FALSE)"

pg "DROP INDEX IF EXISTS rental_expense_recurrence_uniq" >/dev/null
pg "$DUP" >/dev/null
K3=$(pg "SELECT count(*) FROM rental_expense WHERE recurrence_parent_id=$TMPL")
echo "    index dropped, raw duplicate insert -> children=$K3"
[ "$K3" -gt 3 ] && ok "WITHOUT the index a duplicate IS accepted — the guard is load-bearing" \
                || no "duplicate rejected with no index; the guard is not what prevents it"

pg "DELETE FROM rental_expense WHERE name='CFTEST dup'" >/dev/null
pg "CREATE UNIQUE INDEX IF NOT EXISTS rental_expense_recurrence_uniq
      ON rental_expense(recurrence_parent_id, date)
      WHERE recurrence_parent_id IS NOT NULL" >/dev/null
IDX=$(pg "SELECT count(*) FROM pg_indexes WHERE indexname='rental_expense_recurrence_uniq'")
[ "$IDX" = "1" ] && ok "index restored" || no "INDEX NOT RESTORED — fix before deploying"

ERR=$(PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$DUP" 2>&1 | head -1)
K4=$(pg "SELECT count(*) FROM rental_expense WHERE recurrence_parent_id=$TMPL")
echo "    index restored, same insert -> $(printf '%s' "$ERR" | head -c 70)"
printf '%s' "$ERR" | grep -qi "duplicate key\|unique" \
    && ok "WITH the index the same insert is rejected" \
    || no "duplicate accepted even with the index"
[ "$K4" = "3" ] && ok "still exactly 3 children" || no "$K4 children"

echo
echo "############ 4. forecast: budgeted expense appears in future months ############"
# DELTAS, not absolute totals. The forecast is a global aggregate, so any
# other data in the database — a seeded demo facility, another test's
# leftovers — lands in the same figure. Asserting absolutes made this
# suite pass only on an empty database, and it broke the moment the demo
# seed gained recurring expenses.
SEP=$(delta 2026-09 budgeted_expense)
OCT=$(delta 2026-10 budgeted_expense)
echo "    budgeted expense delta  Sep=$SEP Oct=$OCT  (baseline excluded)"
[ "$SEP" = "200.0" ] && ok "September gained exactly the RM 200 wifi budget" || no "Sep delta is $SEP"
[ "$OCT" = "200.0" ] && ok "October too — it recurs"                         || no "Oct delta is $OCT"

echo
echo "############ 5. the generated children are NOT double counted ############"
# The three generated children are dated Jun-Aug, outside the window, so
# they must not appear there. If templates and children were both counted,
# the delta would be 400 rather than 200.
[ "$SEP" = "200.0" ] && ok "template counted once, not once per child" \
                     || no "Sep delta $SEP — template and children both counted"
JUN=$(delta_from 2026-06-01 2026-06 committed_expense)
echo "    June committed (actual) expense delta: $JUN"
[ "$JUN" = "200.0" ] && ok "a generated child counts as a COMMITTED expense, once" || no "June delta is $JUN"

echo
echo "############ 6. forecast: projected rental income ############"
P1=$(pg "INSERT INTO res_partner (name,is_company,active) VALUES ('CFTEST Cust',false,true) RETURNING id")
U1=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('CF-01','cf','available',1) RETURNING id")
U2=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('CF-02','cf','available',1) RETURNING id")
pg "INSERT INTO rental_contract_line
      (partner_id,unit_id,date_start,unit_price,tax_ids_json,state,billing_mode,
       billing_anchor_day,billing_months,billing_lead_days,next_period_start,company_id)
    VALUES ($P1,$U1,'2026-09-01',$((300*M)),'[]','active','recurring',1,1,7,'2026-09-01',1)" >/dev/null
# A quarterly line: must appear in Sep and Dec, not Oct or Nov.
pg "INSERT INTO rental_contract_line
      (partner_id,unit_id,date_start,unit_price,tax_ids_json,state,billing_mode,
       billing_anchor_day,billing_months,billing_lead_days,next_period_start,company_id)
    VALUES ($P1,$U2,'2026-09-01',$((900*M)),'[]','active','recurring',1,3,7,'2026-09-01',1)" >/dev/null

S=$(delta 2026-09 projected_income); O=$(delta 2026-10 projected_income)
N=$(delta 2026-11 projected_income); D=$(delta 2026-12 projected_income)
echo "    projected income delta  Sep=$S Oct=$O Nov=$N Dec=$D"
[ "$S" = "1200.0" ] && ok "Sep = 300 monthly + 900 quarterly"       || no "Sep delta is $S"
[ "$O" = "300.0" ]  && ok "Oct = monthly only"                       || no "Oct delta is $O"
[ "$N" = "300.0" ]  && ok "Nov = monthly only"                       || no "Nov delta is $N"
[ "$D" = "1200.0" ] && ok "Dec = the quarterly line returns"          || no "Dec delta is $D"

echo
echo "############ 7. a walk-in is NOT projected ############"
U3=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('CF-03','cf','available',1) RETURNING id")
pg "INSERT INTO rental_contract_line
      (partner_id,unit_id,date_start,unit_price,tax_ids_json,state,billing_mode,
       billing_anchor_day,billing_months,billing_lead_days,next_period_start,company_id)
    VALUES ($P1,$U3,'2026-09-01',$((500*M)),'[]','active','manual',1,1,7,'2026-09-01',1)" >/dev/null
S2=$(delta 2026-09 projected_income)
echo "    after adding a RM 500 walk-in: Sep delta=$S2 (unchanged from $S)"
[ "$S2" = "$S" ] && ok "walk-in excluded — it cannot be forecast" || no "Sep delta became $S2"

echo
echo "############ 8. an invoiced period is a RECEIVABLE, not projected twice ############"
BEFORE=$(delta 2026-09 projected_income)
INCOME_BEFORE=$(delta 2026-09 income)
curl -s -b /tmp/vrc_cookie.txt -X POST "$BASE/rental/billing/run?date=2026-08-26" > /dev/null
AFTER=$(delta 2026-09 projected_income)
RECV=$(delta 2026-09 receivable)
INCOME=$(delta 2026-09 income)
echo "    projected delta $BEFORE -> $AFTER   receivable delta=$RECV   income delta=$INCOME"
[ "$AFTER" != "$BEFORE" ] && ok "billing moved the amount out of 'projected'" \
                          || no "projected unchanged after billing — likely double counted"
# The total for the month must NOT have doubled: what moved out of
# projected must reappear once as a receivable.
python3 - "$INCOME_BEFORE" "$INCOME" <<'PY'
import sys
before, after = float(sys.argv[1]), float(sys.argv[2])
if abs(after - before) < 0.01:
    print("    PASS  September income unchanged in total — no double count")
else:
    print(f"    FAIL  income delta moved {before} -> {after}; billing double counted")
    sys.exit(1)
PY
[ $? -eq 0 ] || FAILED=1

echo
echo "############ 9. net and cumulative add up ############"
# Fetched with curl, not urllib.urlopen: the route authenticates now and
# urlopen carries no cookie jar.
#
# The checker goes to a FILE rather than `python3 - <<PY`, because that
# form reads the SCRIPT from stdin and so cannot also receive the piped
# JSON — the two uses of stdin collide and python sees an empty document.
cat > /tmp/vrc_check.py <<'PY'
import json, sys
d = json.load(sys.stdin)
bad = 0
run = 0.0
for r in d['series']:
    if abs((r['income'] - r['expense']) - r['net']) > 0.005:
        print(f"    FAIL  {r['month']}: net != income - expense"); bad += 1
    run = round(run + r['net'], 2)
    if abs(run - r['cumulative']) > 0.005:
        print(f"    FAIL  {r['month']}: cumulative {r['cumulative']} != running {run}"); bad += 1
if not bad:
    print("    PASS  every month: net = income - expense")
    print("    PASS  cumulative is the running total of net")
sys.exit(1 if bad else 0)
PY
curl -s -b "$CK" "$BASE/rental/cashflow?months=6&from=2026-09-01" \
  | python3 /tmp/vrc_check.py
[ $? -eq 0 ] || FAILED=1
rm -f /tmp/vrc_check.py

echo
echo "############ 10. assumptions are stated ############"
A=$(curl -s -b /tmp/vrc_cookie.txt "$BASE/rental/cashflow?months=1" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['assumptions']))")
echo "    assumptions returned: $A"
[ "$A" -ge 4 ] && ok "the forecast states its assumptions rather than implying them" \
               || no "only $A assumptions"

echo
echo "############ cleanup ############"
cleanup
LEFT=$(pg "SELECT count(*) FROM rental_expense WHERE name LIKE 'CFTEST%'")
[ "$LEFT" = "0" ] && ok "test data removed" || no "$LEFT rows leaked"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

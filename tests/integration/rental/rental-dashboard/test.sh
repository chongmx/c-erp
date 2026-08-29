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
# Rental dashboard — /rental/dashboard and the OWL panel.
#
# Two classes of failure worth catching:
#
#   * the endpoint and the panel disagree — the KPI tile says one thing
#     and the chart beside it says another, which destroys trust in both
#   * a number is plausible but wrong (MRR counting a quarterly line at
#     three times its monthly worth, occupancy counting retired units)
#
# So every figure is asserted against one computed independently in SQL,
# never "greater than zero".
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
M=1000000

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

# The /rental/ routes authenticate (docs/061).
CK=/tmp/vrd2_cookie.txt
cat > /tmp/vrd2_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/vrd2_auth.json > /tmp/vrd2_auth_out.json
grep -q '"session_id"' /tmp/vrd2_auth_out.json || { echo "cannot authenticate"; exit 1; }

# $1 = python expression over the payload `d`
dash() {
    curl -s -b "$CK" "$BASE/rental/dashboard?months=6&fresh=1" \
    | python3 -c "import json,sys; d=json.load(sys.stdin); print($1)" 2>/dev/null
}

cleanup() {
    pg "DELETE FROM rental_event WHERE summary LIKE 'DBTEST%'" >/dev/null
    pg "DELETE FROM rental_expense WHERE name LIKE 'DBTEST%'" >/dev/null
    pg "DELETE FROM rental_contract_line WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'DBTEST%')" >/dev/null
    pg "DELETE FROM rental_unit WHERE code LIKE 'DB-%'" >/dev/null
    pg "DELETE FROM res_partner WHERE name LIKE 'DBTEST%'" >/dev/null
}
cleanup

echo "############ 1. the endpoint responds with every panel ############"
CODE=$(curl -s -o /dev/null -w '%{http_code}' -b "$CK" "$BASE/rental/dashboard?months=6")
echo "    HTTP $CODE"
[ "$CODE" = "200" ] && ok "dashboard endpoint responds" || no "returned $CODE"
for k in occupancy mrr receivables ageing attention activity cashflow noi_month; do
    HAS=$(dash "'$k' in d")
    [ "$HAS" = "True" ] && ok "payload carries '$k'" || no "'$k' missing"
done
# ONE endpoint, not N calls (docs/040 §3.4) — so it must be cached.
CACHED=$(dash "d.get('cached_seconds',0)")
[ "$CACHED" = "60" ] && ok "response is cached (60 s)" || no "cached_seconds is $CACHED"

echo
echo "############ 2. occupancy excludes retired from the denominator ############"
P=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
U1=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('DB-01','x','occupied',1) RETURNING id")
U2=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('DB-02','x','available',1) RETURNING id")
U3=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('DB-03','x','retired',1) RETURNING id")

API_LET=$(dash "d['occupancy']['lettable']")
SQL_LET=$(pg "SELECT count(*) FROM rental_unit WHERE active AND state <> 'retired'")
API_OCC=$(dash "d['occupancy']['occupied']")
SQL_OCC=$(pg "SELECT count(*) FROM rental_unit WHERE active AND state = 'occupied'")
API_PCT=$(dash "d['occupancy']['pct']")
echo "    lettable api=$API_LET sql=$SQL_LET   occupied api=$API_OCC sql=$SQL_OCC   pct=$API_PCT"
[ "$API_LET" = "$SQL_LET" ] && ok "lettable matches SQL (retired excluded)" || no "api $API_LET vs sql $SQL_LET"
[ "$API_OCC" = "$SQL_OCC" ] && ok "occupied matches SQL"                    || no "api $API_OCC vs sql $SQL_OCC"

# The retired unit must not be in the denominator: adding one cannot
# lower occupancy, or a decommissioned locker depresses the number
# permanently.
BEFORE=$(dash "d['occupancy']['pct']")
pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('DB-04','x','retired',1)" >/dev/null
AFTER=$(dash "d['occupancy']['pct']")
echo "    occupancy before=$BEFORE after adding a retired unit=$AFTER"
[ "$BEFORE" = "$AFTER" ] && ok "a retired unit does not move occupancy" \
                         || no "occupancy changed $BEFORE -> $AFTER"

echo
echo "############ 3. MRR normalises the billing interval ############"
# A quarterly line at 900 is 300/month. Counting it whole would inflate
# the one number most likely to be quoted.
PT=$(pg "INSERT INTO res_partner (name,is_company,active) VALUES ('DBTEST Cust',false,true) RETURNING id")
UM=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('DB-10','m','available',1) RETURNING id")
UQ=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('DB-11','q','available',1) RETURNING id")
BASE_MRR=$(dash "d['mrr']")
pg "INSERT INTO rental_contract_line
      (partner_id,unit_id,date_start,unit_price,tax_ids_json,state,billing_mode,
       billing_anchor_day,billing_months,billing_lead_days,next_period_start,company_id)
    VALUES ($PT,$UM,'2026-09-01',$((100*M)),'[]','active','recurring',1,1,7,'2026-09-01',1)" >/dev/null
pg "INSERT INTO rental_contract_line
      (partner_id,unit_id,date_start,unit_price,tax_ids_json,state,billing_mode,
       billing_anchor_day,billing_months,billing_lead_days,next_period_start,company_id)
    VALUES ($PT,$UQ,'2026-09-01',$((900*M)),'[]','active','recurring',1,3,7,'2026-09-01',1)" >/dev/null
NEW_MRR=$(dash "d['mrr']")
DELTA=$(python3 -c "print(round($NEW_MRR - $BASE_MRR, 2))")
echo "    MRR $BASE_MRR -> $NEW_MRR   delta=$DELTA (expect 400.00 = 100 + 900/3)"
[ "$DELTA" = "400.0" ] && ok "quarterly line counted at a THIRD of its amount" \
                       || no "delta is $DELTA — interval not normalised"

echo
echo "############ 4. a walk-in is excluded from MRR ############"
UW=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('DB-12','w','available',1) RETURNING id")
pg "INSERT INTO rental_contract_line
      (partner_id,unit_id,date_start,unit_price,tax_ids_json,state,billing_mode,
       billing_anchor_day,billing_months,billing_lead_days,next_period_start,company_id)
    VALUES ($PT,$UW,'2026-09-01',$((500*M)),'[]','active','manual',1,1,7,'2026-09-01',1)" >/dev/null
W_MRR=$(dash "d['mrr']")
echo "    after adding a RM 500 walk-in: MRR=$W_MRR"
[ "$W_MRR" = "$NEW_MRR" ] && ok "walk-in excluded — MRR means RECURRING revenue" \
                          || no "MRR moved to $W_MRR"

echo
echo "############ 5. the KPI tile and the chart tell the same story ############"
# The KPI reads the first month of the same series the chart draws, so
# they cannot disagree. Asserted because two panels contradicting each
# other destroys trust in both.
NOI=$(dash "d['noi_month']")
FIRST=$(dash "d['cashflow']['series'][0]['net']")
echo "    noi_month=$NOI   cashflow.series[0].net=$FIRST"
[ "$NOI" = "$FIRST" ] && ok "the Net tile is the first month of the chart" \
                      || no "$NOI vs $FIRST — the panels disagree"

OUT_API=$(dash "d['receivables']['outstanding']")
OUT_SQL=$(pg "SELECT COALESCE(SUM(amount_residual),0)/1000000.0 FROM account_move
               WHERE move_type='out_invoice' AND state='posted' AND amount_residual>0")
echo "    outstanding api=$OUT_API sql=$OUT_SQL"
python3 -c "
import sys
a,b = float('$OUT_API'), float('$OUT_SQL' or 0)
sys.exit(0 if abs(a-b) < 0.01 else 1)" \
    && ok "outstanding matches an independent SQL total" \
    || no "api $OUT_API vs sql $OUT_SQL"

echo
echo "############ 6. ageing buckets are ordered and total correctly ############"
SUM_B=$(dash "round(sum(d['ageing'].values()), 2)")
echo "    ageing buckets sum=$SUM_B  outstanding=$OUT_API"
python3 -c "
import sys
a,b = float('$SUM_B'), float('$OUT_API')
sys.exit(0 if abs(a-b) < 0.01 else 1)" \
    && ok "the buckets partition the outstanding total exactly" \
    || no "buckets sum $SUM_B != outstanding $OUT_API"
for b in current d0_30 d31_60 d61_90 d90_plus; do
    HAS=$(dash "'$b' in d['ageing']")
    [ "$HAS" = "True" ] || no "bucket '$b' missing"
done
ok "all five buckets present, including empty ones"

echo
echo "############ 7. the panel is registered and served ############"
for p in /src/components/rental/RentalDashboard.js /src/components/rental/rental.css; do
    c=$(curl -s -o /dev/null -w '%{http_code}' "$BASE$p")
    printf '    %-48s %s\n' "$p" "$c"
    [ "$c" = "200" ] && ok "served" || no "$p returned $c"
done
HTML=$(curl -s "$BASE/index.html")
D_LINE=$(printf '%s' "$HTML" | grep -n 'RentalDashboard.js' | head -1 | cut -d: -f1)
A_LINE=$(printf '%s' "$HTML" | grep -n 'src/app.js' | head -1 | cut -d: -f1)
[ -n "$D_LINE" ] && [ "$D_LINE" -lt "$A_LINE" ] \
    && ok "loaded before app.js, which references it in CUSTOM_VIEWS" \
    || no "load order wrong — app.js would throw ReferenceError"
curl -s "$BASE/src/app.js" | grep -q "'rental.dashboard'" \
    && ok "rental.dashboard registered in CUSTOM_VIEWS" || no "not registered"
ACT=$(pg "SELECT count(*) FROM ir_act_window WHERE res_model='rental.dashboard'")
MEN=$(pg "SELECT count(*) FROM ir_ui_menu WHERE name='Dashboard' AND parent_id=310")
[ "$ACT" = "1" ] && ok "action seeded" || no "no action for rental.dashboard"
[ "$MEN" = "1" ] && ok "menu entry seeded" || no "no Dashboard menu item"

echo
echo "############ 8. chart rules from docs/046 §9 ############"
JS=$(curl -s "$BASE/src/components/rental/RentalDashboard.js")
CSS=$(curl -s "$BASE/src/components/rental/rental.css")
# Income and expense are FLOWS; cumulative is a STOCK. Putting them on
# one scale is the dual-axis mistake in disguise, so cumulative must be
# a separate VIEW rather than a third series.
printf '%s' "$JS" | grep -q "cfView === 'cumulative'" \
    && ok "cumulative is a separate view, not a third series" || no "no separate cumulative view"
printf '%s' "$JS" | grep -q "cfView === 'table'" \
    && ok "a table view is available (accessible alternative)" || no "no table view"
printf '%s' "$CSS" | grep -q -- "--s-income" && printf '%s' "$CSS" | grep -q -- "--s-expense" \
    && ok "series colours are named tokens from the validated palette" || no "series tokens missing"
# Status colours are reserved and must never become series colours.
printf '%s' "$JS" | grep -q "var(--s-income)" && printf '%s' "$JS" | grep -q "var(--s-expense)" \
    && ok "the chart uses series slots, not status colours" || no "chart does not use series tokens"
# The app shell is dark and has NO theme toggle, so the panels must
# inherit its palette rather than carry a second opinion. Defaulting to
# light and switching on prefers-color-scheme put white panels inside a
# dark navy app on every light-preferring OS.
printf '%s' "$CSS" | grep -q 'var(--bg,' && printf '%s' "$CSS" | grep -q 'var(--surface,' \
    && ok "surfaces inherited from app.css, not hard-coded" \
    || no "panels do not inherit the shell palette"
# An actual @media RULE, not the phrase appearing in a comment — the
# first version of this check matched its own explanatory comment and
# reported a failure that did not exist.
printf '%s' "$CSS" | grep -qE '@media[^{]*prefers-color-scheme' \
    && no "still keyed to the OS preference — the app is dark regardless" \
    || ok "not keyed to the OS preference"
printf '%s' "$CSS" | grep -q 'data-theme="light"' \
    && ok "an explicit light variant exists for previews / a future theme" \
    || no "no light variant"
# The ordinal ramp must be themeable, because its DIRECTION flips: on a
# dark surface the most overdue bucket has to be the brightest, or the
# one needing action is the least visible.
printf '%s' "$CSS" | grep -q -- '--age-90-plus' \
    && ok "ageing ramp is tokenised so it can flip with the theme" \
    || no "ageing colours are hard-coded"
printf '%s' "$JS" | grep -q 'var(--age-90-plus)' \
    && ok "the panel reads the ramp from CSS, not literals" \
    || no "the panel hard-codes ageing colours"
printf '%s' "$CSS" | grep -q "tabular-nums" \
    && ok "tabular figures in tables and axes" || no "no tabular-nums"

echo
echo "############ cleanup ############"
cleanup
LEFT=$(pg "SELECT count(*) FROM rental_unit WHERE code LIKE 'DB-%'")
[ "$LEFT" = "0" ] && ok "test data removed" || no "$LEFT units leaked"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

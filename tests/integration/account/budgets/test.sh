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
# Budgets (docs/085): budgetary positions, budgets, planned vs actual.
#
# The load-bearing assertion is that the "actual" column is the REAL ledger
# figure — a budget whose position covers an expense account must pick up
# exactly the posted expense for the budget period, and nothing outside it.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }
CTX="\"context\":{\"session_id\":\"$SID\"}"
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":$4}}"; }
rid(){ sed -n 's/.*"result":\([0-9]*\).*/\1/p'; }

EXP=$(pg "SELECT id FROM account_account WHERE account_type='expense' ORDER BY id LIMIT 1")
JRN=$(pg "SELECT id FROM account_journal ORDER BY id LIMIT 1")
CASH=$(pg "SELECT id FROM account_account WHERE account_type='asset_cash' ORDER BY id LIMIT 1")
[ -z "$CASH" ] && CASH=$(pg "SELECT id FROM account_account WHERE account_type LIKE 'asset%' ORDER BY id LIMIT 1")

echo "############ budgetary position + budget ############"
POST=$(call account.budget.post create "[{\"name\":\"Operating Expenses\",\"account_ids_json\":\"[$EXP]\"}]" "{$CTX}" | rid)
[ -n "$POST" ] && ok "budgetary position created ($POST)" || no "position create failed"
BUD=$(call account.budget create "[{\"name\":\"FY2026 Operating Budget\",\"date_from\":\"2026-01-01\",\"date_to\":\"2026-12-31\"}]" "{$CTX}" | rid)
[ -n "$BUD" ] && ok "budget created ($BUD)" || { no "budget create failed"; echo "*** FAILURES ***"; exit 1; }
LINE=$(call account.budget.line create "[{\"budget_id\":$BUD,\"post_id\":$POST,\"planned_amount\":50000}]" "{$CTX}" | rid)
[ -n "$LINE" ] && ok "budget line created (planned RM50,000)" || no "line create failed"

echo "############ post RM1,200 of expense inside the period ############"
# Repeatable: drop any entries left by a previous run before measuring.
pg "DELETE FROM account_move_line WHERE move_id IN (SELECT id FROM account_move WHERE name LIKE 'BUDGET-TEST%')" >/dev/null
pg "DELETE FROM account_move WHERE name LIKE 'BUDGET-TEST%'" >/dev/null
BEFORE=$(pg "SELECT COALESCE(SUM(debit-credit),0) FROM account_move_line l JOIN account_move m ON m.id=l.move_id WHERE m.state='posted' AND l.account_id=$EXP AND l.date BETWEEN '2026-01-01' AND '2026-12-31'")
# NOTE: psql -tAc on "INSERT … RETURNING id" prints the id AND the "INSERT 0 1"
# command tag, so the id must be taken from the FIRST line only.
MV=$(pg "INSERT INTO account_move (name,move_type,state,date,journal_id,company_id,amount_total) VALUES ('BUDGET-TEST','entry','posted','2026-03-15',$JRN,1,1200000000) RETURNING id" | head -1)
pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,debit,credit) VALUES ($MV,$EXP,$JRN,1,'2026-03-15','Budget test expense',1200000000,0), ($MV,$CASH,$JRN,1,'2026-03-15','Budget test expense',0,1200000000)" >/dev/null
# also post one OUTSIDE the period — the budget must ignore it
MV2=$(pg "INSERT INTO account_move (name,move_type,state,date,journal_id,company_id,amount_total) VALUES ('BUDGET-TEST-OUT','entry','posted','2025-03-15',$JRN,1,999000000) RETURNING id" | head -1)
pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,debit,credit) VALUES ($MV2,$EXP,$JRN,1,'2025-03-15','Outside period',999000000,0), ($MV2,$CASH,$JRN,1,'2025-03-15','Outside period',0,999000000)" >/dev/null

echo "############ compute actuals from the ledger ############"
call account.budget action_compute "[[$BUD]]" "{$CTX}" >/dev/null
ACTUAL=$(pg "SELECT practical_amount FROM account_budget_line WHERE id=$LINE")
EXPECT=$(pg "SELECT COALESCE(SUM(debit-credit),0) FROM account_move_line l JOIN account_move m ON m.id=l.move_id WHERE m.state='posted' AND l.account_id=$EXP AND l.date BETWEEN '2026-01-01' AND '2026-12-31'")
[ "$ACTUAL" = "$EXPECT" ] && ok "actual matches the ledger for the period ($ACTUAL)" || no "actual $ACTUAL != ledger $EXPECT"
INCR=$(( ${ACTUAL:-0} - ${BEFORE:-0} ))
[ "$INCR" = "1200000000" ] && ok "the RM1,200 posted inside the period is included" || no "expected +1200000000, got +$INCR"
echo "$EXPECT" | grep -q "999000000" && no "an out-of-period entry leaked into the budget" || ok "the out-of-period RM999 entry is excluded"

echo "############ workflow ############"
call account.budget action_confirm "[[$BUD]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT state FROM account_budget WHERE id=$BUD")" = "confirm" ] && ok "budget confirms" || no "confirm failed"
call account.budget action_done "[[$BUD]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT state FROM account_budget WHERE id=$BUD")" = "done" ] && ok "budget marks done" || no "done failed"

echo "############ menus ############"
# Assert by menu NAME, not by a hardcoded id: menu ids move when a collision is
# repaired (Budgetary Positions moved 32 → 64 when id 32 was returned to the
# Settings ▸ Companies menu it belongs to — docs/089).
[ "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Budgets' LIMIT 1")" = "account.budget" ] \
    && ok "Accounting -> Budgets menu wired" || no "Budgets menu missing"
[ "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Budgetary Positions' LIMIT 1")" = "account.budget.post" ] \
    && ok "Configuration -> Budgetary Positions wired" || no "Positions menu missing"

echo "############ housekeeping ############"
# This script used to leave its budget behind on every run — 21 identical
# "FY2026 Operating Budget" rows had piled up in the dev database (docs/092).
# The BUDGET-TEST journal entries go too: they are posted, so leaving them
# would quietly skew the trial balance and every report built on it.
pg "DELETE FROM account_budget_line WHERE budget_id IN (SELECT id FROM account_budget WHERE name LIKE 'FY2026 Operating Budget%')" >/dev/null
pg "DELETE FROM account_budget WHERE name LIKE 'FY2026 Operating Budget%'" >/dev/null
pg "DELETE FROM account_budget_post WHERE name = 'Operating Expenses' AND NOT EXISTS (SELECT 1 FROM account_budget_line l WHERE l.post_id = account_budget_post.id)" >/dev/null
pg "DELETE FROM account_move_line WHERE move_id IN (SELECT id FROM account_move WHERE name LIKE 'BUDGET-TEST%')" >/dev/null
pg "DELETE FROM account_move WHERE name LIKE 'BUDGET-TEST%'" >/dev/null
[ "$(pg "SELECT count(*) FROM account_budget WHERE name LIKE 'FY2026 Operating Budget%'")" = "0" ] \
    && ok "test budgets and their journal entries removed" || no "test budgets left behind"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

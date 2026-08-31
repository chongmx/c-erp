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
# Employee expenses — hr.expense / hr.expense.sheet (docs/090).
#
# Draft -> Submitted -> Approved -> Posted -> Paid, with the posting step
# producing a real, balanced journal entry.
#
# The load-bearing assertions:
#   * the workflow refuses illegal transitions (a stale browser tab must not
#     be able to approve twice, or post something nobody approved);
#   * the journal entry BALANCES and debits the expense account chosen on
#     each line;
#   * SST is included in the expense, not booked to a recoverable input-tax
#     account — Malaysian SST is not a VAT and cannot be reclaimed.
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

echo "############ menus ############"
[ "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Employee Expenses' LIMIT 1")" = "hr.expense" ] \
    && ok "Employees -> Employee Expenses wired" || no "Employee Expenses menu missing"
[ "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Expense Reports' LIMIT 1")" = "hr.expense.sheet" ] \
    && ok "Accounting -> Expense Reports wired" || no "Expense Reports menu missing"
[ "$(pg "SELECT p.name FROM ir_ui_menu m JOIN ir_ui_menu p ON p.id=m.parent_id WHERE m.name='Expense Reports' LIMIT 1")" = "Accounting" ] \
    && ok "Expense Reports sits under Accounting" || no "Expense Reports is in the wrong app"

echo "############ fixture ############"
pg "DELETE FROM hr_expense WHERE name LIKE 'QA-EXP%'" >/dev/null
pg "DELETE FROM hr_expense_sheet WHERE name LIKE 'QA Expense%'" >/dev/null
EMP=$(pg "SELECT id FROM hr_employee ORDER BY id LIMIT 1")
if [ -z "$EMP" ]; then
    EMP=$(call hr.employee create "[{\"name\":\"QA Expense Claimant\"}]" "{$CTX}" | rid)
fi
[ -n "$EMP" ] && ok "employee ready ($EMP)" || { no "no employee"; echo "*** FAILURES ***"; exit 1; }
EXPACC=$(pg "SELECT id FROM account_account WHERE account_type='expense' ORDER BY id LIMIT 1")
PAYACC=$(pg "SELECT id FROM account_account WHERE account_type='liability_payable' ORDER BY id LIMIT 1")
[ -n "$EXPACC" ] && [ -n "$PAYACC" ] && ok "expense + payable accounts found ($EXPACC / $PAYACC)" \
    || { no "chart of accounts incomplete"; echo "*** FAILURES ***"; exit 1; }

echo "############ report + expenses ############"
SH=$(call hr.expense.sheet create "[{\"name\":\"QA Expense Report\",\"employee_id\":$EMP,\"date\":\"2026-04-10\",\"payment_mode\":\"own_account\"}]" "{$CTX}" | rid)
[ -n "$SH" ] && ok "expense report created ($SH)" || { no "sheet create failed"; echo "*** FAILURES ***"; exit 1; }
# A 6% SST tax, created if the chart has none — the tax treatment is the point.
# account_tax.amount is a plain percentage, not micros.
TAX=$(pg "SELECT id FROM account_tax WHERE amount=6 AND COALESCE(price_include,false)=false ORDER BY id LIMIT 1")
[ -z "$TAX" ] && TAX=$(call account.tax create "[{\"name\":\"QA SST 6%\",\"amount\":6,\"amount_type\":\"percent\",\"type_tax_use\":\"purchase\"}]" "{$CTX}" | rid)
E1=$(call hr.expense create "[{\"name\":\"QA-EXP taxi\",\"sheet_id\":$SH,\"employee_id\":$EMP,\"date\":\"2026-04-08\",\"account_id\":$EXPACC,\"quantity\":1,\"unit_amount\":100}]" "{$CTX}" | rid)
E2=$(call hr.expense create "[{\"name\":\"QA-EXP meals\",\"sheet_id\":$SH,\"employee_id\":$EMP,\"date\":\"2026-04-09\",\"account_id\":$EXPACC,\"quantity\":2,\"unit_amount\":50,\"tax_id\":$TAX}]" "{$CTX}" | rid)
[ -n "$E1" ] && [ -n "$E2" ] && ok "two expenses recorded" || no "expense create failed"

echo "############ totals are derived, never trusted from the client ############"
[ "$(pg "SELECT total_amount FROM hr_expense WHERE id=$E1")" = "100000000" ] \
    && ok "1 x RM100 = RM100" || no "line 1 total = $(pg "SELECT total_amount FROM hr_expense WHERE id=$E1")"
[ "$(pg "SELECT total_amount FROM hr_expense WHERE id=$E2")" = "106000000" ] \
    && ok "2 x RM50 + 6% SST = RM106" || no "line 2 total = $(pg "SELECT total_amount FROM hr_expense WHERE id=$E2")"
[ "$(pg "SELECT tax_amount FROM hr_expense WHERE id=$E2")" = "6000000" ] \
    && ok "the SST is recorded as RM6" || no "tax = $(pg "SELECT tax_amount FROM hr_expense WHERE id=$E2")"
[ "$(pg "SELECT total_amount FROM hr_expense_sheet WHERE id=$SH")" = "206000000" ] \
    && ok "the report total rolls up to RM206" || no "sheet total = $(pg "SELECT total_amount FROM hr_expense_sheet WHERE id=$SH")"
# Editing a line must move the report total with it.
call hr.expense write "[[$E1],{\"unit_amount\":120}]" "{$CTX}" >/dev/null
[ "$(pg "SELECT total_amount FROM hr_expense_sheet WHERE id=$SH")" = "226000000" ] \
    && ok "editing a line reprices the report (RM226)" || no "report total did not follow the edit"
call hr.expense write "[[$E1],{\"unit_amount\":100}]" "{$CTX}" >/dev/null

echo "############ the workflow refuses illegal transitions ############"
R=$(call hr.expense.sheet action_approve "[[$SH]]" "{$CTX}")
echo "$R" | grep -qi 'only a submitted' && ok "a draft report cannot be approved" || no "approve skipped the submit step"
R=$(call hr.expense.sheet action_post "[[$SH]]" "{$CTX}")
echo "$R" | grep -qi 'approve the expense report' && ok "an unapproved report cannot be posted" || no "post skipped the approval"
EMPTY=$(call hr.expense.sheet create "[{\"name\":\"QA Expense Empty\",\"employee_id\":$EMP}]" "{$CTX}" | rid)
R=$(call hr.expense.sheet action_submit "[[$EMPTY]]" "{$CTX}")
echo "$R" | grep -qi 'at least one expense' && ok "an empty report cannot be submitted" || no "an empty report was submitted"
pg "DELETE FROM hr_expense_sheet WHERE id=$EMPTY" >/dev/null

echo "############ submit -> approve -> post ############"
call hr.expense.sheet action_submit "[[$SH]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT state FROM hr_expense_sheet WHERE id=$SH")" = "submit" ] && ok "report submits" || no "submit failed"
[ "$(pg "SELECT state FROM hr_expense WHERE id=$E1")" = "reported" ] && ok "its expenses follow to Submitted" || no "expense state did not follow"
call hr.expense.sheet action_approve "[[$SH]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT state FROM hr_expense_sheet WHERE id=$SH")" = "approve" ] && ok "report approves" || no "approve failed"
call hr.expense.sheet action_post "[[$SH]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT state FROM hr_expense_sheet WHERE id=$SH")" = "post" ] && ok "report posts" || no "post failed"
MV=$(pg "SELECT move_id FROM hr_expense_sheet WHERE id=$SH")
[ -n "$MV" ] && [ "$MV" != "" ] && ok "a journal entry was created ($MV)" || { no "no journal entry"; echo "*** FAILURES ***"; exit 1; }

echo "############ the journal entry is correct ############"
[ "$(pg "SELECT state FROM account_move WHERE id=$MV")" = "posted" ] && ok "the entry is posted" || no "entry is not posted"
DR=$(pg "SELECT COALESCE(SUM(debit),0) FROM account_move_line WHERE move_id=$MV")
CR=$(pg "SELECT COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$MV")
[ "$DR" = "$CR" ] && ok "debits equal credits ($DR)" || no "the entry does not balance: Dr $DR / Cr $CR"
[ "$DR" = "206000000" ] && ok "the entry totals RM206" || no "entry total = $DR"
[ "$(pg "SELECT COALESCE(SUM(debit),0) FROM account_move_line WHERE move_id=$MV AND account_id=$EXPACC")" = "206000000" ] \
    && ok "the full tax-inclusive amount is charged to the expense account" \
    || no "the expense account was not debited in full"
[ "$(pg "SELECT COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$MV AND account_id=$PAYACC")" = "206000000" ] \
    && ok "the employee is credited what they are owed" || no "the payable was not credited"
# SST must NOT land in a separate recoverable tax account.
[ "$(pg "SELECT count(*) FROM account_move_line l JOIN account_account a ON a.id=l.account_id WHERE l.move_id=$MV AND a.id NOT IN ($EXPACC,$PAYACC)")" = "0" ] \
    && ok "no recoverable input-tax line (SST is a cost, not a VAT credit)" || no "an unexpected tax line was posted"

echo "############ reimbursement ############"
call hr.expense.sheet action_register_payment "[[$SH]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT state FROM hr_expense_sheet WHERE id=$SH")" = "done" ] && ok "the report is marked Paid" || no "payment did not complete"
PM=$(pg "SELECT payment_move_id FROM hr_expense_sheet WHERE id=$SH")
[ -n "$PM" ] && ok "a reimbursement entry was created ($PM)" || no "no reimbursement entry"
[ "$(pg "SELECT COALESCE(SUM(debit),0) FROM account_move_line WHERE move_id=$PM AND account_id=$PAYACC")" = "206000000" ] \
    && ok "the payment clears the employee payable" || no "the payable was not cleared"
PDR=$(pg "SELECT COALESCE(SUM(debit),0) FROM account_move_line WHERE move_id=$PM")
PCR=$(pg "SELECT COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$PM")
[ "$PDR" = "$PCR" ] && ok "the reimbursement entry balances" || no "reimbursement does not balance: Dr $PDR / Cr $PCR"
# Net effect on the payable across both entries is zero: the employee is square.
NET=$(pg "SELECT COALESCE(SUM(debit-credit),0) FROM account_move_line WHERE move_id IN ($MV,$PM) AND account_id=$PAYACC")
[ "$NET" = "0" ] && ok "the employee payable nets to zero once reimbursed" || no "payable left at $NET"

echo "############ a company-paid report has nothing to reimburse ############"
SH2=$(call hr.expense.sheet create "[{\"name\":\"QA Expense Company Card\",\"employee_id\":$EMP,\"date\":\"2026-04-11\",\"payment_mode\":\"company_account\"}]" "{$CTX}" | rid)
call hr.expense create "[{\"name\":\"QA-EXP hotel\",\"sheet_id\":$SH2,\"employee_id\":$EMP,\"account_id\":$EXPACC,\"quantity\":1,\"unit_amount\":300,\"payment_mode\":\"company_account\"}]" "{$CTX}" >/dev/null
call hr.expense.sheet action_submit  "[[$SH2]]" "{$CTX}" >/dev/null
call hr.expense.sheet action_approve "[[$SH2]]" "{$CTX}" >/dev/null
call hr.expense.sheet action_post    "[[$SH2]]" "{$CTX}" >/dev/null
MV2=$(pg "SELECT move_id FROM hr_expense_sheet WHERE id=$SH2")
[ "$(pg "SELECT COALESCE(SUM(credit),0) FROM account_move_line l JOIN account_account a ON a.id=l.account_id WHERE l.move_id=$MV2 AND a.account_type='asset_cash'")" = "300000000" ] \
    && ok "a company-paid report credits cash, not the employee" || no "company-paid report credited the wrong account"
R=$(call hr.expense.sheet action_register_payment "[[$SH2]]" "{$CTX}")
echo "$R" | grep -qi 'nothing to reimburse' && ok "reimbursing a company-paid report is refused" || no "a company-paid report offered a reimbursement"

echo "############ refusal ############"
SH3=$(call hr.expense.sheet create "[{\"name\":\"QA Expense Refused\",\"employee_id\":$EMP}]" "{$CTX}" | rid)
E4=$(call hr.expense create "[{\"name\":\"QA-EXP dubious\",\"sheet_id\":$SH3,\"employee_id\":$EMP,\"account_id\":$EXPACC,\"quantity\":1,\"unit_amount\":90}]" "{$CTX}" | rid)
call hr.expense.sheet action_submit "[[$SH3]]" "{$CTX}" >/dev/null
call hr.expense.sheet action_refuse "[[$SH3]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT state FROM hr_expense_sheet WHERE id=$SH3")" = "cancel" ] && ok "a report can be refused" || no "refuse failed"
[ "$(pg "SELECT state FROM hr_expense WHERE id=$E4")" = "refused" ] && ok "its expenses are marked refused" || no "expense not marked refused"
R=$(call hr.expense.sheet action_post "[[$SH3]]" "{$CTX}")
echo "$R" | grep -qi 'approve the expense report' && ok "a refused report cannot be posted" || no "a refused report was postable"

echo "############ housekeeping ############"
pg "DELETE FROM hr_expense WHERE sheet_id IN ($SH,$SH2,$SH3)" >/dev/null
pg "DELETE FROM account_move_line WHERE move_id IN (SELECT move_id FROM hr_expense_sheet WHERE id IN ($SH,$SH2,$SH3) AND move_id IS NOT NULL)" >/dev/null
pg "DELETE FROM account_move_line WHERE move_id IN (SELECT payment_move_id FROM hr_expense_sheet WHERE id IN ($SH,$SH2,$SH3) AND payment_move_id IS NOT NULL)" >/dev/null
pg "DELETE FROM account_move WHERE id IN ($MV,$PM,$MV2)" >/dev/null
pg "DELETE FROM hr_expense_sheet WHERE id IN ($SH,$SH2,$SH3)" >/dev/null
# The 6% tax this script may have created, but only if nothing else uses it —
# a real chart of accounts could legitimately have one by now.
pg "DELETE FROM account_tax t WHERE t.name = 'QA SST 6%'
      AND NOT EXISTS (SELECT 1 FROM hr_expense e WHERE e.tax_id = t.id)" >/dev/null
ok "fixtures cleaned up"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

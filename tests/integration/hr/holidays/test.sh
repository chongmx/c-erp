#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# Time off — allocations, requests and the balance (docs/113 §2).
#
# Four tables and one number that has to be right. Everything here exists to
# make that number impossible to corrupt, so the assertions are mostly about
# refusals:
#
#   * WORKING days, not calendar days — a request across a weekend must not
#     burn weekend days, and a public holiday inside the range must not count.
#   * the balance is a CEILING when the type requires an allocation, and it
#     can never go negative.
#   * one employee cannot hold two approved leaves over the same dates.
#   * a client-supplied number_of_days is discarded; the dates are the truth.
#   * cancelling an approved leave gives the days back.
#
# Dates are fixed and in the future (March 2026) so the arithmetic is stable:
# 2026-03-02 is a Monday.
# =============================================================
auth_or_die

cleanup() {
    pg "DELETE FROM hr_leave WHERE employee_id IN
          (SELECT id FROM hr_employee WHERE name LIKE 'LV %')" >/dev/null
    pg "DELETE FROM hr_leave_allocation WHERE employee_id IN
          (SELECT id FROM hr_employee WHERE name LIKE 'LV %')" >/dev/null
    pg "DELETE FROM hr_employee WHERE name LIKE 'LV %'" >/dev/null
    pg "DELETE FROM hr_public_holiday WHERE name LIKE 'LV %'" >/dev/null
    pg "DELETE FROM hr_leave_type WHERE code = 'LVTEST'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

# ------------------------------------------------------------------
sec "1. schema, seeds and menus"
# ------------------------------------------------------------------
for t in hr_leave hr_leave_type hr_leave_allocation hr_public_holiday; do
    t_eq "1" "$(pg "SELECT count(*) FROM pg_tables WHERE tablename='$t'")" "$t exists"
done
t_ge "$(pg "SELECT count(*) FROM hr_leave_type")" "4" "the default leave types are seeded"
t_eq "1" "$(pg "SELECT requires_allocation::int FROM hr_leave_type WHERE code='ANNUAL'")" "Annual Leave requires an allocation"
t_eq "0" "$(pg "SELECT requires_allocation::int FROM hr_leave_type WHERE code='UNPAID'")" "Unpaid Leave does not"
# Public holidays are deliberately NOT seeded: wrong dates silently miscount.
t_eq "0" "$(pg "SELECT count(*) FROM hr_public_holiday WHERE name NOT LIKE 'LV %'")" "no public holidays are shipped"
t_eq "hr.leave" "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Time Off' LIMIT 1")" \
     "Employees -> Time Off is wired"

ANNUAL=$(pg "SELECT id FROM hr_leave_type WHERE code='ANNUAL'")
UNPAID=$(pg "SELECT id FROM hr_leave_type WHERE code='UNPAID'")
EMP=$(call hr.employee create '[{"name":"LV Taker"}]' | rid)
t_nonempty "$EMP" "an employee to take leave"
[ -z "$EMP" ] || [ -z "$ANNUAL" ] && { verdict; exit 1; }

# ------------------------------------------------------------------
sec "2. WORKING days, not calendar days"
# ------------------------------------------------------------------
# Mon 2 Mar → Fri 6 Mar = 5 working days.
L1=$(call hr.leave create "[{\"employee_id\":$EMP,\"leave_type_id\":$ANNUAL,\"date_from\":\"2026-03-02\",\"date_to\":\"2026-03-06\"}]" | rid)
t_nonempty "$L1" "a Mon-Fri request was created"
t_eq "5.00" "$(pg "SELECT number_of_days FROM hr_leave WHERE id=$L1")" "Mon-Fri counts 5 days"

# Mon 2 Mar → Sun 8 Mar spans a weekend and is still 5 working days.
L2=$(call hr.leave create "[{\"employee_id\":$EMP,\"leave_type_id\":$ANNUAL,\"date_from\":\"2026-03-02\",\"date_to\":\"2026-03-08\"}]" | rid)
t_eq "5.00" "$(pg "SELECT number_of_days FROM hr_leave WHERE id=$L2")" "a range covering the weekend still counts 5"

# A weekend-only request counts nothing.
L3=$(call hr.leave create "[{\"employee_id\":$EMP,\"leave_type_id\":$ANNUAL,\"date_from\":\"2026-03-07\",\"date_to\":\"2026-03-08\"}]" | rid)
t_eq "0.00" "$(pg "SELECT number_of_days FROM hr_leave WHERE id=$L3")" "a Sat-Sun request counts 0 days"

# A public holiday inside the range is not a leave day.
pg "INSERT INTO hr_public_holiday (name, date, company_id) VALUES ('LV Test Holiday','2026-03-04',1)" >/dev/null
call hr.leave write "[[$L1],{\"date_from\":\"2026-03-02\",\"date_to\":\"2026-03-06\"}]" >/dev/null
t_eq "4.00" "$(pg "SELECT number_of_days FROM hr_leave WHERE id=$L1")" "a public holiday in range drops the count to 4"
pg "DELETE FROM hr_public_holiday WHERE name='LV Test Holiday'" >/dev/null
call hr.leave write "[[$L1],{\"date_from\":\"2026-03-02\",\"date_to\":\"2026-03-06\"}]" >/dev/null
t_eq "5.00" "$(pg "SELECT number_of_days FROM hr_leave WHERE id=$L1")" "removing the holiday restores it to 5"

# The day count is derived: a client-supplied value is discarded.
call hr.leave write "[[$L1],{\"number_of_days\":99}]" >/dev/null 2>&1
t_eq "5.00" "$(pg "SELECT number_of_days FROM hr_leave WHERE id=$L1")" "a client-supplied day count is ignored"

# ------------------------------------------------------------------
sec "3. approving without an allocation is refused"
# ------------------------------------------------------------------
# Annual Leave requires an allocation and this employee has none, so the
# balance is zero and the request cannot be approved. This is the check that
# stops a balance going negative.
call hr.leave action_confirm "[[$L1]]" >/dev/null
t_eq "confirm" "$(pg "SELECT state FROM hr_leave WHERE id=$L1")" "the request submits"
R=$(call hr.leave action_approve "[[$L1]]")
has_error "$R" && ok "approving with no allocation is refused" || no "a leave was approved against a zero balance"
t_eq "confirm" "$(pg "SELECT state FROM hr_leave WHERE id=$L1")" "and it stayed submitted"

# ------------------------------------------------------------------
sec "4. an allocation, approved, becomes the ceiling"
# ------------------------------------------------------------------
AL=$(call hr.leave.allocation create "[{\"employee_id\":$EMP,\"leave_type_id\":$ANNUAL,\"number_of_days\":8}]" | rid)
t_nonempty "$AL" "an 8-day allocation was created"
# A draft allocation is not a balance.
R=$(call hr.leave action_approve "[[$L1]]")
has_error "$R" && ok "a DRAFT allocation does not create a balance" || no "a draft allocation was counted"

call hr.leave.allocation action_confirm "[[$AL]]" >/dev/null
call hr.leave.allocation action_approve "[[$AL]]" >/dev/null
t_eq "validate" "$(pg "SELECT state FROM hr_leave_allocation WHERE id=$AL")" "the allocation is approved"

BAL=$(call_k hr.leave leave_balance "[$EMP]" "\"leave_type_id\":$ANNUAL")
t_contains "$BAL" '"allocated":8' "the balance reports 8 allocated"

R=$(call hr.leave action_approve "[[$L1]]")
has_error "$R" && no "approving within the balance failed: $(echo "$R" | head -c 160)" || ok "5 days out of 8 is approved"
t_eq "validate" "$(pg "SELECT state FROM hr_leave WHERE id=$L1")" "the leave is approved"

BAL=$(call_k hr.leave leave_balance "[$EMP]" "\"leave_type_id\":$ANNUAL")
t_contains "$BAL" '"taken":5' "5 days are taken"
t_contains "$BAL" '"remaining":3' "3 remain"

# ------------------------------------------------------------------
sec "5. the ceiling holds — the balance never goes negative"
# ------------------------------------------------------------------
# 3 days remain; ask for 5 (Mon 16 - Fri 20 Mar).
L4=$(call hr.leave create "[{\"employee_id\":$EMP,\"leave_type_id\":$ANNUAL,\"date_from\":\"2026-03-16\",\"date_to\":\"2026-03-20\"}]" | rid)
t_eq "5.00" "$(pg "SELECT number_of_days FROM hr_leave WHERE id=$L4")" "the second request is 5 days"
call hr.leave action_confirm "[[$L4]]" >/dev/null
R=$(call hr.leave action_approve "[[$L4]]")
has_error "$R" && ok "5 days against a 3-day balance is refused" || no "the balance went negative"
NEG=$(pg "SELECT (COALESCE(SUM(number_of_days),0) > 8)::int FROM hr_leave WHERE employee_id=$EMP AND leave_type_id=$ANNUAL AND state='validate'")
t_eq "0" "$NEG" "approved leave never exceeds the allocation"

# Exactly 3 days (Mon 16 - Wed 18) fits.
call hr.leave write "[[$L4],{\"date_from\":\"2026-03-16\",\"date_to\":\"2026-03-18\"}]" >/dev/null
t_eq "3.00" "$(pg "SELECT number_of_days FROM hr_leave WHERE id=$L4")" "trimmed to 3 days"
call hr.leave action_confirm "[[$L4]]" >/dev/null 2>&1
R=$(call hr.leave action_approve "[[$L4]]")
has_error "$R" && no "exactly the remaining balance was refused: $(echo "$R" | head -c 160)" || ok "exactly the remaining 3 days is approved"

BAL=$(call_k hr.leave leave_balance "[$EMP]" "\"leave_type_id\":$ANNUAL")
t_contains "$BAL" '"remaining":0' "the balance is now exactly zero"

# ------------------------------------------------------------------
sec "6. no two approved leaves over the same dates"
# ------------------------------------------------------------------
# Give plenty of allocation so the refusal can only be the overlap rule.
AL2=$(call hr.leave.allocation create "[{\"employee_id\":$EMP,\"leave_type_id\":$ANNUAL,\"number_of_days\":30}]" | rid)
call hr.leave.allocation action_confirm "[[$AL2]]" >/dev/null
call hr.leave.allocation action_approve "[[$AL2]]" >/dev/null

# L1 is approved for Mon 2 - Fri 6 Mar. Ask for Wed 4 - Thu 5, inside it.
L5=$(call hr.leave create "[{\"employee_id\":$EMP,\"leave_type_id\":$ANNUAL,\"date_from\":\"2026-03-04\",\"date_to\":\"2026-03-05\"}]" | rid)
call hr.leave action_confirm "[[$L5]]" >/dev/null
R=$(call hr.leave action_approve "[[$L5]]")
has_error "$R" && ok "overlapping approved leave is refused" || no "the employee was approved for leave twice over the same dates"

# A different employee over the same dates is fine — the rule is per person.
EMP2=$(call hr.employee create '[{"name":"LV Other"}]' | rid)
AL3=$(call hr.leave.allocation create "[{\"employee_id\":$EMP2,\"leave_type_id\":$ANNUAL,\"number_of_days\":10}]" | rid)
call hr.leave.allocation action_confirm "[[$AL3]]" >/dev/null
call hr.leave.allocation action_approve "[[$AL3]]" >/dev/null
L6=$(call hr.leave create "[{\"employee_id\":$EMP2,\"leave_type_id\":$ANNUAL,\"date_from\":\"2026-03-04\",\"date_to\":\"2026-03-05\"}]" | rid)
call hr.leave action_confirm "[[$L6]]" >/dev/null
R=$(call hr.leave action_approve "[[$L6]]")
has_error "$R" && no "a different employee was blocked by someone else's leave" || ok "a different employee may take the same dates"

# ------------------------------------------------------------------
sec "7. cancelling an approved leave returns the days"
# ------------------------------------------------------------------
BEFORE=$(pg "SELECT COALESCE(SUM(number_of_days),0) FROM hr_leave WHERE employee_id=$EMP AND leave_type_id=$ANNUAL AND state='validate'")
call hr.leave action_cancel "[[$L4]]" >/dev/null
t_eq "cancel" "$(pg "SELECT state FROM hr_leave WHERE id=$L4")" "the leave is cancelled"
AFTER=$(pg "SELECT COALESCE(SUM(number_of_days),0) FROM hr_leave WHERE employee_id=$EMP AND leave_type_id=$ANNUAL AND state='validate'")
t_eq "3.00" "$(pg "SELECT ($BEFORE - $AFTER)::numeric(6,2)")" "the 3 days came back"
# ...and the freed dates can now be approved for somebody.
R=$(call hr.leave action_approve "[[$L5]]")
has_error "$R" && ok "the still-overlapping request is still refused" || ok "the freed dates are usable again"

# ------------------------------------------------------------------
sec "8. the state machine refuses illegal transitions"
# ------------------------------------------------------------------
R=$(call hr.leave action_approve "[[$L1]]")
has_error "$R" && ok "an already-approved leave cannot be approved twice" || no "double approval was accepted"

L7=$(call hr.leave create "[{\"employee_id\":$EMP2,\"leave_type_id\":$ANNUAL,\"date_from\":\"2026-04-06\",\"date_to\":\"2026-04-07\"}]" | rid)
R=$(call hr.leave action_approve "[[$L7]]")
has_error "$R" && ok "a draft leave cannot skip submission" || no "a draft was approved directly"

call hr.leave action_confirm "[[$L7]]" >/dev/null
call hr.leave action_refuse  "[[$L7]]" >/dev/null
t_eq "refuse" "$(pg "SELECT state FROM hr_leave WHERE id=$L7")" "a request can be refused"
R=$(call hr.leave action_approve "[[$L7]]")
has_error "$R" && ok "a refused request cannot then be approved" || no "a refused request was approved"

# Editing an approved leave would silently move a reported balance.
R=$(call hr.leave write "[[$L1],{\"date_to\":\"2026-03-13\"}]")
has_error "$R" && ok "an approved leave cannot be edited in place" || no "an approved leave was edited"

# ------------------------------------------------------------------
sec "9. a type that needs no allocation is not capped"
# ------------------------------------------------------------------
L8=$(call hr.leave create "[{\"employee_id\":$EMP2,\"leave_type_id\":$UNPAID,\"date_from\":\"2026-05-04\",\"date_to\":\"2026-05-08\"}]" | rid)
t_eq "5.00" "$(pg "SELECT number_of_days FROM hr_leave WHERE id=$L8")" "5 unpaid days requested"
call hr.leave action_confirm "[[$L8]]" >/dev/null
R=$(call hr.leave action_approve "[[$L8]]")
has_error "$R" && no "unpaid leave was capped by a balance it does not use: $(echo "$R" | head -c 160)" \
                || ok "unpaid leave is approved with no allocation"

# ------------------------------------------------------------------
sec "10. a zero-day request cannot be approved"
# ------------------------------------------------------------------
# L3 is the Sat-Sun request: 0 working days. Approving it would consume
# nothing and block those dates for no reason.
call hr.leave action_confirm "[[$L3]]" >/dev/null 2>&1
R=$(call hr.leave action_approve "[[$L3]]")
has_error "$R" && ok "a request covering no working days is refused" || no "a zero-day leave was approved"

# ------------------------------------------------------------------
sec "11. the balance endpoint"
# ------------------------------------------------------------------
ALL=$(call hr.leave leave_balance "[$EMP]")
t_contains "$ALL" 'Annual Leave' "the balance lists every active type"
t_contains "$ALL" 'Unpaid Leave' "including the ones needing no allocation"
R=$(call hr.leave leave_balance '[]')
has_error "$R" && ok "a balance with no employee is refused" || no "a balance was returned for nobody"

verdict

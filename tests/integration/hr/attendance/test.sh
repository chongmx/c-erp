#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# Attendance — the clock (docs/113 §1).
#
# Attendance is two timestamps and a derived number, so the interesting
# assertions are all about what must be IMPOSSIBLE:
#
#   * two open attendances for one employee — the double-clock-in a kiosk
#     produces when somebody taps twice. Guarded by a PARTIAL UNIQUE INDEX,
#     not by the handler, so §5 removes the handler's head start and checks
#     the database still refuses.
#   * worked_hours arriving from the client. It is derived from the stored
#     timestamps; §6 writes a lie and asserts it is ignored.
#   * checking out when not in, or in when already in.
# =============================================================
auth_or_die

cleanup() {
    pg "DELETE FROM hr_attendance WHERE employee_id IN
          (SELECT id FROM hr_employee WHERE name LIKE 'ATT %')" >/dev/null
    pg "DELETE FROM hr_employee WHERE name LIKE 'ATT %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

# ------------------------------------------------------------------
sec "1. the table and its guards exist"
# ------------------------------------------------------------------
t_eq "1" "$(pg "SELECT count(*) FROM pg_tables WHERE tablename='hr_attendance'")" "hr_attendance exists"
t_eq "1" "$(pg "SELECT count(*) FROM pg_indexes WHERE indexname='hr_attendance_one_open_uniq'")" \
     "the one-open-attendance unique index exists"
t_eq "1" "$(pg "SELECT count(*) FROM pg_constraint WHERE conname='hr_attendance_interval_chk'")" \
     "the check_out > check_in constraint exists"
t_eq "hr.attendance" "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Attendance' LIMIT 1")" \
     "Employees -> Attendance is wired"

# ------------------------------------------------------------------
sec "2. an employee to clock"
# ------------------------------------------------------------------
EMP=$(call hr.employee create '[{"name":"ATT Clocker"}]' | rid)
t_nonempty "$EMP" "employee created"
[ -z "$EMP" ] && { verdict; exit 1; }

ST=$(call hr.attendance attendance_state "[$EMP]")
t_contains "$ST" '"checked_out"' "a new employee starts checked out"

# ------------------------------------------------------------------
sec "3. check in, then out"
# ------------------------------------------------------------------
IN=$(call hr.attendance action_check_in "[$EMP]")
has_error "$IN" && no "check-in failed: $(echo "$IN" | head -c 160)"
t_contains "$IN" '"checked_in"' "the employee is checked in"
t_eq "1" "$(pg "SELECT count(*) FROM hr_attendance WHERE employee_id=$EMP AND check_out IS NULL")" \
     "exactly one open attendance"

ST=$(call hr.attendance attendance_state "[$EMP]")
t_contains "$ST" '"checked_in"' "the state agrees"

sleep 1.2
OUT=$(call hr.attendance action_check_out "[$EMP]")
has_error "$OUT" && no "check-out failed: $(echo "$OUT" | head -c 160)"
t_contains "$OUT" '"checked_out"' "the employee is checked out"
t_eq "0" "$(pg "SELECT count(*) FROM hr_attendance WHERE employee_id=$EMP AND check_out IS NULL")" \
     "nothing is left open"
# worked_hours is derived on the server; a 1.2s shift rounds to 0.00 hours,
# which is correct — what matters is that check_out was stamped at all.
t_eq "1" "$(pg "SELECT count(*) FROM hr_attendance WHERE employee_id=$EMP AND check_out IS NOT NULL")" \
     "the record was closed with a check-out time"

# ------------------------------------------------------------------
sec "4. the illegal transitions are refused"
# ------------------------------------------------------------------
R=$(call hr.attendance action_check_out "[$EMP]")
has_error "$R" && ok "checking out when not in is refused" || no "a second check-out was accepted"

call hr.attendance action_check_in "[$EMP]" >/dev/null
R=$(call hr.attendance action_check_in "[$EMP]")
has_error "$R" && ok "checking in twice is refused" || no "a double check-in was accepted"
t_eq "1" "$(pg "SELECT count(*) FROM hr_attendance WHERE employee_id=$EMP AND check_out IS NULL")" \
     "still exactly one open attendance"

# ------------------------------------------------------------------
sec "5. NEGATIVE CONTROL — the database refuses it too"
# ------------------------------------------------------------------
# The handler check above would pass even if the index were missing. Insert a
# second open attendance directly, bypassing the handler entirely: the partial
# unique index is what has to stop it, and this is the only way to prove it is
# doing the work rather than the C++.
DIRECT=$(pgv "INSERT INTO hr_attendance (employee_id, check_in) VALUES ($EMP, now())" 2>&1)
case "$DIRECT" in
    *hr_attendance_one_open_uniq*|*duplicate*|*unique*)
        ok "a direct INSERT of a second open attendance is rejected by the index" ;;
    *)  no "the database accepted a second open attendance: $(printf '%s' "$DIRECT" | head -c 120)" ;;
esac
t_eq "1" "$(pg "SELECT count(*) FROM hr_attendance WHERE employee_id=$EMP AND check_out IS NULL")" \
     "and there is still only one"

# A check_out before its check_in is refused by the CHECK constraint.
BAD=$(pgv "INSERT INTO hr_attendance (employee_id, check_in, check_out)
           VALUES ($EMP, now(), now() - INTERVAL '2 hours')" 2>&1)
case "$BAD" in
    *hr_attendance_interval_chk*|*violates*) ok "a negative shift is rejected" ;;
    *) no "the database accepted check_out < check_in" ;;
esac

# ------------------------------------------------------------------
sec "6. worked_hours is derived, never accepted from the caller"
# ------------------------------------------------------------------
OPEN=$(pg "SELECT id FROM hr_attendance WHERE employee_id=$EMP AND check_out IS NULL LIMIT 1")
call hr.attendance write "[[$OPEN],{\"worked_hours\":999}]" >/dev/null 2>&1
t_eq "0" "$(pg "SELECT (worked_hours >= 999)::int FROM hr_attendance WHERE id=$OPEN")" \
     "a client-supplied worked_hours is ignored on an open record"
call hr.attendance action_check_out "[$EMP]" >/dev/null
t_eq "0" "$(pg "SELECT (worked_hours >= 999)::int FROM hr_attendance WHERE id=$OPEN")" \
     "and check-out recomputes it from the stored timestamps"

# The case that actually mattered: a CLOSED record. check-out never revisits
# one, so if `write` accepted the field here the inflated number would simply
# stand — a timesheet a client wrote for itself.
call hr.attendance write "[[$OPEN],{\"worked_hours\":999}]" >/dev/null 2>&1
t_eq "0" "$(pg "SELECT (worked_hours >= 999)::int FROM hr_attendance WHERE id=$OPEN")" \
     "a closed record cannot be given hours it did not work"
# ...and the value that IS there agrees with its own timestamps.
t_eq "0" "$(pg "SELECT (ABS(worked_hours - ROUND((EXTRACT(EPOCH FROM (check_out-check_in))/3600.0)::numeric,2)) > 0.01)::int
                  FROM hr_attendance WHERE id=$OPEN")" \
     "the stored hours match the stored timestamps"

# ------------------------------------------------------------------
sec "7. the kiosk toggle"
# ------------------------------------------------------------------
# One button: in if out, out if in. This is what the tablet by the door calls.
T1=$(call hr.attendance action_toggle "[$EMP]")
t_contains "$T1" '"checked_in"'  "toggle checks in when out"
T2=$(call hr.attendance action_toggle "[$EMP]")
t_contains "$T2" '"checked_out"' "toggle checks out when in"

# ------------------------------------------------------------------
sec "8. an unknown or archived employee is refused"
# ------------------------------------------------------------------
R=$(call hr.attendance action_check_in '[999999]')
has_error "$R" && ok "clocking a non-existent employee is refused" || no "a ghost employee was clocked"
call hr.employee write "[[$EMP],{\"active\":false}]" >/dev/null
R=$(call hr.attendance action_check_in "[$EMP]")
has_error "$R" && ok "clocking an archived employee is refused" || no "an archived employee was clocked"
call hr.employee write "[[$EMP],{\"active\":true}]" >/dev/null

# ------------------------------------------------------------------
sec "9. today's hours roll up"
# ------------------------------------------------------------------
N=$(pg "SELECT count(*) FROM hr_attendance WHERE employee_id=$EMP")
t_ge "$N" "3" "several attendance records exist for today"
ST=$(call hr.attendance attendance_state "[$EMP]")
t_contains "$ST" 'worked_hours_today' "the state reports today's total"

verdict

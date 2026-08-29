#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# FUNCTIONAL JOURNEY 05 — PROJECT.  (docs/109 §3)
#
#   a project -> tasks -> move them across the board -> log time -> planned
#   versus logged
#
# A week in the life of a project, run in order. The per-module test covers
# each write on its own; what only shows up in sequence is whether the numbers
# a manager actually looks at on Friday follow from what the team did all week
# — planned hours against logged hours, per task and per project.
#
# Two behaviours carry the risk, and both are exercised through the journey
# rather than in isolation:
#
#   move_stage  must stamp a completion date when a card reaches a closing
#               stage AND clear it when the card is dragged back out. A
#               one-way close is the classic bug: the task reopens on the
#               board but still reports as finished, so the project looks
#               done while work is outstanding.
#
#   set_cell    is idempotent by design — it takes the value the cell should
#               now read, never a delta. Logging 8 twice must leave 8. In a
#               journey this matters more than in a unit check, because the
#               week's total is what everything downstream is built on.
#
# Prefixed PJ / 'PJ ' and removed on the way out.
# =============================================================
auth_or_die

cleanup() {
    pg "DELETE FROM project_timesheet WHERE project_id IN (SELECT id FROM project_project WHERE name LIKE 'PJ %')" >/dev/null
    pg "DELETE FROM project_task      WHERE project_id IN (SELECT id FROM project_project WHERE name LIKE 'PJ %')" >/dev/null
    pg "DELETE FROM project_task_type WHERE name LIKE 'PJ %'" >/dev/null
    pg "DELETE FROM project_project   WHERE name LIKE 'PJ %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

MONDAY=$(date -d 'last monday' +%Y-%m-%d 2>/dev/null || date +%Y-%m-%d)

# ------------------------------------------------------------------
sec "1. starting a project"
# ------------------------------------------------------------------
PRJ=$(call project.project create '[{"name":"PJ Apollo","code":"PJA","allow_timesheets":true}]' | rid)
t_nonempty "$PRJ" "the project was created"
[ -z "$PRJ" ] && { verdict; exit 1; }

# A new project must arrive with a usable board. A project with no stages is
# one the team cannot actually work in.
STAGES=$(pg "SELECT count(*) FROM project_task_type WHERE project_id IS NULL OR project_id=$PRJ")
t_ge "$STAGES" 3 "there are stages to work through"
CLOSING=$(pg "SELECT id FROM project_task_type WHERE (project_id IS NULL OR project_id=$PRJ) AND is_closed ORDER BY sequence, id LIMIT 1")
OPENING=$(pg "SELECT id FROM project_task_type WHERE (project_id IS NULL OR project_id=$PRJ) AND NOT is_closed ORDER BY sequence, id LIMIT 1")
t_nonempty "$CLOSING" "one of them closes a task"
t_nonempty "$OPENING" "and one of them is an open stage"

# ------------------------------------------------------------------
sec "2. planning the week"
# ------------------------------------------------------------------
T1=$(call project.task create "[{\"project_id\":$PRJ,\"name\":\"PJ Design the board\",\"planned_hours\":8}]"  | rid)
T2=$(call project.task create "[{\"project_id\":$PRJ,\"name\":\"PJ Order the parts\",\"planned_hours\":4}]"   | rid)
T3=$(call project.task create "[{\"project_id\":$PRJ,\"name\":\"PJ Write the tests\",\"planned_hours\":12}]"  | rid)
t_nonempty "$T1" "task 1 created"
t_nonempty "$T2" "task 2 created"
t_nonempty "$T3" "task 3 created"
t_eq "3" "$(pg "SELECT count(*) FROM project_task WHERE project_id=$PRJ")" "the project has three tasks"

PLANNED=$(pg "SELECT COALESCE(SUM(planned_hours),0) FROM project_task WHERE project_id=$PRJ")
echo "    planned across the project: $PLANNED"
# t_ge, not t_ne "0" — an EMPTY value is not equal to "0" either, so a query
# that failed outright would sail through a "not zero" check. This journey
# spent a run proving that: the timesheet column is `unit_amount`, not
# `hours`, every SUM errored, pg() swallowed it, and three assertions passed
# on empty strings. An assertion that cannot fail on missing data is not an
# assertion.
t_ge "${PLANNED%%.*}" 1 "the plan carries hours"

# ------------------------------------------------------------------
sec "3. working the board"
# ------------------------------------------------------------------
MRES=$(call project.task move_stage "[{\"task_id\":$T2,\"stage_id\":$CLOSING,\"sequence\":1}]")
has_error "$MRES" && no "moving a task failed: $(echo "$MRES" | head -c 200)"
t_eq "$CLOSING" "$(pg "SELECT stage_id FROM project_task WHERE id=$T2")" "the task moved to the closing stage"
DATE_END=$(pg "SELECT COALESCE(date_end::text,'') FROM project_task WHERE id=$T2")
t_nonempty "$DATE_END" "reaching a closing stage stamped a completion date"

# And back out again — the half that is usually missing.
call project.task move_stage "[{\"task_id\":$T2,\"stage_id\":$OPENING,\"sequence\":1}]" >/dev/null
t_eq "$OPENING" "$(pg "SELECT stage_id FROM project_task WHERE id=$T2")" "it moved back to an open stage"
REOPENED=$(pg "SELECT COALESCE(date_end::text,'') FROM project_task WHERE id=$T2")
if [ -z "$REOPENED" ]; then
    ok "reopening CLEARED the completion date"
else
    no "the task reopened but still reports as completed on $REOPENED"
fi

# ------------------------------------------------------------------
sec "4. logging the week"
# ------------------------------------------------------------------
log_day() {  # log_day <task> <date> <hours>
    call project.timesheet set_cell \
        "[{\"project_id\":$PRJ,\"task_id\":$1,\"date\":\"$2\",\"hours\":$3}]"
}
D1="$MONDAY"
D2=$(date -d "$MONDAY +1 day" +%Y-%m-%d)
D3=$(date -d "$MONDAY +2 day" +%Y-%m-%d)

SRES=$(log_day "$T1" "$D1" 6); has_error "$SRES" && no "logging time failed: $(echo "$SRES" | head -c 200)"
log_day "$T1" "$D2" 2 >/dev/null
log_day "$T3" "$D3" 5 >/dev/null

LOGGED=$(pg "SELECT COALESCE(SUM(unit_amount),0) FROM project_timesheet WHERE project_id=$PRJ")
echo "    logged so far: $LOGGED"
t_ge "${LOGGED%%.*}" 1 "the week has time on it"

# Idempotence, in the place it actually bites: a retry or a double-submit must
# not double someone's day.
log_day "$T1" "$D1" 6 >/dev/null
AFTER=$(pg "SELECT COALESCE(SUM(unit_amount),0) FROM project_timesheet WHERE project_id=$PRJ")
t_eq "${LOGGED%%.*}" "${AFTER%%.*}" "logging the same day twice does not double it"
t_eq "1" "$(pg "SELECT count(*) FROM project_timesheet WHERE project_id=$PRJ AND task_id=$T1 AND date='$D1'")" \
     "and it left one row for that day, not two"

# Correcting a cell replaces the value rather than adding to it.
log_day "$T1" "$D1" 8 >/dev/null
CORRECTED=$(pg "SELECT unit_amount FROM project_timesheet WHERE project_id=$PRJ AND task_id=$T1 AND date='$D1'")
t_eq "8" "${CORRECTED%%.*}" "correcting a day replaces the value"

# ------------------------------------------------------------------
sec "5. planned versus logged"
# ------------------------------------------------------------------
# What the manager reads on Friday. Each side is summed independently from the
# rows, so a mismatch means the arithmetic the screens show is not the
# arithmetic the database supports.
PLANNED_T1=$(pg "SELECT COALESCE(planned_hours,0) FROM project_task WHERE id=$T1")
LOGGED_T1=$(pg  "SELECT COALESCE(SUM(unit_amount),0) FROM project_timesheet WHERE task_id=$T1")
echo "    task 1: planned $PLANNED_T1, logged $LOGGED_T1"
t_eq "10" "${LOGGED_T1%%.*}" "task 1 has 10 hours against it (8 corrected + 2)"

PROJ_LOGGED=$(pg "SELECT COALESCE(SUM(unit_amount),0) FROM project_timesheet WHERE project_id=$PRJ")
SUM_OF_TASKS=$(pg "SELECT COALESCE(SUM(t.h),0) FROM (
                     SELECT task_id, SUM(unit_amount) h FROM project_timesheet
                      WHERE project_id=$PRJ GROUP BY task_id) t")
t_eq "${SUM_OF_TASKS%%.*}" "${PROJ_LOGGED%%.*}" "the project total equals the sum of its tasks"

# Time logged against a task that is not in this project must never be counted
# into it — the classic way a project's hours quietly inflate.
STRAY=$(pg "SELECT count(*) FROM project_timesheet ts
             LEFT JOIN project_task t ON t.id = ts.task_id
            WHERE ts.project_id=$PRJ AND t.id IS NOT NULL AND t.project_id <> $PRJ")
t_eq "0" "${STRAY:-0}" "no time from another project's tasks is counted in"

# ==================================================================
# THE CANCEL PATH
#
# Work gets dropped: a task is descoped, a project is shelved. The question
# this section answers is what happens to everything hanging off it.
#
# Time already logged is the interesting case. It is a RECORD OF WORK SOMEONE
# DID — deleting a task must not make Tuesday afternoon disappear from the
# week's total without anyone noticing, and it must not leave timesheet rows
# pointing at a task that no longer exists, which is how a project's hours
# start disagreeing with the sum of its tasks.
# ==================================================================
sec "6. dropping a task that has time against it"
BEFORE_TOTAL=$(pg "SELECT COALESCE(SUM(unit_amount),0) FROM project_timesheet WHERE project_id=$PRJ")
T3_HOURS=$(pg  "SELECT COALESCE(SUM(unit_amount),0) FROM project_timesheet WHERE task_id=$T3")
T3_ROWS=$(pg   "SELECT count(*) FROM project_timesheet WHERE task_id=$T3")
echo "    project total $BEFORE_TOTAL, of which task 3 holds $T3_HOURS across $T3_ROWS row(s)"
t_ge "${T3_HOURS%%.*}" 1 "task 3 has time on it before we drop it"

URES=$(call project.task unlink "[[$T3]]")
has_error "$URES" && no "deleting the task failed: $(echo "$URES" | head -c 200)"
t_eq "0" "$(pg "SELECT count(*) FROM project_task WHERE id=$T3")" "the task is gone"

sec "7. nothing is left dangling behind it"
# The check that earns its keep: a timesheet row pointing at a deleted task is
# invisible until something joins on it and silently returns nothing.
ORPHANED=$(pg "SELECT count(*) FROM project_timesheet ts
                WHERE ts.task_id IS NOT NULL
                  AND NOT EXISTS (SELECT 1 FROM project_task t WHERE t.id = ts.task_id)")
t_eq "0" "${ORPHANED:-0}" "no timesheet row points at a task that no longer exists"

AFTER_TOTAL=$(pg "SELECT COALESCE(SUM(unit_amount),0) FROM project_timesheet WHERE project_id=$PRJ")
SUM_NOW=$(pg "SELECT COALESCE(SUM(t.h),0) FROM (
                SELECT task_id, SUM(unit_amount) h FROM project_timesheet
                 WHERE project_id=$PRJ GROUP BY task_id) t")
echo "    project total $BEFORE_TOTAL -> $AFTER_TOTAL"
t_eq "${SUM_NOW%%.*}" "${AFTER_TOTAL%%.*}" "the project total still equals the sum of its tasks"

# Whichever policy the system takes — carry the hours or drop them — it has to
# be one of them, consistently. A total that changed by something OTHER than
# the deleted task's hours means rows went somewhere unaccounted for.
DROPPED=$(pg "SELECT (${BEFORE_TOTAL%%.*} - ${AFTER_TOTAL%%.*})")
case "${DROPPED%%.*}" in
    0)                  ok "the hours were kept against the project (total unchanged)" ;;
    "${T3_HOURS%%.*}")  ok "the hours went with the task (total fell by exactly $T3_HOURS)" ;;
    *)                  no "the total moved by $DROPPED, which is neither 0 nor the task's $T3_HOURS — rows are unaccounted for" ;;
esac

sec "8. shelving the project"
ARES=$(call project.project write "[[$PRJ],{\"active\":false}]")
has_error "$ARES" && no "archiving the project failed: $(echo "$ARES" | head -c 200)"
ACTIVE=$(pg "SELECT active::text FROM project_project WHERE id=$PRJ")
case "$ACTIVE" in
    f|false) ok "the project is archived" ;;
    *)       no "the project is still active (active=$ACTIVE)" ;;
esac
# Archived, not deleted: the record of what was done has to survive.
t_eq "1" "$(pg "SELECT count(*) FROM project_project WHERE id=$PRJ")" "the project row still exists"
t_ge "$(pg "SELECT count(*) FROM project_timesheet WHERE project_id=$PRJ")" 1 \
     "and the time logged against it is still there"

verdict

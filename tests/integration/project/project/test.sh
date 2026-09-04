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
# Projects, tasks and timesheets (docs/100).
#
# Two behaviours carry most of the risk here, and most of the checks below
# exist for them:
#
#   move_stage  — the board's only write. It has to move a card, place it at a
#                 position, stamp a completion date when the card reaches a
#                 closing stage, and CLEAR that date when the card is dragged
#                 back out. A one-way close is the classic bug: the task
#                 reopens on the board but still reports as finished.
#
#   set_cell    — the timesheet's only write, and it is IDEMPOTENT on purpose.
#                 It takes the value the cell should now read, never a delta,
#                 so a double-submit or a retry cannot double someone's day.
#                 Sending 8 twice must leave 8, and it must collapse duplicate
#                 rows for the same day rather than sum them.
#
# The grid is also asserted to agree with itself: the row totals, the column
# totals and the week total are all computed server-side from the same rows, so
# a mismatch means the grid is showing arithmetic the database does not support.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }

cleanup(){
    pg "DELETE FROM project_timesheet WHERE project_id IN (SELECT id FROM project_project WHERE name LIKE 'QA-PRJ%')" >/dev/null
    pg "DELETE FROM project_task      WHERE project_id IN (SELECT id FROM project_project WHERE name LIKE 'QA-PRJ%')" >/dev/null
    pg "DELETE FROM project_task_type WHERE name LIKE 'QA-PRJ%'" >/dev/null
    pg "DELETE FROM project_project   WHERE name LIKE 'QA-PRJ%'" >/dev/null
}
cleanup; trap cleanup EXIT

SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; echo "*** FAILURES ***"; exit 1; }
CTX="\"context\":{\"session_id\":\"$SID\"}"
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":{$CTX}}}"; }
rid(){ sed -n 's/.*"result":\([0-9][0-9]*\).*/\1/p'; }
py(){ python3 -c "$1" 2>/dev/null; }
export PYTHONIOENCODING=utf-8

echo "############ schema and default stages ############"
for t in project_project project_task_type project_task project_timesheet; do
    [ "$(pg "SELECT count(*) FROM information_schema.tables WHERE table_name='$t'")" = "1" ] \
      && ok "$t exists" || no "$t missing"
done
NST=$(pg "SELECT count(*) FROM project_task_type WHERE project_id IS NULL")
[ -n "$NST" ] && [ "$NST" -ge 5 ] && ok "default stages seeded ($NST)" || no "default stages: $NST"
[ "$(pg "SELECT count(*) FROM project_task_type WHERE project_id IS NULL AND is_closed")" -ge 1 ] \
  && ok "at least one closing stage exists" || no "no closing stage"

echo "############ project CRUD ############"
P=$(call project.project create '[{"name":"QA-PRJ Apollo","code":"QAP","allow_timesheets":true}]' | rid)
[ -n "$P" ] && ok "project created (id $P)" || no "project create failed"
call project.project create '[{"name":""}]' | grep -q 'name is required' \
  && ok "a nameless project is rejected" || no "nameless project accepted"
call project.project create '[{"name":"QA-PRJ Bad","date_start":"2026-06-01","date_end":"2026-05-01"}]' \
  | grep -qi 'End Date cannot be before' && ok "end-before-start is rejected" || no "bad date range accepted"
# An empty date must reach SQL as NULL, not '' (docs/096).
call project.project create '[{"name":"QA-PRJ Dates","date_start":"","date_end":""}]' | grep -q '"result"' \
  && ok "empty dates are accepted as NULL" || no "empty dates broke the insert"

echo "############ stages and tasks ############"
NEW=$(pg "SELECT id FROM project_task_type WHERE project_id IS NULL AND name='New'")
PROG=$(pg "SELECT id FROM project_task_type WHERE project_id IS NULL AND name='In Progress'")
DONE=$(pg "SELECT id FROM project_task_type WHERE project_id IS NULL AND name='Done'")

T1=$(call project.task create "[{\"name\":\"QA-PRJ t1\",\"project_id\":$P,\"stage_id\":$NEW,\"planned_hours\":8}]" | rid)
T2=$(call project.task create "[{\"name\":\"QA-PRJ t2\",\"project_id\":$P,\"stage_id\":$NEW,\"planned_hours\":4}]" | rid)
T3=$(call project.task create "[{\"name\":\"QA-PRJ t3\",\"project_id\":$P,\"stage_id\":$PROG}]" | rid)
[ -n "$T1" ] && [ -n "$T2" ] && [ -n "$T3" ] && ok "three tasks created" || no "task create failed"
call project.task create '[{"name":"QA-PRJ orphan"}]' | grep -q 'project_id is required' \
  && ok "a task without a project is rejected" || no "project-less task accepted"
call project.task create "[{\"name\":\"QA-PRJ neg\",\"project_id\":$P,\"planned_hours\":-3}]" \
  | grep -qi 'cannot be negative' && ok "negative planned hours rejected" || no "negative hours accepted"
call project.task create "[{\"name\":\"QA-PRJ ks\",\"project_id\":$P,\"kanban_state\":\"sideways\"}]" \
  | grep -qi 'kanban_state must be' && ok "an unknown kanban_state is rejected" || no "bad kanban_state accepted"

echo "############ board ############"
B=$(call project.task board "[{\"project_id\":$P}]")
echo "$B" | grep -q '"stages"' && ok "board returns stages" || no "board has no stages"
NT=$(echo "$B" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(len(d['tasks']))")
[ "$NT" = "3" ] && ok "board returns the 3 tasks" || no "board returned $NT tasks"
# The card must carry what the column renders, or the board needs a second call.
FIELDS=$(echo "$B" | py "
import json,sys
d=json.load(sys.stdin)['result']
t=d['tasks'][0]
print(','.join(sorted(t.keys())))")
for f in kanban_state logged_hours planned_hours project_name stage_id user_login; do
    echo "$FIELDS" | grep -q "$f" && ok "card carries $f" || no "card missing $f ($FIELDS)"
done

echo "############ move_stage ############"
call project.task move_stage "[{\"task_id\":$T1,\"stage_id\":$PROG}]" | grep -q '"ok":true' \
  && ok "move_stage returns ok" || no "move_stage failed"
[ "$(pg "SELECT stage_id FROM project_task WHERE id=$T1")" = "$PROG" ] \
  && ok "the task really moved stage" || no "stage not persisted"

# Reaching a closing stage stamps date_end...
call project.task move_stage "[{\"task_id\":$T1,\"stage_id\":$DONE}]" | grep -q '"closed":true' \
  && ok "a closing stage reports closed" || no "closing stage not reported"
[ -n "$(pg "SELECT date_end FROM project_task WHERE id=$T1")" ] \
  && ok "date_end is stamped on close" || no "date_end not stamped"
# ...and leaving one must CLEAR it, or a reopened task still reads as finished.
call project.task move_stage "[{\"task_id\":$T1,\"stage_id\":$PROG}]" >/dev/null
[ -z "$(pg "SELECT date_end FROM project_task WHERE id=$T1")" ] \
  && ok "date_end is cleared when reopened" || no "date_end survived reopening"

# Position within the column, not just the column itself.
call project.task move_stage "[{\"task_id\":$T2,\"stage_id\":$PROG,\"index\":0}]" >/dev/null
FIRST=$(pg "SELECT id FROM project_task WHERE stage_id=$PROG AND project_id=$P ORDER BY sequence, id LIMIT 1")
[ "$FIRST" = "$T2" ] && ok "index=0 puts the card at the top" || no "top card is $FIRST, expected $T2"
call project.task move_stage "[{\"task_id\":$T2,\"stage_id\":$PROG,\"index\":99}]" >/dev/null
LAST=$(pg "SELECT id FROM project_task WHERE stage_id=$PROG AND project_id=$P ORDER BY sequence DESC, id DESC LIMIT 1")
[ "$LAST" = "$T2" ] && ok "an out-of-range index clamps to the end" || no "last card is $LAST, expected $T2"

call project.task move_stage "[{\"task_id\":999999,\"stage_id\":$PROG}]" | grep -qi 'No such task' \
  && ok "moving an unknown task is rejected" || no "unknown task accepted"
call project.task move_stage "[{\"task_id\":$T1,\"stage_id\":999999}]" | grep -qi 'No such stage' \
  && ok "moving to an unknown stage is rejected" || no "unknown stage accepted"
call project.task move_stage "[{\"stage_id\":$PROG}]" | grep -qi 'task_id is required' \
  && ok "move_stage requires a task" || no "move_stage accepted no task"

echo "############ timesheet set_cell is idempotent ############"
# ::date matters: date_trunc returns a TIMESTAMP, and "timestamp + 1" is an
# error in Postgres, not tomorrow. pg() hides the error, so the variable comes
# back empty and every assertion built on it fails for the wrong reason.
MON=$(pg "SELECT to_char(date_trunc('week', CURRENT_DATE)::date,'YYYY-MM-DD')")
TUE=$(pg "SELECT to_char(date_trunc('week', CURRENT_DATE)::date + 1,'YYYY-MM-DD')")
[ -z "$MON" ] || [ -z "$TUE" ] && { echo "    FAIL  could not compute week dates"; FAILED=1; }

call project.timesheet set_cell "[{\"project_id\":$P,\"task_id\":$T1,\"date\":\"$MON\",\"hours\":8}]" >/dev/null
H=$(pg "SELECT COALESCE(sum(unit_amount),0) FROM project_timesheet WHERE project_id=$P AND date='$MON'")
[ "$H" = "8.00" ] && ok "a cell writes 8h" || no "first write gave $H"

# The load-bearing property: sending the same value again must not add to it.
call project.timesheet set_cell "[{\"project_id\":$P,\"task_id\":$T1,\"date\":\"$MON\",\"hours\":8}]" >/dev/null
H=$(pg "SELECT COALESCE(sum(unit_amount),0) FROM project_timesheet WHERE project_id=$P AND date='$MON'")
[ "$H" = "8.00" ] && ok "re-sending 8h leaves 8h (idempotent)" || no "re-send doubled to $H"
N=$(pg "SELECT count(*) FROM project_timesheet WHERE project_id=$P AND date='$MON'")
[ "$N" = "1" ] && ok "the cell stays a single row" || no "cell became $N rows"

call project.timesheet set_cell "[{\"project_id\":$P,\"task_id\":$T1,\"date\":\"$MON\",\"hours\":5.5}]" >/dev/null
H=$(pg "SELECT COALESCE(sum(unit_amount),0) FROM project_timesheet WHERE project_id=$P AND date='$MON'")
[ "$H" = "5.50" ] && ok "lowering a cell replaces, not adds ($H)" || no "lowering gave $H"

# Several pre-existing rows for one day must collapse to what the grid shows.
pg "INSERT INTO project_timesheet (date, project_id, task_id, unit_amount) VALUES ('$MON',$P,$T1,3)" >/dev/null
call project.timesheet set_cell "[{\"project_id\":$P,\"task_id\":$T1,\"date\":\"$MON\",\"hours\":6}]" >/dev/null
N=$(pg "SELECT count(*) FROM project_timesheet WHERE project_id=$P AND task_id=$T1 AND date='$MON'")
H=$(pg "SELECT COALESCE(sum(unit_amount),0) FROM project_timesheet WHERE project_id=$P AND task_id=$T1 AND date='$MON'")
[ "$N" = "1" ] && [ "$H" = "6.00" ] && ok "duplicate rows collapse to one ($H)" || no "collapse gave $N rows / $H h"

call project.timesheet set_cell "[{\"project_id\":$P,\"task_id\":$T1,\"date\":\"$MON\",\"hours\":0}]" >/dev/null
[ "$(pg "SELECT count(*) FROM project_timesheet WHERE project_id=$P AND task_id=$T1 AND date='$MON'")" = "0" ] \
  && ok "zero deletes the entry" || no "zero did not clear the cell"

call project.timesheet set_cell "[{\"project_id\":$P,\"date\":\"$MON\",\"hours\":-2}]" | grep -qi 'cannot be negative' \
  && ok "negative hours are rejected" || no "negative hours accepted"
call project.timesheet set_cell "[{\"project_id\":$P,\"date\":\"$MON\",\"hours\":30}]" | grep -qi 'exceed 24 hours' \
  && ok "more than 24h in one entry is rejected" || no "30h accepted"
call project.timesheet set_cell "[{\"project_id\":999999,\"date\":\"$MON\",\"hours\":1}]" | grep -qi 'No such project' \
  && ok "an unknown project is rejected" || no "unknown project accepted"
call project.timesheet set_cell "[{\"date\":\"$MON\",\"hours\":1}]" | grep -qi 'project_id is required' \
  && ok "set_cell requires a project" || no "set_cell accepted no project"

echo "############ the week grid ############"
call project.timesheet set_cell "[{\"project_id\":$P,\"task_id\":$T1,\"date\":\"$MON\",\"hours\":8}]" >/dev/null
call project.timesheet set_cell "[{\"project_id\":$P,\"task_id\":$T1,\"date\":\"$TUE\",\"hours\":4}]" >/dev/null
call project.timesheet set_cell "[{\"project_id\":$P,\"task_id\":$T2,\"date\":\"$MON\",\"hours\":2}]" >/dev/null

G=$(call project.timesheet grid "[{\"date\":\"$MON\",\"project_id\":$P}]")
ND=$(echo "$G" | py "
import json,sys
print(len(json.load(sys.stdin)['result']['days']))")
[ "$ND" = "7" ] && ok "the grid spans 7 days" || no "grid has $ND days"
[ "$(echo "$G" | py "
import json,sys
print(json.load(sys.stdin)['result']['week_start'])")" = "$MON" ] \
  && ok "the week starts on Monday" || no "week_start is not Monday"
NR=$(echo "$G" | py "
import json,sys
print(len(json.load(sys.stdin)['result']['rows']))")
[ "$NR" = "2" ] && ok "one row per (project, task) — 2 rows" || no "grid has $NR rows, expected 2"

TOT=$(echo "$G" | py "
import json,sys
print(json.load(sys.stdin)['result']['total'])")
[ "$TOT" = "14.0" ] && ok "week total is 14h" || no "week total is $TOT, expected 14.0"

# The three totals the grid renders must all be derivable from its own cells.
AGREE=$(echo "$G" | py "
import json,sys
d=json.load(sys.stdin)['result']
cells=sum(v for r in d['rows'] for v in r['cells'].values())
rowsum=sum(r['total'] for r in d['rows'])
colsum=sum(d['col_totals'].values())
ok = abs(cells-rowsum)<1e-6 and abs(cells-colsum)<1e-6 and abs(cells-d['total'])<1e-6
print('agree' if ok else 'cells=%s rows=%s cols=%s total=%s'%(cells,rowsum,colsum,d['total']))")
[ "$AGREE" = "agree" ] && ok "row, column and week totals all agree" || no "totals disagree: $AGREE"

MONH=$(echo "$G" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(d['col_totals']['$MON'])")
[ "$MONH" = "10.0" ] && ok "Monday's column totals 10h" || no "Monday total is $MONH, expected 10.0"

# An unfiltered grid must not be scoped to the fixture project.
GA=$(call project.timesheet grid "[{\"date\":\"$MON\"}]")
echo "$GA" | grep -q '"rows"' && ok "an unfiltered grid still returns rows" || no "unfiltered grid failed"

echo "############ project stats ############"
S=$(call project.project stats "[{\"project_id\":$P}]")
LOG=$(echo "$S" | py "
import json,sys
print(json.load(sys.stdin)['result']['logged_hours'])")
[ "$LOG" = "14.0" ] && ok "stats report 14 logged hours" || no "stats logged $LOG"
PLAN=$(echo "$S" | py "
import json,sys
print(json.load(sys.stdin)['result']['planned_hours'])")
[ "$PLAN" = "12.0" ] && ok "stats report 12 planned hours" || no "stats planned $PLAN"
OPEN=$(echo "$S" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(d['open'], d['closed'], d['total'])")
[ "$OPEN" = "3 0 3" ] && ok "3 open / 0 closed before closing anything" || no "open/closed/total = $OPEN"
call project.task move_stage "[{\"task_id\":$T3,\"stage_id\":$DONE}]" >/dev/null
OPEN=$(call project.project stats "[{\"project_id\":$P}]" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(d['open'], d['closed'])")
[ "$OPEN" = "2 1" ] && ok "closing a task moves it from open to closed" || no "after close: $OPEN"

echo "############ summary ############"
SUM=$(call project.timesheet summary "[{}]")
echo "$SUM" | grep -q 'QA-PRJ Apollo' && ok "summary groups hours by project" || no "summary missing the project"

echo "############ cascade ############"
# Deleting a project must not strand its tasks or timesheets.
P2=$(call project.project create '[{"name":"QA-PRJ Doomed"}]' | rid)
TD=$(call project.task create "[{\"name\":\"QA-PRJ doomed task\",\"project_id\":$P2}]" | rid)
call project.timesheet set_cell "[{\"project_id\":$P2,\"task_id\":$TD,\"date\":\"$MON\",\"hours\":1}]" >/dev/null
pg "DELETE FROM project_project WHERE id=$P2" >/dev/null
[ "$(pg "SELECT count(*) FROM project_task WHERE project_id=$P2")" = "0" ] \
  && ok "tasks cascade with the project" || no "tasks survived the project"
[ "$(pg "SELECT count(*) FROM project_timesheet WHERE project_id=$P2")" = "0" ] \
  && ok "timesheets cascade with the project" || no "timesheets survived the project"

echo "############ menus ############"
[ "$(pg "SELECT count(*) FROM ir_ui_menu WHERE parent_id=130")" = "6" ] \
  && ok "6 menu entries under the Project app" || no "unexpected Project menu count"
[ "$(pg "SELECT count(*) FROM ir_ui_menu WHERE id BETWEEN 131 AND 136 AND action_id BETWEEN 108 AND 113")" = "0" ] \
  && ok "no stale menus left in ReportModule's 131-136 range" || no "stale project menus in 131-136"
for M in project.board project.timegrid; do
    grep -q "'$M'" web/static/src/app.js && ok "$M is registered as a custom view" || no "$M not in CUSTOM_VIEWS"
done

[ -z "$FAILED" ] && echo "  All checks passed." || echo "  *** FAILURES ***"

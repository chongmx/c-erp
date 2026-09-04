# 100 — Projects, tasks and timesheets

Status: **done**. `./scripts/run_tests.sh` → 73 passed, 0 failed
(`verify_project.sh`, 60 checks).

This closes the last item on the reference ERP-14 gap list.

---

## 1. Schema

Four tables in a new `modules/project/` module:

| table | notes |
|---|---|
| `project_project` | name, code, customer, manager, dates, `allow_timesheets` |
| `project_task_type` | kanban stages; `project_id NULL` = shared by every project |
| `project_task` | stage, assignee, deadline, `planned_hours`, `kanban_state`, `priority` |
| `project_timesheet` | date, project, task, user/employee, `unit_amount` (hours) |

**Timesheets got their own table, not `account_analytic_line`.** I said earlier
that the analytic line was the natural backbone — having looked, it isn't, yet:
the table is empty, no module owns it, and it has none of the columns a
timesheet needs (task, employee, hours). Bolting those onto a generic accounting
table would couple timesheets to accounting for no present benefit. A dedicated
table with an optional analytic link later is the cleaner order.

Hours are `NUMERIC(10,2)`, deliberately **not** `markScaled`. That flag only
does anything on `int8` columns — a `NUMERIC` is read straight through as a
double — so marking it would have been a no-op that implied a scaling rule the
code doesn't follow.

Five stages seed once: New, In Progress, Review, **Done** (closing),
**Cancelled** (closing, folded). A board with no stages cannot render a single
column, so a working default matters more here than in most seeds. It seeds only
when none exist — a renamed or deleted stage is not restored, because that would
fight the user on every restart.

## 2. `move_stage` — the board's only write

Dropping a card does four things, and the fourth is the one that gets forgotten:

1. sets `stage_id`
2. renumbers `sequence` in the target column so the drop **position** sticks
3. stamps `date_end` when the card reaches a closing stage
4. **clears `date_end` when the card leaves one**

Without (4) you get a task that is visibly back in progress and still reports as
finished in every rollup — a one-way close. The test drags a card into Done and
back out and asserts the date is gone.

## 3. `set_cell` — the timesheet's only write, and it is idempotent

The grid sends **the value the cell should now read**, never a delta. That single
choice is what lets the grid save on blur with no Save button: a double-submit, a
retry, or a stray blur cannot double someone's day. It also collapses several
pre-existing rows for the same day into one, so what the grid shows is what is
stored, and writing `0` deletes the entry rather than leaving a zero row.

Guards: negative hours and anything over 24 in one entry are rejected at the
boundary — a slipped decimal point is much cheaper to catch here than in a
payroll report.

`grid` returns the week's rows **and** the row, column and week totals. The test
asserts all three are derivable from the grid's own cells; a mismatch would mean
the screen is showing arithmetic the database doesn't support.

## 4. The screens

**Task Board** (`TaskBoard.js`, action 108, menu 137) — kanban over stages.

- Moves are applied **optimistically**: the card lands where you dropped it and
  the server call follows. On failure it springs back and the error shows —
  optimism without a rollback is just a lie.
- Drop *position* is honoured, not only the column.
- Every card carries ‹ › buttons that shift it a stage. Dragging is not an
  accessible interaction, so the same move is always available from the keyboard.
- Cards show a planned-vs-logged bar that turns red past 100%, a blocked/done
  left border, priority star, assignee and an overdue deadline (a closed task is
  never marked late).
- Quick-add per column when a single project is selected.

**Timesheet Grid** (`TimesheetGrid.js`, action 111, menu 140) — a week per
screen, one row per (project, task), one column per day. Today and weekends are
tinted; a day over 8h is flagged. Hours parse as `1.5`, `1,5` or `1:30`, because
people write durations all three ways and rejecting two of them is a papercut on
the most repeated action in the app.

Menus sit under a new **Project** app root (130), with Task Board, Projects,
Tasks, Timesheets, Timesheet Entries and Task Stages.

## 5. Bugs found on the way

1. **Menu ids 131/132 already belonged to ReportModule.** `verify_menu_ids.sh`
   caught it immediately — exactly what it exists for. Children moved to
   137–142, plus a targeted cleanup that deletes only rows in 131–136 pointing
   at *this* module's actions, so ReportModule's own menus are untouched. The new
   app root 130 was added to that script's `RESERVED_MENUS`.
2. **The timesheet add-row was crushed.** As a `<tr>` with a `colspan`, the cell
   inherits the combined width of the narrow day columns, so its button and hint
   wrapped into a one-word-per-line vertical strip. Only visible once rendered.
   Moved out of the table into a flex bar beneath it, which is immune to the
   table's column sizing.
3. **A test bug, not a code bug:** `date_trunc('week', …)` returns a *timestamp*,
   and `timestamp + 1` is an error in Postgres, not tomorrow. `pg()` swallows the
   error, so `$TUE` came back empty and two assertions failed for a reason that
   had nothing to do with the feature. The fix is `::date + 1`, plus an explicit
   guard so an empty date fails loudly next time.

## 6. Not done

- **No subtask rollup.** `parent_id` exists on the task, but nothing aggregates a
  child's hours into its parent.
- **No employee cost or billing.** `employee_id` is on the timesheet, but hours
  are not costed and nothing invoices them.
- **No per-project stage editor in the board** — stages are managed through
  Project → Task Stages.
- **Drag-and-drop is HTML5 DnD**, which does not work on touch. The ‹ › buttons
  are the fallback on a tablet, not a nicety.

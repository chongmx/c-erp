#pragma once
// =============================================================
// modules/help/HelpContent.hpp — the shipped help articles (docs/101)
//
// Kept out of HelpModule.cpp so the prose can be edited without reading past
// schema and SQL to find it. Bodies are markdown; the client renders them.
//
// A section is a row with an empty body and is_section = true. Articles point
// at their section by slug. Slugs are stable and public — an AI assistant will
// cite them back to the user ("see Filling in your timesheet"), so renaming one
// breaks a link somebody may have followed.
// =============================================================
namespace odoo::modules::help {

struct HelpSeed {
    const char* book;        ///< the tab this belongs to
    const char* bookLabel;
    const char* parent;      ///< section slug, or "" when this IS a section
    const char* slug;
    const char* title;
    int         sequence;
    const char* keywords;
    const char* body;        ///< markdown; empty for a section
};

/// One book per ERP module. Listed here rather than discovered from written
/// articles so the tab bar shows the whole system, including the modules whose
/// help has not been written yet — a visible "not documented" tab is more
/// honest, and more useful, than a module silently missing from the bar.
///
/// The bar scrolls horizontally, so this list can grow without redesigning it.
struct HelpBook { const char* slug; const char* label; int sequence; };

static const HelpBook kHelpBooks[] = {
    {"project",   "Project",       10},
    {"parts",     "Parts",         20},
    {"product",   "Products",      30},
    {"stock",     "Inventory",     40},
    {"sale",      "Sales",         50},
    {"purchase",  "Purchase",      60},
    {"account",   "Accounting",    70},
    {"mrp",       "Manufacturing", 80},
    {"rental",    "Rental",        90},
    {"hr",        "Employees",    100},
    {"report",    "Reporting",    110},
    {"base",      "Contacts",     120},
    {"settings",  "Settings",     130},
    {"help",      "Using Help",   900},
};
static constexpr int kHelpBookCount = sizeof(kHelpBooks) / sizeof(kHelpBooks[0]);

static const HelpSeed kHelpSeeds[] = {

// ============================ PROJECT ============================
{"project","Project","","project-start","Getting started",10,"",""},
{"project","Project","","project-daily","Daily use",20,"",""},
{"project","Project","","project-track","Tracking time and progress",30,"",""},
{"project","Project","","project-ref","Reference",40,"",""},

{"project","Project","project-start","project-overview","What the Project app is for",10,
 "project overview purpose modules menu what is",
R"MD(The **Project** app tracks work that has stages and takes time: a customer job, an
internal build, a maintenance run. It gives you three things.

- A **board** showing where every piece of work currently is.
- A **record** of who is doing what, and when it is due.
- **Hours logged against tasks**, so you can compare what you estimated with what
  the work actually cost.

## Where everything lives

Open **Project** from the main menu. Six entries:

| Menu | Use it to |
|---|---|
| **Task Board** | See and move work. This is the day-to-day screen. |
| **Projects** | Create and edit projects. |
| **Tasks** | The full task list — filter, sort, group, bulk-edit. |
| **Timesheets** | Enter hours, a week at a time. |
| **Timesheet Entries** | Every individual hour record, for reporting. |
| **Task Stages** | The columns your board shows. |

## The shape of the data

A **project** contains **tasks**. Each task sits in one **stage**. Hours are
logged as **timesheet entries** against a project, and usually against a task.

    Project  →  Task  →  Timesheet entries
                 ↑
               Stage (which column it's in)

## When not to use it

- For a shopping list of one-off to-dos with no hours and no stages, the
  overhead is not worth it.
- For making things, use **Manufacturing** — a manufacturing order tracks
  materials and work centres, which a task does not.
)MD"},

{"project","Project","project-start","project-first-project","Create your first project",20,
 "create new project setup first getting started reference customer manager",
R"MD(## 1. Create the project

**Project → Projects → New**

| Field | Notes |
|---|---|
| **Name** | Required. What the job is called. |
| **Reference** | A short code such as `WEB`. Shows in front of the name. |
| **Customer** | Optional. Link to a contact if this is billable work. |
| **Manager** | Optional. Who owns the project. |
| **Start / End Date** | Optional. The End Date cannot be before the Start Date. |
| **Allow Timesheets** | Leave on if people will log hours against it. |

Save.

## 2. Open the board

**Project → Task Board**, then pick your project in the first dropdown.

You will see five columns — New, In Progress, Review, Done, Cancelled. Those are
the default stages; see *Stages and how work moves* to change them.

## 3. Add your first tasks

At the bottom of any column there is a **+ New task…** box. Type a name and
press **Enter**. The task is created in that column.

> The quick-add box only appears when you have selected **one** project. A task
> must belong to a project, and "All projects" does not say which one.

For anything beyond a name — an estimate, an assignee, a deadline — click the
card to open the full task form, or use **Project → Tasks → New**.
)MD"},

{"project","Project","project-start","project-stages","Stages and how work moves",30,
 "stages columns kanban workflow closing done cancelled fold sequence",
R"MD(Stages are the columns on your board. Five ship by default:

| Stage | Closing? | Notes |
|---|---|---|
| New | no | Where quick-added tasks land. |
| In Progress | no | |
| Review | no | |
| **Done** | **yes** | |
| **Cancelled** | **yes** | Shown dimmed (folded). |

## Closing stages matter

A stage marked **Closing** means "this work is finished". Two things follow:

- Moving a card **into** a closing stage stamps its completion date.
- Moving a card **back out** clears that date again.

So a task you drag out of Done is genuinely reopened — it stops counting as
finished everywhere, not just on the board.

Anything that reports "done" counts *closing stages*, not the card's colour.

## Shared vs per-project stages

A stage with **no project set** is shared by every project. That is how the
defaults work, and it is usually what you want.

To give one project its own workflow, create stages with that project filled in.
The board then shows the shared stages **plus** that project's own.

## Editing stages

**Project → Task Stages.** Order is controlled by **Sequence** (low first).

> **Don't delete a stage that has tasks in it.** Deleting sets those tasks'
> stage to empty and they disappear from every column. Untick **Active**
> instead — the stage stops being offered, and existing tasks keep their place.
)MD"},

{"project","Project","project-daily","project-task-board","Using the task board",10,
 "board kanban drag drop move card columns filter assignee quick add keyboard touch",
R"MD(**Project → Task Board** is where work moves.

## Moving a task

- **Drag** the card to another column.
- Or use the **‹ ›** buttons on the card, which appear when you hover it.

Both do the same thing. The buttons exist because dragging does not work on a
touch screen and is not usable from a keyboard.

**Where you drop matters.** Drop a card at the top of a column and it stays at
the top — that is how you prioritise within a stage.

If a move fails — connection lost, task deleted by someone else — the card
returns to where it was and an error appears. The board never quietly keeps a
move the server rejected.

## Reading a card

| What you see | What it means |
|---|---|
| **★** before the title | High priority |
| Grey / green / red left edge | Kanban state: normal / done / blocked |
| Small grey line under the title | Project name (only when viewing All projects) |
| Name badge | Who it is assigned to |
| Date | Deadline — **red** if overdue. A task in a closing stage is never marked late. |
| `3/8h` | Hours logged / hours planned |
| Thin bar | Progress against the estimate; **red** once logged exceeds planned |

## Filtering

Two dropdowns at the top: **project** and **assignee**. Pick a person to get
their board across every project.

## The header numbers

`6 open · 1 done · 1 blocked · 18.5 / 53 h` — counts for whatever you have
filtered to, and total hours logged against total hours planned.

## Column headers

Each column shows how many cards it holds and the total planned hours in it —
useful for spotting a stage that has quietly become a queue.
)MD"},

{"project","Project","project-daily","project-tasks","Tasks in detail",20,
 "task fields priority deadline kanban state blocked planned hours parent subtask description",
R"MD(Open a task by clicking its card, or from **Project → Tasks**.

## Fields

| Field | Notes |
|---|---|
| **Task** | Required. |
| **Project** | Required. A task cannot exist without one. |
| **Stage** | Which column it sits in. |
| **Assigned To** | A user. |
| **Customer** | Optional, if this task is for a specific contact. |
| **Deadline** | Shows on the card; turns red when overdue. |
| **Planned Hours** | Your estimate. Cannot be negative. |
| **Priority** | `0` normal, `1` high — high shows a ★. |
| **Kanban State** | `normal`, `done` or `blocked`. |
| **Parent Task** | For subtasks. See the caveat below. |
| **Description** | Free text. |
| **Active** | Untick to archive rather than delete. |

## Kanban state is not the stage

This is the distinction people trip over.

- **Stage** = where the work is in the workflow. *In Progress.*
- **Kanban state** = how it is going right now. *Blocked.*

A task can be **In Progress** *and* **blocked** at the same time — it is being
worked on, and something is in the way. Marking it blocked colours the card red
and adds it to the "blocked" count in the header, without moving it backwards.

## Working in bulk

**Project → Tasks** is the full list. Group by stage or assignee, filter, and
edit many at once — faster than dragging cards one at a time when you are
reorganising.

## Subtasks

**Parent Task** links a task to another. Be aware: **hours and estimates do not
roll up** to the parent yet. The link is for organisation only.
)MD"},

{"project","Project","project-daily","project-timesheets","Filling in your timesheet",30,
 "timesheet hours grid week enter log time format 1:30 replace save row daily total",
R"MD(**Project → Timesheets** shows one week at a time: a row per project and task, a
column per day.

## Entering hours

Click a cell, type, and click away. It saves immediately — there is no Save
button.

Three formats all mean an hour and a half:

    1.5      1,5      1:30

## Typing replaces — it never adds

This is the rule worth remembering. The number you type is what the cell will
read afterwards. Type `8`, then type `8` again, and you still have 8 hours — not
16. That means a slow connection, a double-click or a stray retry cannot inflate
your day.

To remove an entry, clear the cell or type `0`.

## Adding a row

The bar under the grid: choose a **project**, optionally a **task**, then
**Add row**. The row appears straight away. It is saved as soon as you put hours
in it — an empty row is not stored.

## Moving between weeks

**‹** and **›** step a week at a time; **This week** jumps back to today. The
current day's column is highlighted, and weekends are shaded.

## The totals

- **Right-hand column** — that row's week.
- **Bottom row** — that day across all rows. It turns amber above 8 hours.
- **Top right** — the whole week.

All three are calculated on the server from the same entries, so they always
agree with each other.

## Limits

A single entry cannot be negative, and cannot exceed 24 hours. Both are almost
always a slipped decimal point, and are much cheaper to catch here than in a
report later.
)MD"},

{"project","Project","project-track","project-progress","Planned vs logged hours",10,
 "estimate planned logged progress bar over budget red",
R"MD(Two numbers, everywhere in the app:

- **Planned** — the estimate you put on the task.
- **Logged** — the sum of timesheet entries against that task.

The card shows them as `logged/planned` with a bar beneath. The bar fills as
hours are logged and **turns red once logged passes planned**.

## Reading it honestly

Red does not mean somebody did badly. It means the estimate was low. The useful
question is *why* — was the task bigger than it looked, or was it the wrong
shape to estimate at all?

A task with **no planned hours** shows no bar. That is deliberate: a progress bar
against an estimate of zero would be meaningless, not 100%.

## Where the totals come from

The board header aggregates every task in your current filter. So filtering to
one person shows their planned and logged hours; clearing the filter shows the
project's.
)MD"},

{"project","Project","project-track","project-reporting","Where the numbers come from",20,
 "reporting stats pivot graph group by analysis closed count",
R"MD(## The board header

Counts of open, done and blocked tasks, plus hours. **Done** means the task's
stage is a **closing stage** — not the card's kanban state. A card coloured green
but sitting in *In Progress* counts as open, because it is.

## Timesheet reporting

**Project → Timesheet Entries** is the raw list: one row per entry, with project,
task, user, date and hours. From there you can group and aggregate — by project,
by person, by month — and switch to pivot or graph views for a chart.

Use this rather than the weekly grid when you want a question answered. The grid
is for *entering* hours; this list is for *reading* them.

## Task reporting

**Project → Tasks**, grouped by stage or assignee, gives workload at a glance.
Group by stage to see where work is piling up.
)MD"},

{"project","Project","project-ref","project-walkthrough","A worked example, end to end",10,
 "example walkthrough tutorial scenario complete",
R"MD(A small job for a customer, from setup to review.

## Monday — set it up

**Projects → New**: name `Rewire bench PSU`, reference `PSU`, customer `Acme`,
Allow Timesheets on. Save.

**Task Board**, pick *Rewire bench PSU*, and quick-add four tasks into **New**:

    Strip old harness
    Source replacement connectors
    Rewire and dress
    Test and sign off

Open each one and add an estimate: 3h, 2h, 6h, 3h. Assign them.

Board now reads: **New 4 · 14h**.

## Tuesday — start work

Drag *Strip old harness* to **In Progress**. Open **Timesheets**, find the row
for that task, and put `3` under Tuesday.

The card now shows `3/3h` with a full bar.

## Wednesday — something is in the way

The connectors are back-ordered. Open *Source replacement connectors*, set
**Kanban State** to `blocked`. The card turns red and the header shows
`1 blocked`. It stays in its stage — it has not gone backwards, it is stuck.

## Thursday and Friday — the bulk of it

*Rewire and dress* goes to **In Progress**. You log `4` on Thursday and `5.5` on
Friday — but you estimated 6. The bar goes **red** at `9.5/6h`.

That is the useful signal: dressing a harness took longer than the estimate
assumed.

## Following Monday — close it out

Everything moves to **Done**. Each card's completion date is stamped as it
lands.

## The review

Board header: **0 open · 4 done · 17.5 / 14 h**.

You planned 14 hours and it took 17.5. Next time you quote a rewire, you have a
real number instead of a guess — which is the entire point of logging hours.

> If you later reopen one of those tasks by dragging it out of Done, its
> completion date clears and it counts as open again.
)MD"},

{"project","Project","project-ref","project-faq","Questions and gotchas",20,
 "faq problems troubleshooting cannot delete archive touch tablet billing",
R"MD(**I dragged a task to Done and back out. Is it still counted as finished?**
No. Leaving a closing stage clears the completion date, so it counts as open
again everywhere.

**The + New task box isn't there.**
Select a single project at the top. A task must belong to one, and "All
projects" does not say which.

**Can my hours get doubled by accident?**
No. A timesheet cell stores what you typed, it does not add to what was there.
If a number looks doubled, you most likely had two separate entries for the same
task and day from before — the next save merges them into one.

**I deleted a project and lost its tasks and hours.**
Deleting a project deletes its tasks and their timesheet entries. To put a
project away without losing anything, untick **Active** instead.

**Dragging doesn't work on my tablet.**
Use the **‹ ›** buttons on the card. Browser drag-and-drop does not work on
touch screens.

**Can I invoice the hours I've logged?**
Not yet. Hours are recorded and reported, but they are not costed and nothing
bills them.

**Do subtask hours add up into the parent?**
Not yet. **Parent Task** organises tasks; it does not roll up estimates or hours.

**Two people edited the same task. What happens?**
The last write wins. There is no per-field merge.
)MD"},

// ============================ USING HELP ============================
{"help","Using Help","","help-basics","About this help",10,"",""},

{"help","Using Help","help-basics","help-how-to-use","Finding your way around",10,
 "help navigation sidebar tree search tabs resize collapse",
R"MD(## The layout

- **Tabs** across the top are *books* — one per area of the system.
- The **left sidebar** is the contents of the book you are in. Click a section to
  fold it; click an article to read it.
- The **right sidebar** is reserved for the assistant (see below).

Both sidebars can be **dragged by their inner edge** to resize, and **collapsed**
with the button in their header. Your sizes are remembered on this browser.

## Search

The box above the tree filters the current book as you type, matching titles,
keywords and body text. Clear it to get the full contents back.

## Deep links

Every article has its own address. Copy the URL while reading one and it will
open straight back to it — useful in a message to a colleague.
)MD"},

{"help","Using Help","help-basics","help-assistant","The assistant panel",20,
 "ai assistant chat ask question future",
R"MD(The right-hand panel is where an **AI assistant** will answer questions about
this system, grounded in these help articles.

It is not connected yet. The panel is present, and the help content is already
stored in the shape it needs — one row per article, each with a stable slug, a
title, keywords and a markdown body — so answers can be retrieved from specific
articles and **cited back to you** rather than invented.

Until it is wired up, the panel lists the articles most related to whatever you
are reading, which is the same retrieval step the assistant will use.
)MD"},

};

static constexpr int kHelpSeedCount = sizeof(kHelpSeeds) / sizeof(kHelpSeeds[0]);

} // namespace odoo::modules::help

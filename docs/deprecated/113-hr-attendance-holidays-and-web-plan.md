# 113 — HR Attendance, Holidays, and the web module

**Status:** phases 1, 2 and 3a built and tested. Phase 3b: quote requests built;
repeat-order and statements still planned, not built.

| Phase | State | Tests |
|---|---|---|
| 1 — Attendance | built | `tests/integration/hr/attendance` (28) |
| 2 — Holidays | built | `tests/integration/hr/holidays` (50) |
| 3a — Staff kiosk | built | `tests/integration/hr/kiosk` (42) |
| 3b — Portal quote requests | built | `tests/functional/09-portal` §13 |
| 3b — Repeat order, statements | **not built** | — |

Help: `hr-attendance`, `hr-timeoff`, `hr-kiosk`, `sale-portal-quotes`.

Doc 112 listed `hr_attendance` and `hr_holidays` under "deliberately excluded".
That call was made on the assumption they were HR-suite nice-to-haves. They are
not — without them the ERP cannot answer *"was this person at work"* or *"is this
person entitled to be away"*, which is the floor for payroll, for costing a job,
and for scheduling production. This document supersedes that exclusion.

---

## Phase 1 — Attendance

**One question: who is at work right now, and for how long today.**

### Schema — `hr_attendance`

| Column | Notes |
|---|---|
| `employee_id` | FK `hr_employee`, `ON DELETE CASCADE` |
| `check_in` | `TIMESTAMP NOT NULL` |
| `check_out` | `TIMESTAMP NULL` — NULL means *still in* |
| `worked_hours` | `NUMERIC(8,2)`, **derived server-side on check-out**, never accepted from the client |
| `company_id`, `create_date`, `write_date` | |

Three guards, all in the database rather than in a handler, because a handler is
one code path and a constraint is all of them:

1. **`CHECK (check_out IS NULL OR check_out > check_in)`** — no negative shifts.
2. **A partial unique index on `(employee_id) WHERE check_out IS NULL`** — an
   employee can have at most **one open attendance**. This is the double-clock-in
   guard, and it makes the race between two kiosk taps impossible rather than
   unlikely.
3. **No overlapping closed intervals** per employee — enforced in the check-out
   path, since it needs a range comparison a unique index cannot express.

### API — `hr.attendance`

| Method | Does |
|---|---|
| `action_check_in(employee_id)` | Opens an attendance. Refuses if one is already open. |
| `action_check_out(employee_id)` | Closes the open one and computes `worked_hours`. Refuses if none is open. |
| `action_toggle(employee_id)` | What a kiosk button calls — in if out, out if in. Returns the resulting state. |
| `attendance_state(employee_id)` | `{state: "checked_in"\|"checked_out", since, worked_hours_today}` |

`worked_hours` is computed from the stored timestamps, so a client that lies
about the duration changes nothing.

---

## Phase 2 — Holidays

**Two questions: is this person entitled to be away, and are they away.**

### Schema

**`hr_leave_type`** — `name`, `code`, `requires_allocation` (bool), `is_paid`
(bool), `color`, `max_days_per_request` (0 = unlimited), `active`, `company_id`.
Seeded: Annual, Sick, Unpaid, Emergency.

**`hr_public_holiday`** — `name`, `date`, `company_id`, unique on
`(date, company_id)`. **Deliberately seeded empty**: Malaysian public holidays
vary by year and by state, and a wrong date silently miscounts every leave
request that spans it. The admin enters them; the day-counter uses them.

**`hr_leave_allocation`** — `employee_id`, `leave_type_id`, `number_of_days`,
`date_from`, `date_to` (nullable = open-ended), `state`
(`draft→confirm→validate`/`refuse`), `notes`.

**`hr_leave`** — the request: `employee_id`, `leave_type_id`, `date_from`,
`date_to`, `number_of_days` (**derived**), `state`
(`draft→confirm→validate`/`refuse`/`cancel`), `reason`.

### The rules that are actually load-bearing

1. **Working days, not calendar days.** `number_of_days` excludes Saturdays,
   Sundays and any `hr_public_holiday` in range. Computed server-side; a request
   spanning a weekend does not burn weekend days.
2. **No overlapping approved leave** for one employee — you cannot be on leave
   twice, and a second request covering the same dates is refused at approval.
3. **Allocation is a ceiling.** When the type sets `requires_allocation`,
   approving a request that would exceed *approved allocations minus already
   approved leave* is refused. **The balance can never go negative.**
4. **State-machine guards.** Approve only from `confirm`; no double-approve; a
   refused or cancelled request cannot be approved; cancelling an approved leave
   returns the days to the balance.
5. `date_to >= date_from`, enforced by CHECK.

### API — `hr.leave`

`action_confirm`, `action_approve`, `action_refuse`, `action_cancel`,
`action_reset_draft`, plus `leave_balance(employee_id[, leave_type_id])`
returning `{allocated, taken, pending, remaining}` per type.

`hr.leave.allocation` gets `action_confirm` / `action_approve` / `action_refuse`.

### Menus

Under **Employees** (parent 80): Attendance, Time Off, Allocations; under
**Configuration** (83): Leave Types, Public Holidays. Menu ids **404–410**,
action ids **118–124** — both verified free before use (the ranges near the HR
block are exhausted; see the id survey in this doc's commit).

---

## Phase 3 — The web module (planned, not built)

Two audiences that today have no front door of their own.

### 3a. Staff kiosk — `/kiosk` — BUILT

A tablet by the door. A keypad, a PIN, one action, showing who you are and
today's hours.

**Built as the PIN option below.** `hr_employee.pin_hash` holds a PBKDF2 hash
set through `hr.employee.set_pin` (digits only, min 4) and cleared through
`clear_pin`; `has_pin` is the only readable fact. `POST /kiosk/api/punch`
verifies the PIN against every active employee that has one, toggles that
person's attendance, and returns their name, direction and hours today. The
route issues **no cookie and no session**, exposes no way to list employees, and
throttles repeated failures per IP (8 in 180s). A USB badge reader that types
digits and presses Enter works unmodified.

The open question was **authentication**, and it decided the design:

| Option | Trade-off |
|---|---|
| Full staff login per tap | Secure, and unusable on a shared tablet — nobody types a password twice a day. |
| **Employee PIN** (recommended) | A 4–6 digit PIN on `hr_employee`, hashed like a password. The kiosk itself is an unauthenticated page; the PIN authorises the single toggle action and nothing else. |
| Badge / QR scan | Best UX, needs hardware; the PIN design accepts a scanned code in the same field, so this is an add-on, not a rewrite. |

**Constraint:** the kiosk must be able to do *exactly one thing* — toggle the
attendance of the employee whose PIN was entered. It must not carry a session
that can read the ERP. A leaked kiosk is then worth one person's clock, not the
company's data.

### 3b. Customer portal extensions

`/portal` already does invoices, orders, deliveries, PDFs and payment proofs
(doc 109 §09-portal covers it with 92 checks). What it cannot do:

1. **Request a quote** — **BUILT.** `POST /portal/api/quote` takes
   `{lines:[{product_id, quantity}], note}` and creates a *draft* `sale.order`
   with origin `Portal Quote Request`. Prices come from the product; a
   `price_unit` in the request is ignored. Non-sellable or unknown products are
   dropped, and a request with no usable line is refused rather than left as an
   empty order for staff to puzzle over. Capped at 50 lines.

   The hard-coding this was blocked on is also fixed: `/portal/api/request` now
   resolves the customer's **own** company and a sale journal belonging to it,
   instead of the literals `journal_id=1, company_id=1` — which on a
   multi-company database filed every portal request into company 1's books.
   The invoice line now takes the same journal as its header.
2. **Place a repeat order** — re-order a previous order's lines, again as a
   draft for staff confirmation.
3. **Download statements** — an account statement PDF for a date range.

**The rule for all three:** the portal proposes, staff dispose. A customer-facing
route must never post to the ledger or confirm an order, because the portal's
authentication is a partner password, not a staff session.

### 3c. Sequencing

Kiosk first — it is small, self-contained, and completes Phase 1. Portal quote
requests next, after the `/portal/api/request` company/journal hard-coding is
fixed. Statements last.

---

## What this does not do

- No approval **hierarchy** — any user with HR rights approves. A manager-only
  chain needs `hr_employee.parent_id` walking and is deliberately out of scope.
- No accrual **schedules** — allocations are granted as a number, not accrued
  monthly.
- No payroll, no attendance→payslip link.
- No half-days. `number_of_days` is whole working days; half-day support changes
  the counter's return type and is better added deliberately than bolted on.

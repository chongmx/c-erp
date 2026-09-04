# 058 — Recurring billing, recurring expenses, cashflow forecast

**Date:** 2026-08-05
**Implements:** `054` phases 5 and 7 · `057` §1 (revised)
**Status:** ✅ Complete and verified

---

## What was built

| | |
|---|---|
| Migration 812 | `contract_id` nullable, `partner_id` + `billing_mode` on the line, `rental_next_period()` |
| `RentalBilling` | recurring invoicing — cron and manual, one code path |
| `RentalExpenses` | recurring expense templates → dated occurrences |
| `RentalForecast` | month-by-month cashflow, derived on every request |
| `verify_rental_billing.sh` | 26 checks |
| `verify_rental_cashflow.sh` | 25 checks |

Three routes: `POST /rental/billing/run`, `POST /rental/expenses/generate`,
`GET /rental/cashflow`.

---

## Contracts became optional, not mandatory

Per `057` §1 there is no `rental.tenancy` migration. `contract_id` is now **nullable** and
the customer moved onto the line, so a walk-in needs no contract to exist. What a contract
does is switch on recurring billing:

```
billing_mode = 'manual'      invoice raised by hand when they pay
billing_mode = 'recurring'   the cron bills every period until cancellation
```

`billing_mode` is a column rather than "recurring if `next_period_start IS NOT NULL`". The
implicit version works and produces exactly one bug — *why is this customer not being
invoiced?* — answerable only by reading the cron's SQL.

A trigger enforces that a line attached to a contract belongs to that contract's customer.
Without it a contract could quietly invoice the wrong person.

---

## The period-drift bug, avoided by construction

`date + interval '1 month'` **drifts**:

```
Jan 31 + 1 month = Feb 28
Feb 28 + 1 month = Mar 28     <- the 31st is gone, permanently
```

A tenancy anchored on the 31st would silently walk back to the 28th and never recover.
`rental_next_period(from, anchor, months)` anchors to the day of month and clamps to the
month length instead:

```
Jan 31 -> Feb 28 -> Mar 31 -> Apr 30
```

It is a SQL function so the engine, the forecast and the tests share **one**
implementation, and the test asserts the naive form really does differ — otherwise the
guard could be removed without anything failing.

---

## Billing

Bills **in advance**: an invoice is raised `billing_lead_days` (default 7) before the
period starts, so the tenant has time to pay before occupying. This is also what makes
cancellation free of proration — they have already paid for the period they are in.

Grouped by **`(partner, period_start, company, currency)`**. Three units taken on three
dates produce three invoices with three due dates; two taken on the same day combine onto
one invoice with two lines. No configuration — it falls out of the data.

**One transaction per group**, so a failure on one customer cannot half-bill another or
abort the run. Asserted by injecting a real failure and checking the healthy customer is
still billed and the failed group left nothing behind.

Advance credit is consumed automatically: a customer who paid six months up front has it
drawn down one period at a time, on their own billing date, through `PaymentAllocation`
rather than a second implementation.

### The guard, proved both ways

`ir.cron` is at-least-once by design, so the **scheduler is not** what prevents
double-billing — `UNIQUE (contract_line_id, period_start)` is:

```
with the constraint:     invoices 1 -> 1   (reported as skipped, not failed)
constraint dropped:      invoices 1 -> 2
```

An idempotency test that has never seen the failure it prevents is asserting that today's
code does what today's code does.

---

## Recurring expenses are budgets

A template (`is_recurring = TRUE`) carries the **budgeted** amount — wifi RM 200/month.
The cron clones it into dated children, which are the **actuals**, editable when the real
bill arrives.

That split is what makes the forecast honest: past months use what was spent, future
months use what was budgeted. One number for both would make the forecast quietly rewrite
history every time a bill came in higher than expected.

**Catch-up is deliberate.** A template three months overdue generates three occurrences,
not one. A cron that was down for a week must not silently lose a month of expense — the
forecast would then be wrong in the direction that flatters.

---

## Cashflow forecast — nothing is stored

Every figure is derived on request from the tenancies, the templates and the open
invoices. A stored forecast is a number that was right when written and silently wrong
from the next rate change onward, and a stale forecast is worse than none because it is
trusted.

Income has two sources, kept apart so they cannot double count:

| | |
|---|---|
| **receivable** | already invoiced, unpaid — counted in the month it falls **due** |
| **projected** | not yet invoiced — counted from `next_period_start` |

Because billing is in advance, `next_period_start` is always the first *unbilled* period,
so the two sets cannot overlap. The test asserts this directly: billing a period moves the
amount from `projected` to `receivable` and **the month's total is unchanged**.

Walk-ins are excluded — a manually invoiced tenancy cannot be forecast, and including it
would present a guess as a projection.

The response states its assumptions rather than implying them, including "no churn
assumption: every active tenancy is assumed to continue." A forecast whose assumptions are
invisible gets read as a prediction.

---

## Four bugs found by the tests

### 1. Group fields were overwritten by the last line — a wrong invoice, silently

Grouping on `(partner, period)` alone, the group's company, currency and journal were
reassigned on **every line**, so a group took whichever line was read last. Two units
under one customer in **different currencies** would have produced a single invoice in
whichever currency happened to sort last — no error, just a wrong invoice.

Found because a failure-injection test refused to fail: the injected company on one line
was being overwritten by the other line in the same group. The fix makes company and
currency part of the grouping key, so a mismatch produces two correct invoices instead of
one wrong one.

This is the most valuable find here, and it came from a test that *passed* while proving
nothing — the reason the premise is now asserted (`groups_failed >= 1`) before the
conclusion is checked.

### 2. `ON CONFLICT` cannot infer a partial index without its predicate

`ON CONFLICT (recurrence_parent_id, date)` against an index declared
`WHERE recurrence_parent_id IS NOT NULL` raises *"no unique or exclusion constraint
matching the ON CONFLICT specification"*. Every expense template failed. The predicate has
to be repeated in the `ON CONFLICT` clause.

### 3. A negative control that proved the opposite of what it claimed

The first version dropped the unique index and re-ran the generator, expecting duplicates.
It got none — because `ON CONFLICT` *infers* that index, so without it the statement
errors rather than duplicating. The run looked safe for entirely the wrong reason.

Rewritten to attempt a **raw insert** — the shape a concurrent generator or a repair script
would take — with the index absent and again with it present. Duplicate accepted without
it, rejected with it. That is the claim actually being made.

### 4. Two column-name assumptions

`account_move.due_date`, not the reference ERP's `invoice_date_due`; and `account_move_line` has no
`discount` column at all. The second mattered: the discount is applied in the computed
subtotal, so writing the standard rate as `price_unit` would print RM 120.00 on a line
charging RM 102.00 with nothing explaining the gap. The **net** rate is written instead, so
`price_unit == credit` with quantity 1. Showing "RM 120.00 less 15%" needs a discount
column and belongs with committed-use pricing, not smuggled in here.

---

## Verification

```
verify_rental_billing    26 checks   drift, advance billing, grouping, idempotency
                                     + negative control, tax, isolation, walk-ins,
                                     advance credit
verify_rental_cashflow   25 checks   generation, catch-up, idempotency + negative
                                     control, projection, quarterly cadence,
                                     double-count checks, arithmetic, assumptions
```

---

## Not built, deliberately

- **Committed-use discounts** — deferred at your request. `discount_pct` exists on the line
  and `TaxEngine` already applies it; what is missing is the commitment term and the
  expiry behaviour.
- **Churn in the forecast** — every active tenancy is assumed to continue. A churn rate
  invented without data is a worse estimate than none, and the assumption is stated.
- **Late fees, clawback, notice periods** — each interacts with tax and credit notes, and
  is better specified from real cases after a few billing cycles.

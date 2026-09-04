# 051 — P4 (`ir.sequence`) and P5 (`ir.cron`)

**Date:** 2026-08-04
**Implements:** `045` P4, P5
**Status:** ✅ Both complete — build clean, migrations 980/990 applied, full regression green

---

## P4 — `ir.sequence`

### What it replaces

Raw PostgreSQL sequences created inline in `ensureSchema_()` (`sale_order_seq`,
`purchase_order_seq`, `stock_in/out/int_seq`), plus hand-built prefixes like
`"SO/" << year << "/" << setw(4) << n`.

### The two real defects it fixes

**1. `nextval()` burns numbers on rollback.** PostgreSQL sequences are deliberately
non-transactional — a number taken by a transaction that later aborts is gone. Fine for
internal references; for **tax invoices** most jurisdictions require a gapless series, and a
missing number is something an auditor asks about.

`IrSequence::nextByCode(txn, code)` allocates inside the **caller's** transaction, so the
number returns to the pool if the operation fails. A no-transaction overload remains for
internal references where throughput matters more than gaplessness.

**2. Invoice numbering had a race that produced duplicate numbers.**
`AccountModule::action_post` computed the number as:

```sql
SELECT COUNT(*) FROM account_move
 WHERE journal_id = $1 AND state = 'posted' AND EXTRACT(YEAR FROM date) = $2
```

…`+ 1`, with **no lock**. Two concurrent posts read the same count and produced the *same
invoice number* on a legal document. It also reused numbers whenever a posted move was
deleted or reset to draft. Now a row-locked `ir.sequence`, per journal, created on first post
(journals are user-defined, so the sequence cannot be seeded at migration time).

### Design

`ir_sequence`: `code`, `prefix`, `suffix`, `padding`, `number_next`, `number_increment`,
`reset_policy` (never/yearly/monthly), `last_reset_period`, `company_id`, `active`.

- **Concurrency**: `SELECT … FOR UPDATE` serialises allocators on the row. That is the cost of
  gaplessness, and the right trade for document numbers, which are low-frequency.
- **Yearly reset** is detected by comparing the stored period key *at allocation time*, not by
  a scheduled job — so it cannot be missed because the server was down at midnight.
- **Placeholders**: `%(year)s %(y)s %(month)s %(day)s`, the reference ERP convention.
- **Migration 980 seeds `number_next` from each old sequence's `last_value + 1`**, so numbering
  continues instead of restarting at 1 and colliding with existing documents.

### Verified (`scripts/verify_ir_sequence.sh`)

```
continuity     ir_sequence next=5 > highest existing SO=4        PASS
concurrency    80 allocations / 8 parallel clients
               80 rows, 80 distinct, highest 80 — no dups, no gaps PASS
rollback       allocate then ROLLBACK -> number_next back to 1     PASS
               (a PG sequence would have burned it)
cleanup        no nextval() calls remain in modules/              PASS
```

---

## P5 — `ir.cron`

### Why it is more than a timer

The mechanism already existed: `Container::startSessionEviction_()` used drogon's
`runEvery()`. What was missing is everything that makes a scheduler trustworthy for money:

| Property | Why it matters |
|---|---|
| **Persistence** | `next_run` is a column, so a job due while the server was down runs at startup instead of being silently skipped |
| **Overlap guard** | Billing that takes 90 s on a 60 s interval must not run twice concurrently and double-bill |
| **Failure handling** | A throwing job is logged, counted and **rescheduled** — never dropped, and never stalls other jobs |
| **Backoff** | Repeated failures back off exponentially (capped at 8 h) instead of hammering a broken dependency every minute — but never stop retrying. A silently disabled billing job is worse than a noisy one |
| **Missing handler** | A row with no registered handler warns **once**, not every tick |

### At-least-once, not exactly-once

Stated plainly in the header because it shapes how jobs must be written: a crash between "work
done" and "next-run written" re-runs the job. **Jobs that create documents carry their own
uniqueness constraint** — for rental billing that is `UNIQUE(contract_line_id, period_start)`
(`040` §3.2). A scheduler cannot provide this on the job's behalf.

Also: `next_run` is computed from `now()` on success, not from the previous `next_run`, so an
overdue job does not then fire repeatedly to "catch up".

### Session GC moved onto it

`session.gc` was the first user of the bare timer, so it is the first real job — running one
through the scheduler is how we know the scheduler works.

The direct timer is **retained as a safety net** at a longer interval (300 s). Session eviction
is a memory-exhaustion control (S-43); it must keep working even if someone deactivates its
cron row. Belt and braces on a control that exists to stop the process being OOM-killed.

`rental.billing` and `rental.expenses` are seeded **inactive** — their handlers do not exist
yet, and an active row with no handler would log a warning forever.

### Verified (`scripts/verify_ir_cron.sh`)

```
scheduler started                                          PASS
session.gc fires; last_run advances; next_run rescheduled  PASS
job body executed (eviction logged)                        PASS
missing handler reported once, not per tick                PASS
a broken job does not disable healthy ones                 PASS
next_run persisted; overdue job stays due across restart   PASS
```

---

## Regression after both

```
money unit 52 · roundtrip · display · precision · currency
no-double-audit · sequence · security · session
ledger integrity 10/10 exact · ViewModel compliance 42/42
```

---

## What P4/P5 unblock

| Now possible | Needs |
|---|---|
| Rental contract + invoice numbering | P4 ✅ |
| Recurring rental invoicing | P5 ✅ + the billing engine (P1/rental) |
| Recurring expenses | P5 ✅ |
| Dunning / overdue reminders | P5 ✅ + `ir.mail_server` |

Remaining before the rental module: **P1** (payment allocation, which also carries the FX
settlement work from `048` §4.6) and **P3** (tax engine). P3 is the item most likely to
overrun and is worth prototyping first.

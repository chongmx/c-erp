# 054 — Rental Module: Implementation Plan

**Date:** 2026-08-04
**Implements:** `040` §3 (data model, billing, dashboard) · `046` (UI)
**Prerequisites:** all seven complete — see `053`
**Status:** Plan, not yet started

---

## 0. What changed since `040` was written

`040` was written before P1–P7 existed. Three things are now settled that were open then,
and two are newly discovered. They change the sequence, not the design.

| | Then | Now |
|---|---|---|
| `ir.cron` | missing — hard blocker | ✅ `core/IrCron` — `registerJob`, overlap guard, backoff |
| `ir.sequence` | missing — hard blocker | ✅ `core/IrSequence` — gapless, `FOR UPDATE`, yearly reset |
| Tax engine | missing — hard blocker | ✅ `core/TaxEngine` + `TaxHelpers::applyLineTaxes` |
| Payment allocation | missing — hard blocker | ✅ `core/PaymentAllocation` — allocations as rows, residual derived |
| Money | `double` | ✅ int64 micro-units, user-configurable precision, multi-currency |
| **`ir.attachment`** | "missing (Stage 5)" | **still missing — see §7** |
| **Migration range 800–899** | assumed usable | **verified usable — see below** |

**Migration numbering is safe.** P2–P5 took 900–999 and P3/P1 took 1000–1010, so an 800-series
migration is numerically *below* what is already applied. That would be fatal with a
"current version" high-water mark. It is not, here: `MigrationRunner::runPending` builds a
`std::set<int>` of applied versions and skips by set membership
(`core/infrastructure/MigrationRunner.cpp:36-57`), so a lower version added later still runs.
Checked rather than assumed, because getting this wrong means the rental tables silently
never get created in production.

---

## 1. Sequencing principle

`040` §3.8 and `046` §8 both list the event log near the **end**. That ordering is wrong, and
it is the one change I want to make to the plan.

Every step after the schema emits domain events — contract activated, unit assigned, invoice
generated, payment applied, contract cancelled. If `rental.event` arrives at step 8, then
steps 3–7 are written without it and each one has to be revisited to thread `emit()` calls
back in. Retrofitted instrumentation is also where events get *missed*, because the person
adding them is no longer the person who knew which branches mattered.

So: **the event table and its `emit()` helper land in phase 2, immediately after the schema,
and every phase after that emits as it is written.** The event *feed UI* stays late, where
`046` put it — that part genuinely is a leaf.

The same logic applies to `UNIQUE (contract_line_id, period_start)`. It ships in the first
billing commit, never as a follow-up. A uniqueness constraint added after data exists is a
migration that can fail on real rows; added before, it is free.

---

## 2. Phases

### Phase 0 — frontend groundwork (0.5 d)

Not rental code, but it blocks clean rental UI.

`ActionView` dispatches custom views through a `t-elif` ladder
(`web/static/src/app.js:8979-8990`). Rental adds three more models to it. Replace the ladder
with the `CUSTOM_VIEWS` lookup from `046` §0 so rental adds a map entry, not a rung.

- **R0.1** `CUSTOM_VIEWS` map refactor
- **R0.2** `rental.css` — tokens + light/dark scopes, from the validated palette in `046` §3

Both pay for themselves on any later view, so they are worth doing even if rental slips.

### Phase 1 — schema and models (1 w)

Migrations **800–810**, one concern each:

| # | Table |
|---|---|
| 800 | `rental_unit_type` |
| 801 | `rental_unit` |
| 802 | `rental_contract` |
| 803 | `rental_contract_line` |
| 804 | `rental_invoice_link` — **with the UNIQUE constraint** |
| 805 | `rental_expense_category` |
| 806 | `rental_expense` — with `UNIQUE (recurrence_parent_id, date)` |
| 807 | `rental_event` |
| 808 | seed: unit types, expense categories |
| 809 | `ir_sequence` rows — contract numbering (`RENT/%(year)s/`) |
| 810 | `ir_cron` rows — daily billing, daily recurring-expense generation |

Models per `CLAUDE.md` PERF-E (`.hpp` declaration / `.cpp` implementation). `GenericViewModel<T>`
for the plain-CRUD models (`rental.unit.type`, `rental.unit`, `rental.expense.category`,
`rental.expense`) so they inherit auditing, rule merging and OCC for free — per `040` §1.2, the
module must *inherit* the audited path, not hand-roll one. Only `rental.contract`,
`rental.contract.line` and the dashboard justify custom ViewModels.

All money columns BIGINT micro-units and `markScaled()`-registered. Every field that the UI
writes must be in `registerFields()` — **the `tax_ids_json` bug in `053` was exactly this
mistake**, and it produced no error anywhere.

**Verification:** `scripts/verify_rental_schema.sh` — tables exist, constraints present, money
columns are BIGINT, seed rows landed. Plus a ledger-integrity clause extending
`verify_ledger_integrity.sql` §6 to cover the new money columns.

### Phase 2 — event log (2 d)

`rental.event` + `RentalEvents::emit(txn, type, ctx…)`, taking the caller's transaction so an
event cannot commit when the thing it describes rolled back.

Kept **separate from `audit_log`**: that is CRUD forensics (who wrote which row), this is
business narrative. Conflating them yields a log that is bad at both.

**Verification:** emit inside a rolled-back transaction leaves no row.

### Phase 3 — units and state derivation (1 w)

`rental.unit.type`, `rental.unit`, and the rule that matters:

> `state` is **derived** from active contract lines, never hand-edited — except
> `maintenance` and `retired`, which are operator facts rather than consequences.

Deriving it is what prevents the double-let bug. A stored, hand-edited state and a contract
line that disagrees is the single most common defect in this domain.

UI: `RentalUnitListView` then `RentalUnitGrid` (`046` §4) — colour **plus glyph**, never state
by colour alone; per-cell hover tooltips; list view as the accessible fallback.

**Verification:** assigning a unit to an active line flips it to `occupied`; ending the line
releases it; a second contract line cannot claim an occupied unit.

### Phase 4 — contract lifecycle (1.5 w)

`rental.contract` + `rental.contract.line`, with **per-line dates**. Each line carries its own
`date_start`, `billing_anchor_day` and `next_invoice_date`. There is deliberately **no
contract-level billing date** — the requirement (customers renting several units from
different dates, therefore different due dates) falls straight out of the data instead of
needing configuration.

Actions: activate, add line, end line, cancel contract. Contract numbering from `ir.sequence`
inside the transaction — never `COUNT(*)+1`, which P4 removed from two other modules and P1
removed from a third.

Emits: `contract_created`, `contract_activated`, `line_added`, `unit_assigned`, `rate_changed`,
`contract_cancelled`.

**Verification:** per-line `next_invoice_date` derives correctly from differing start dates;
cancellation closes lines and releases units; concurrent activation cannot double-let.

### Phase 5 — billing engine (1.5 w) — the core

`RentalBilling.hpp/.cpp`, run by the daily cron **and** by a manual "Generate invoices now"
action through the *same* function with a date parameter. Never a second implementation — a
manual path that drifts from the scheduled one is how double-billing gets discovered in
production.

1. Select active lines where `next_invoice_date <= as_of`
2. Group by `(contract_id, next_invoice_date)` — shared due dates combine onto one invoice,
   differing ones produce separate invoices
3. Compute `period_start` / `period_end` from `billing_period`
4. Apply `proration_policy` to the **first** period only, via `Money::prorate`
5. Create the `out_invoice`; taxes from `unit_type.tax_ids` through `applyLineTaxes`
6. Insert `rental.invoice.link` — **the UNIQUE constraint aborts a duplicate run**
7. Advance `next_invoice_date`, set `invoiced_through`
8. Emit `invoice_generated`

**One transaction per contract group**, so a failure on one customer cannot half-bill another;
log and continue, never abort the run.

`ir.cron` is at-least-once by design, so the UNIQUE constraint — not the scheduler — is the
thing that makes double-billing impossible. Also true if the process restarts mid-run or
someone clicks the manual action twice.

**Verification** (`scripts/verify_rental_billing.sh`), and this is the suite I care most about:
- running the same billing date twice produces **one** invoice, not two
- three lines with three start dates produce three invoices with three due dates
- two lines sharing a due date combine onto one
- proration applies to the first period only
- tax lands on the invoice and the entry balances
- a failure injected into contract B does not affect contract A
- **negative control**: with the UNIQUE constraint dropped, the double-run *does* double-bill —
  otherwise the idempotency test proves nothing

### Phase 6 — payments, deposits, overdue (1 w)

Mostly wiring, since P1 built the hard part.

- **Advance payment** — an unallocated credit already falls out of `PaymentAllocation`;
  auto-apply oldest-open-first on the next generated invoice, emit `payment_applied`
- **Late payment** — ageing buckets computed in the dashboard query, never stored
- **Deposit** — held as a **liability**, tracked `held → refunded/forfeited`, and **never
  auto-applied to rent**. That is a decision, not a default

Note for this phase: `053` fixed the allocator so outbound payments match vendor bills. That
fix is what makes phase 7's recurring supplier expenses settle at all.

### Phase 7 — expenses (1 w)

One-off and recurring. A recurring expense is a **template row** (`is_recurring`) that the cron
clones into dated children, with `UNIQUE (recurrence_parent_id, date)` — the same idempotency
discipline as billing, for the same reason.

`unit_id` is nullable (site-wide vs per-unit); `contract_id` nullable (rechargeable or not).

> **Gap: `ir.attachment` does not exist.** `040` §3.1 assumed `attachment_id` on
> `rental.expense`, and §3.7 listed attachments as a Stage-5 dependency that was never built.
> Grepping the tree finds no attachment model at all. So **receipt upload and signed-contract
> storage are out of scope for v1** — the column is carried nullable so the capability can be
> added without a schema change, but nothing writes it. Building a general attachment subsystem
> is its own piece of work and should not be smuggled into the rental module, where it would be
> built rental-shaped and then have to be torn out. Flagging it rather than quietly dropping it.

### Phase 8 — dashboard (1.5 w)

**One endpoint**, `GET /rental/dashboard`, returning the whole payload from a handful of
aggregate queries, cached 60 s in the existing `TtlCache`. Not N `search_read` calls from the
frontend — that is the fastest route to a 4-second paint that hammers the connection pool.

Panels per `040` §3.4 / `046` §2. UI order: KPI tiles first (no chart code needed), then the
three charts against the already-validated palette.

Chart rules from `046` §9 stand: no dual-axis, ever; categorical hues in fixed slot order;
ageing is an **ordinal ramp in one hue**, not four categorical hues and not status red — the
buckets are a scale, not four identities.

### Phase 9 — portal (1 w)

Customer-facing: my units, my invoices, balance, payment history. No dashboard, no charts —
customers want one number and a list.

**Requires `ir.rule` record rules scoped by `partner_id`.** `RuleEngine` exists
(`core/RuleEngine.cpp`), so this is configuration rather than construction — but it must be
verified with a negative control: **log in as customer A and attempt to read customer B's
contract, and confirm it fails.** The portal is the public surface; S-41 and S-47 are both
records of what happens when a model joins the system outside the framework.

---

## 3. Order of the whole thing

```
P0  frontend groundwork      0.5 d   ─┐ can start immediately
P1  schema + models          1.0 w   ─┘
P2  event log                2   d    everything after this emits
P3  units + state derivation 1.0 w
P4  contract lifecycle       1.5 w
P5  billing engine           1.5 w    ← the core; most test effort here
P6  payments/deposits        1.0 w
P7  expenses                 1.0 w
P8  dashboard                1.5 w
P9  portal + record rules    1.0 w
                             ─────
                             ~9 weeks
```

Usable earlier than that reads: after **P3** the facility can be recorded, after **P4**
contracts can be entered, and after **P5** it is invoicing — which is the point at which it
replaces whatever is doing the job today. P6–P9 improve a system that is already running.

---

## 4. Rules this module inherits

Non-negotiable, from `CLAUDE.md` and the P1–P7 work:

| Rule | Applies here as |
|---|---|
| SEC-28 | every catch block gates `ex.what()` behind `devMode` |
| S-33 | `PoolExhaustedException` → 503, above `catch (std::exception&)` |
| PERF-E | `.hpp` declaration / `.cpp` implementation, per file |
| PERF-F | 1000-row cap on every list query |
| ARCH-1 | `REGISTER_MUTATOR` for every mutator — enforced at boot |
| ARCH-2 | UNIQUE natural key on every generated document |
| TEST-1 | tests ship in the same commit as the feature |
| P2 | money is int64 micro-units; `markScaled()` every money field |
| P3 | tax only through `TaxEngine`; round per line, then sum |

Plus one earned in `053`: **a field the UI writes must be in `registerFields()`.** The unit
tests cannot catch that class of bug — `TaxEngine` was correct and fully green while
`tax_ids_json` was being silently discarded on write. Every phase here therefore ships an
integration probe, not just unit tests.

---

## 5. Where I expect the difficulty to be

Named in advance, so they get attention rather than discovery.

1. **Date arithmetic at period boundaries.** `billing_anchor_day = 31` in February; DST and
   server timezone against `next_invoice_date <= today`; leap days. This is where billing
   engines actually break, not in the money arithmetic. Table-driven unit tests over awkward
   anchor/period combinations, in the `erp_tests` harness.
2. **Proration policy interacting with tax.** Prorating a tax-inclusive rate must still satisfy
   `base + tax == gross`. `Money::prorate` and the inclusive-by-subtraction rule already hold
   this; the test has to prove it holds for partial periods too.
3. **Cancellation mid-period.** Refund, no refund, or bill through period end — a policy
   decision, and I would rather ask than guess. Defaulting to "bill through period end, no
   refund" as the least surprising, but flagging it.
4. **Contract currency vs company base.** A USD contract generating MYR-booked invoices settled
   by MYR receipts already works (P1/P2), but rental is the first module to exercise it on a
   *recurring* basis, where a rate change between periods is normal rather than exceptional.

---

## 6. Multi-company

`040` §2 (database per company, admin switches) is **orthogonal** to this plan. Rental tables
live inside each tenant database like every other module, so nothing in phases 0–9 changes
depending on when multi-company lands. Keeping them decoupled deliberately: sequencing rental
behind a process-per-database refactor would delay the thing with actual business value behind
the thing with none of it yet.

---

## 7. Decisions (answered 2026-08-04)

### Billing is in ADVANCE

> "The users are billed before they use monthly, so, no cancellation issue."

This is the most consequential answer, and it removes a whole class of work.

`period_start` is in the **future** relative to the invoice date: the invoice for September is
raised before September begins. A tenant who cancels has already paid for the period they are
sitting in, so there is **no mid-period proration and no refund calculation** — cancellation
simply stops the next invoice from being raised.

What this deletes from §5:

- `proration_policy` collapses to one behaviour. The field stays in the schema for a future
  arrears mode, but v1 bills whole periods only, so the "prorate the first period" branch and
  its awkward-boundary tests are not built.
- Cancellation refunds, credit notes on cancellation, and refund-vs-forfeit-of-rent all go away.

What it adds:

- A **lead time**: the invoice must be raised *before* `period_start`, so billing runs on
  `next_invoice_date = period_start - lead_days` (default 7, per contract). Without a lead time,
  "bill in advance" quietly becomes "bill on day one of the period", which is not the same thing
  and gives the tenant no time to pay before occupying.
- `period_start`/`period_end` on `rental.invoice.link` now describe a **future** window. The
  UNIQUE `(contract_line_id, period_start)` guard is unchanged and just as necessary.

### Deposits: supported, optional, and the refund is a choice

> "No deposit collected. it is good to have, deposit will be fully refunded if there is any,
> but let me have options to choose to refund or not."

Built, but never automatic:

- `deposit_amount` defaults to 0 — the no-deposit case is the default path, not a special case
- held as a **liability**, never revenue, and never auto-applied to rent
- on cancellation the operator is **asked**: Refund / Forfeit / Leave held. No default action
  fires. Full refund is the normal choice, but forfeiting is a decision a person makes, so the
  system asks rather than assumes
- emits `deposit_held`, `deposit_refunded`, `deposit_forfeited`

### Advance payments: consumed monthly, at the customer's own billing date

> "advanced payment will consume the advance every month depending on customer's billing date."

When billing generates an invoice for a contract, any unallocated credit for that partner is
applied to it immediately, up to the invoice total. A tenant who pays six months up front sees
one credit drawn down by one month's rent on each of their own billing dates — which differ
per line, and therefore per invoice, exactly as §4 describes.

`PaymentAllocation` (P1) already does this; billing calls it rather than reimplementing it.

### Payment marking stays manual for now

> "for time being, let me mark the invoice as 'paid' the same way odoo handle payment will do.
> I will mark the payment as paid manually"

No bank feed, no auto-reconciliation. The existing **Register Payment** dialog is the path, and
it already handles partial payment, FX settlement and allocation across several invoices. The
only automation is the advance-credit consumption above, which is allocation of money already
received — not a claim that money arrived.

### Attachments — deferred (unanswered, default stands)

Receipt upload and signed-contract storage stay out of v1, per §7 of the original plan.
`attachment_id` is carried nullable so the capability can be added without a schema change.

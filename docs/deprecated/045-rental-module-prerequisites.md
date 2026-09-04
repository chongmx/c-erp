# 045 — Prerequisites for Building the Rental Module Properly

**Date:** 2026-08-02
**Expands:** `040` §3.7 (which listed dependencies but under-specified two of them)
**Question answered:** what must exist, and be right, before the rental module is worth building

---

## 0. Summary

Seven prerequisites. Four are missing subsystems, two are architectural decisions that are
cheap now and expensive later, one is a hygiene fix that four separate security findings have
already argued for.

| # | Prerequisite | Type | Effort | Why it blocks |
|---|---|---|---|---|
| **P1** | Payment allocation (`account.partial.reconcile`) | Missing | 1 w | "Pays in advance / pays late" *is* allocation |
| **P2** | Money representation decision | Design | 2–3 d | `double` + ad-hoc rounding will drift over recurring billing |
| **P3** | Tax computation engine | Missing | 2 w | Every invoice total is currently untaxed |
| **P4** | `ir.sequence` | Missing | 3–4 d | Legal invoice numbering |
| **P5** | `ir.cron` | Missing | 3–4 d | Recurring invoicing has no scheduler |
| **P6** | ViewModel pattern fix (ARCH-1) | Hygiene | 3 d | Rental adds ~9 models; would be defect occurrence #5, on financial records |
| **P7** | Test harness | Missing | 1 w | Unattended money movement on a timer |

**Total ≈ 5 weeks.** P1–P3 are the ones that decide whether the ledger is trustworthy; the
rest are mechanics.

---

## 1. P1 — Payment allocation is the requirement, not a feature of it

The stated requirement — *"the user may sometimes pay in advance or sometimes delay
payment"* — is open-item accounting. What exists today cannot express it.

Current model (`AccountModule.cpp:972`): one payment settles one invoice by decrementing a
scalar.

```cpp
double newResidual = std::max(0.0, amountResidual - payAmount);
std::string newPayState = (newResidual < 0.001) ? "paid" : "partial";
```

Workable for "customer pays this invoice". It cannot represent:

- **One payment across several invoices** — the normal case here. A tenant renting three
  lockers pays one bank transfer covering three invoices with three different due dates.
- **An unallocated credit.** Pay two months up front and there is nowhere to put the money:
  no invoice exists yet to decrement. The scalar has no concept of "money held against future
  invoices".
- **Reversing an allocation** when a payment is misapplied.
- **Which payment settled which invoice** — needed for any statement of account.

Without this the dashboard cannot answer *"what does this customer owe?"*, which is the
number the business runs on.

**Build:** `account.partial.reconcile` — `(payment_id, move_id, amount, date)` — plus
`amount_residual` derived from allocations rather than mutated in place. Auto-allocate
oldest-open-first on payment, configurable.

**Build it in the `account` module, not `rental`.** It is general ERP functionality; putting
it in rental guarantees rebuilding it later.

---

## 2. P2 — Decide how money is represented, before writing a billing engine

**This was not in the earlier analysis and it is the one I would resolve first.**

Today: PostgreSQL stores `NUMERIC(16,2)` (exact), C++ holds `double` (binary floating point),
JSON transports `double`, the browser receives `double`.

The DB is right. Everything above it is approximate.

There is no central rounding discipline. `SaleModule.cpp:1304` rounds inline:

```cpp
vals["price_subtotal"] = std::round(subtotal * 100.0) / 100.0;
```

`AccountModule` does not. Payment settlement leans on an epsilon (`< 0.001`), which is a
symptom, not a policy.

### Why single invoices survive this and recurring billing does not

One invoice, one payment, one comparison — the epsilon absorbs it. A rental ledger compounds:

- 12+ invoices per contract per year, per unit
- first-period **proration** — `rate × days_in_period / days_in_month` is rarely exact
- tax at a percentage of a prorated figure
- **partial allocations** splitting one payment across several invoices, each split rounded

Concretely: a monthly rate of 300.00 prorated over 17/31 days is 164.516129…, taxed at 8%
gives 177.677419…. Round at different points and two systems disagree by a cent. Do it monthly
across dozens of units and the AR total no longer reconciles with the sum of its invoices —
the failure surfaces as "the dashboard says 12,847.03 but the invoices add to 12,847.01", which
is unfalsifiable without a defined rounding policy.

### The decision (pick one, then apply it everywhere)

1. **Integer minor units** — store and compute in cents (`int64_t`). Exact, no rounding
   questions, biggest diff. What accounting systems do.
2. **A `Money` type wrapping `int64_t` cents**, converting at the JSON and SQL boundaries.
   Same guarantees, contained blast radius. **My recommendation.**
3. **Keep `double`, add a single mandated rounding helper** applied at every arithmetic
   boundary, plus a documented rounding policy (half-up at 2 dp, round after tax per line).
   Cheapest, weakest, and relies on discipline forever.

Whichever you choose, decide **before** the billing engine exists. Retrofitting money
representation through a live ledger means reconciling historical rows.

---

## 3. P3 — Tax engine

`account.tax` exists as a model with a `tax_line_id` field; nothing computes tax lines. Every
invoice total today is untaxed.

For rental specifically, settle these before building:

- **Is storage rental taxable in your jurisdiction, and at what rate?** The origin serves from
  KUL, so Malaysian SST is the likely regime — service tax treatment of storage/warehousing
  needs confirming with your accountant, not assumed from code.
- **Tax-inclusive or tax-exclusive pricing?** Whether the advertised locker price includes tax
  changes the arithmetic and the contract wording. Decide first; it is not a display setting.
- **Rounding**: per line or per invoice. Interacts directly with P2.

---

## 4. P4 — `ir.sequence`

Numbering is currently hardcoded PostgreSQL sequences created inline in `ensureSchema_()`
(`sale_order_seq`, `stock_out_seq`). No prefix or padding config, no per-company numbering, no
yearly reset, no gap control.

Invoice numbering carries legal requirements in most jurisdictions — typically sequential and
gapless per company per year. A rental business issues invoices monthly, forever, so this is
the highest-volume numbering in the system.

Also gives contract numbers (`RENT/2026/0001`).

---

## 5. P5 — `ir.cron`

No scheduler exists. Recurring invoicing and recurring expenses both need one.

Cheaper than it looks: the mechanism already exists from the S-43 fix —

```cpp
drogon::app().getLoop()->runEvery(60.0, [sess]{ ... });   // Container.hpp
```

What is missing is a job table, a registry and failure handling. Generalising that timer is
most of the work.

**Fallback if you want to defer:** a manual "Generate invoices now" button plus an external
systemd timer hitting an authenticated endpoint. Workable, but you own the retry semantics —
and billing is the wrong place to hand-roll those.

---

## 6. P6 — Fix the ViewModel pattern before adding nine models

S-35 (record rules), S-37 (audit), S-38 (CSV rules) and S-47 (identity audit) were **four
instances of one defect**: behaviour wired into `GenericViewModel` is silently absent from
hand-written ViewModels. Each was retrofitted case by case, and each retrofit missed
ViewModels written earlier.

The rental module adds ~9 models, several needing custom ViewModels (contracts, lines,
dashboard). Without the pattern fix it becomes occurrence #5 — this time on contracts,
invoices and payments, where a missing audit row or an unenforced record rule is a financial
and compliance problem rather than a cosmetic one.

**Fix (~3 d):** move audit, rule-domain merge and OCC into `BaseViewModel` so they are
inherited by construction, plus a boot-time assertion that every ViewModel exposing
`create`/`write`/`unlink` either inherits the enforced path or sits on a named allowlist.

`ValidationError` (added in `042` §5) is already in place and rental will use it throughout.

---

## 7. P7 — Test harness

Still the last open P0, and the rental module is the strongest argument for it: a billing
engine moves money **unattended, on a schedule**. Its failure modes are double-billing and
silent non-billing, and both are discovered by customers rather than by staff.

S-49 — the login rate limiter that counted nothing — is precisely the bug class that passes
code review and dies instantly to a runtime test. A billing loop has more of those, not fewer.

Minimum before the billing engine ships:

- proration arithmetic across month lengths and leap years
- **idempotency**: running the billing job twice produces one invoice
- allocation: one payment across three invoices leaves correct residuals
- cancellation mid-period
- the money-rounding policy from P2, asserted

`scripts/test_sessionmanager.cpp` is already close to production shape and ports to the
`ERP_TEST` harness with a `main()` swap.

---

## 8. Design rules to fix now, not later

**Idempotency constraint** — the single most important line in the module:

```sql
UNIQUE (contract_line_id, period_start)   -- on rental.invoice.link
```

Makes double-billing impossible even if cron fires twice, the process restarts mid-run, or
someone triggers a manual run. Nearly every billing bug in systems like this traces back to
not having it.

**Timezone** — `account_move.date` defaults to `CURRENT_DATE` (server timezone) while the
session context hardcodes `tz: UTC`. For a UTC+8 business, "invoices due today" and
month-boundary billing are off by up to 8 hours. Decide the billing timezone explicitly and
store it; do not inherit whatever the VM happens to be set to.

**Deposits are liabilities, not revenue.** A security deposit must post to a liability account
and never auto-apply to rent. That is a decision, not a default.

---

## 9. What is *not* a prerequisite

Worth stating, to keep the critical path honest:

| Not needed | Why |
|---|---|
| `stock.quant` / `qty_available` | Lockers are not inventory. `rental.unit.state` is derived from active contract lines |
| Multi-company | One company launches fine; 964 MB RAM makes process-per-tenant need re-costing anyway |
| `product.template` / variants | No product variants in locker rental |
| `product.pricelist` | Unit-type rates plus a per-line `unit_price` override covers it |
| Bank reconciliation | Manual payment entry is fine at this scale; revisit at volume |
| Multi-currency / FX | Single jurisdiction |
| PartKeepr PK2–PK7 | Different product entirely |
| `mrp.production`, `product.supplierinfo` | Unrelated — strip their dead ACL entries |

**`ir.attachment` and `ir.mail_server` are borderline.** Neither blocks a correct billing
engine, but both become urgent with a real tenant: signed rental agreements need storing, and
monthly invoices need emailing. 3–5 days each. Pull them forward the moment manual handover
stops being tolerable — and note PDF generation is currently broken in production
(`wkhtmltopdf` not installed, `044` §3 P4), so even manual handover does not work today.

---

## 10. Recommended order

```
P2 money representation    2–3 d   decide first — it constrains everything below
P6 ViewModel pattern       3 d     before any new models exist
P4 ir.sequence             3–4 d   independent, unblocks numbering
P5 ir.cron                 3–4 d   independent, generalise the S-43 timer
P1 payment allocation      1 w     needs P2 settled
P3 tax engine              2 w     needs P2 settled; highest risk — prototype early
P7 test harness            1 w     in parallel, seeded from the above
                          ─────
                          ~5 weeks
```

P4 and P5 are independent of the money work and can run in parallel if there are two people.

**Prototype P3 (tax) first even though it is scheduled later** — it is the item most likely to
overrun, and it interacts with P2. A two-day spike answering "inclusive or exclusive, rounded
where, at what rate" de-risks the largest block on the critical path.

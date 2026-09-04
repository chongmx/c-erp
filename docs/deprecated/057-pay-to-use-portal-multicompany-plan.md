# 057 — Pay-to-use rental, customer invoice download, multi-company

**Date:** 2026-08-05
**Supersedes:** `054` §2 phase 4 (contract lifecycle) · updates `040` §2
**Status:** Plan

---

## 0. What changed

Three requirements:

1. **Pay to use, monthly. No contract.** The business does not sign contracts — a
   customer takes a locker and pays monthly until they stop.
2. **Customers download their own invoices.**
3. **Multiple companies**, each with its own database, admin switches between them
   (as specified in `040` §2.1).

(1) removes the entity phases 1–3 were built around, so it is rework — quantified in §1.4.
(2) is largely **already built** and was not known to be. (3) has got *more* expensive to do
the other way, which settles a decision that `040` left open for revisiting.

---

## 1. Pay-to-use: a workflow change, not a schema change

> **Revised.** The first version of this section proposed collapsing
> `rental.contract` + `rental.contract.line` into a single `rental.tenancy` table, at a
> cost of ~3 days. That was wrong, and the reasoning is recorded here because the
> mistake is instructive.

### 1.1 Why no migration is needed

Reading "no contract" as "delete the contract table" confuses the **name** with what the
table holds. It is not storing an agreement. It is storing **per-customer billing
settings**:

```
partner_id · currency_id · journal_id · payment_term_id
deposit_amount · deposit_state · billing_lead_days
```

Collapsing those onto the per-unit rows repeats every one of them per locker. A customer
with five units stores their journal five times, and those copies drift the first time
someone changes payment terms and updates four of the five. That is denormalisation
introduced deliberately, to delete a table whose only genuine problem is that it is
called `contract`.

The two-table shape is correct. It is a **customer rental account** with a row per unit —
and a rental account is a thing the business does have, even when contracts are not.

### 1.2 What the requirement actually demands

Checked against the live schema — nothing structural is missing:

| Requirement | Already supported |
|---|---|
| Monthly, billed in advance | `billing_lead_days` on the header, `next_period_start` per unit |
| No commitment, stop any time | `date_end` nullable, `state = 'ended'` |
| Different start dates → different due dates | per-line `next_period_start` |
| Deposit optional | defaults to 0, `deposit_state = 'none'` |

The one real friction is that `rental_contract_line.contract_id` is `NOT NULL`, so today a
contract must exist *before* anyone can be given a locker. **That** is the ceremony to
remove — and it is removed in the action, not in the schema.

### 1.3 What to build instead

One action:

```
rent_unit(partner, unit, date_start, rate)
    get-or-create the customer's rental account
    insert the tenancy row
    emit unit_assigned      (state follows from the trigger)
```

The operator picks a customer and a unit. The account row appears by itself and is never
something they create. Plus UI labels: "Rental Account", never "Contract", and no
create button on it.

**~0.5 day**, against 3 days for the migration. The difference goes into billing and the
portal.

### 1.4 If the name still grates

Renaming `rental_contract` → `rental_account` is mechanical, and there are now tests to
do it safely. It is an internal table name, not a user-facing word — the labels in §1.3
solve the actual problem. Worth doing only if the internal name causes real confusion
during phase 4, not pre-emptively.

---

## 2. Customer invoice download — mostly already built

### 2.1 What exists today

`modules/portal/PortalModule.cpp` already registers, all cookie-authenticated:

```
/portal/api/invoices              list, scoped by session->partnerId
/portal/api/invoice/{id}/detail   line detail
/portal/api/invoice/{id}/print    printable HTML
/portal/api/invoice/{id}/pdf      PDF download, Content-Disposition: attachment
/portal/api/invoice/{id}/proof    customer uploads proof of payment
```

The PDF route is **already partner-scoped** — `portalRenderDoc(…, session->partnerId, …)`
returns empty for someone else's invoice, which becomes a 404. It carries the
`PoolExhaustedException` → 503 handler and the SEC-28 `devMode` gate.

Rental invoices are ordinary `account.move` rows with `move_type='out_invoice'` and a
`partner_id`, so **they appear in the portal with no new code at all**.

### 2.2 What is actually left

| | Effort |
|---|---|
| Verify end-to-end against a real rental invoice, incl. a **negative control** — customer A must fail to fetch customer B's PDF | 0.5 d |
| Rental invoice layout: unit code, period covered, rate — the generic layout says "Product" | 1 d |
| "My units" portal page — code, type, since, monthly rate | 1 d |
| Portal login provisioning for tenants (`portal.partner` exists; needs a per-tenant flow) | 1 d |
| `ir.rule` record rules scoped by `partner_id` as defence in depth behind the explicit scoping | 0.5 d |

**~4 days**, not the week `046` §8 assumed, because the download itself is done.

> The negative control is not optional. The portal is the public surface, and an
> access-control check that has only ever been tested with the *right* customer proves
> nothing. This is the same discipline that turned two earlier false-pass verifications
> in this project into real ones.

---

## 3. Multi-company — one process per database

### 3.1 The decision is now clearer than when `040` was written

`040` §2.3 chose process-per-database over an in-process db-keyed registry, and left the
door open to revisit. Re-measured today, the alternative has become **more** expensive:

| | When `040` was written | Today |
|---|---|---|
| `registerCreator` call sites | ~40 | **109** |
| Process-wide singletons | 2 (RuleEngine, AuditService) | **5** — P2/P4/P5 added DecimalPrecision, IrSequence, IrCron |
| `using Creator = std::function<std::shared_ptr<TBase>()>` | takes no args | unchanged |

Every one of those singletons holds one `DbConnection` for the whole process, and
`DecimalPrecision` and `IrSequence` additionally hold **cached state that is per-company by
nature** — decimal places and sequence counters. Threading a database through them is not a
signature change; it is a correctness problem with a silent failure mode, where company A's
invoice numbering or decimal precision leaks into company B.

So: **one `c-erp` process per company**, each with its own config, own database, own port,
all on loopback, nginx routing by subdomain.

```
acme.easylockerspace.com   -> 127.0.0.1:8069  (db: erp_acme)
globex.easylockerspace.com -> 127.0.0.1:8070  (db: erp_globex)
```

Isolation is the *reason* for separate databases, and this delivers it structurally rather
than by getting 109 sites right.

### 3.2 Application changes: about five lines

`main.cpp:45` hard-codes the config path:

```cpp
auto cfg = odoo::infrastructure::AppConfig::fromFileOrEnv("config/system.cfg");
```

It takes `argv[1]` with the current path as the default. That is the **entire** application
change. Everything else is operations, which is the point.

### 3.3 The work

| | Effort |
|---|---|
| `main.cpp` accepts a config path argument | 1 h |
| `config/tenants.json` + per-tenant config template | 2 h |
| systemd template unit `c-erp@.service` | 3 h |
| nginx per-subdomain server blocks, incl. `real_ip` (required by S-40) | 3 h |
| `scripts/new_tenant.sh` — createdb, render config, enable unit, reload nginx | 4 h |
| Company switcher: admin menu listing companies, linking to each subdomain | 4 h |
| Per-tenant backup **and a restore drill** | 1 d |
| Measure RSS of one instance; set a tenant-count ceiling | 2 h |
| Verification: cross-tenant isolation negative controls | 4 h |

**~1 week.**

### 3.4 Two things not to do

**Do not scope the session cookie to the parent domain** to make switching seamless. Cookies
scoped per subdomain isolate sessions for free; widening them re-couples the tenants, and
combined with the fixation hardening in S-42 it would let a session minted on one tenant be
presented to another.

**Do not share one database with a `company_id` column.** Every query in 109 registration
sites would have to filter on it correctly, forever, and the failure mode is one customer
seeing another company's data. You already specified separate databases; this records why
that instinct is right.

### 3.5 What isolation must be proved, not assumed

A `verify_multitenant.sh` that fails if any of these do not hold:

- a session cookie from tenant A is rejected by tenant B
- tenant A's admin cannot read tenant B's partners, invoices or units by any route
- invoice sequences advance independently — `RENT/2026/0001` exists in both databases
- decimal precision set in A does not change what B reports in `fields_get`
- the portal on A cannot fetch a PDF for a partner that only exists in B

The last three are the ones that would have silently broken under the in-process design,
which is exactly why they are worth asserting under this one.

---

## 4. Sequence

```
1  rent_unit action + UI labels (no migration)            0.5 d
2  tenancy lifecycle (end / change rate)                  1 d
3  billing engine — group by (partner, period)            1.5 w
4  portal: verify download, negative control, layout      2 d
5  multi-company: config, systemd, nginx, provisioning    1 w
6  portal: my units, tenant login provisioning            2 d
7  expenses, dashboard                                    2 w
                                                          ─────
                                                          ~5.5 weeks
```

Billing (step 3) is the point at which the system replaces whatever does the job today.
Multi-company is deliberately **after** it: a second company is worth having once the first
one is being invoiced correctly, and the ops work does not block the accounting work.

---

## 5. Open questions

1. **Deposits** — you said none collected today, refund optional. Keeping the fields on
   `rental.tenancy` (per unit) rather than per customer. Say if it should be per customer.
2. **Same customer, two companies** — with separate databases they are two unrelated
   records with two portal logins. Assumed acceptable; cross-tenant identity is a much
   larger piece of work.
3. **Invoice numbering across companies** — each database has its own `ir.sequence`, so both
   will produce `RENT/2026/0001`. Fine if they are separate legal entities. If they must be
   globally unique, the prefix needs the company slug.

# 055 — Rental module: phases 0, 1 and 2

**Date:** 2026-08-04
**Implements:** `054` phases 0 (frontend groundwork), 1 (schema + models), 2 (event log)
**Status:** ✅ Complete and verified

---

## What was built

| | |
|---|---|
| `modules/rental/RentalMigrations.{hpp,cpp}` | migrations 800–810 |
| `modules/rental/RentalModule.{hpp,cpp}` | 7 models, ViewModels, views, menus |
| `modules/rental/RentalEvents.{hpp,cpp}` | domain event log + `emit()` |
| `scripts/verify_rental_schema.sh` | 35 checks |
| `scripts/verify_rental_audit.sh` | 9 checks |
| `web/static/src/app.js` | `CUSTOM_VIEWS` map replacing the `t-elif` ladder |

Seven models — `rental.unit.type`, `rental.unit`, `rental.contract`,
`rental.contract.line`, `rental.expense.category`, `rental.expense`, `rental.event` —
all on `GenericViewModel<T>`, so they inherit auditing, record rules and OCC rather
than hand-rolling them.

---

## The two constraints the module leans on

Both are proved by **attempting the violation and requiring it to fail**. A constraint
that is never exercised is a comment, not a guarantee.

### `UNIQUE (contract_line_id, period_start)`

```
duplicate attempt -> ERROR: duplicate key value violates unique constraint
                            "rental_invoice_link_uniq"
links for this line after the attempt: 1
```

`ir.cron` is at-least-once by design, so the *scheduler* is not what prevents
double-billing — this is. It holds if the cron fires twice, if the process restarts
mid-run, or if someone clicks "Generate invoices now" twice.

The test also asserts the constraint is not *too broad*: the following month must
still be billable on the same line, or normal monthly billing would be blocked.

### One live line per unit

```sql
CREATE UNIQUE INDEX rental_cl_unit_live_uniq ON rental_contract_line(unit_id)
    WHERE state IN ('pending','active') AND unit_id IS NOT NULL;
```

Deriving `rental_unit.state` from the lines keeps the UI honest; only a constraint
makes the race impossible when two operators let the same locker concurrently. And
again the converse is asserted — once a line ends, the unit must be re-lettable, or a
locker could never be re-let after its first tenant.

---

## Three bugs found while building this

### 1. `GenericViewModel` double-audited every operation — all models, not just rental

The one that matters. Every create, write and unlink through `GenericViewModel<T>`
wrote **two** `audit_log` rows:

```
rental.unit              create id=5 -> +2 audit row(s)   write -> +2
rental.expense.category  create id=8 -> +2 audit row(s)   write -> +2
```

`REGISTER_MUTATOR` audits inside `registerMutator_` — and the handlers then called
`AuditService::log()` again. This is the P6 leftover (`050`): registrations were
converted to `REGISTER_MUTATOR` but the manual `log()` calls inside the handlers were
not removed. It was fixed in the individual modules and **missed in the template that
backs most models in the system**.

**Why `verify_no_double_audit.sh` passed throughout.** It probed four models. Three
skipped because their creates failed without more setup, and the one that ran —
`res.users` — is one of the few with a *hand-written* ViewModel, the path that was
never broken. The suite reported "no double audit" while never once exercising the
generic path.

Two changes, because the fix alone would leave the hole open:

- the probe list now includes two `GenericViewModel` models chosen because they create
  with no required foreign keys, so they cannot silently skip
- **coverage is asserted**: fewer than four models actually probed is now a failure.
  A skip proves nothing, and this suite spent months proving nothing while printing
  PASS

### 2. The cron migration would have left billing switched off

Migration 990 (P5) had already seeded `rental.billing` and `rental.expenses` as
deliberately **inactive** placeholders, so the scheduler would not log "no handler"
every tick until this module landed. Migration 810 then:

- invented a third code, `rental.expense.recurring`, giving two jobs for one purpose
- used `ON CONFLICT (code) DO NOTHING` with `active TRUE`, which silently kept the
  existing inactive row

So had the handler existed, billing still would never have run, and nothing anywhere
would have reported a problem. `DO NOTHING` hides exactly the case worth knowing about.

810 now reconciles instead of inserting, and the jobs **stay inactive** — the handlers
arrive in phase 5 and phase 7, and each phase switches its own job on when it can
actually service it. `verify_rental_schema.sh` asserts both the count and that they
are off.

### 3. The audit test queried columns that do not exist

`audit_log` stores `record_ids` (an integer *array*) and `uid` — not `res_id` and
`user_id`. The wrong names returned empty, which read as "not audited" when the rows
were present. Fixed to `record_ids @> ARRAY[id]`.

The same script also asserted `>= 2` audit rows where it meant exactly 2 — which
would have passed against the very double-audit bug it was written to find.

---

## Decisions recorded (from the answers in `054` §7)

**Billing is in advance**, which deletes a whole class of work: no mid-period
proration, no cancellation refunds. It adds `billing_lead_days` (default 7) so the
invoice is raised *before* the period starts — without it, "bill in advance" quietly
becomes "bill on day one of the period".

**Deposits** are supported but never automatic: `deposit_amount` defaults to 0, the
amount is held as a liability, and on cancellation the operator is asked
Refund / Forfeit / Leave held rather than a default firing.

**Advance payments** are consumed by each generated invoice at that line's own billing
date, through `PaymentAllocation` rather than a second implementation.

**Payment marking stays manual** — the existing Register Payment dialog.

---

## A note on the migration numbering

800–810 are numerically **below** the already-applied 900–1010. That is fatal under a
high-water-mark scheme and fine here, because `MigrationRunner` skips by set
membership. Checked in the source before writing a line of SQL, and now asserted at
runtime:

```
PASS  applied below the existing high-water mark (1010) — set membership, not max
```

---

## Verification

```
verify_rental_schema   35 checks   constraints proved by violation, money columns
                                   BIGINT, seeds, sequence/cron, event log, API CRUD
verify_rental_audit     9 checks   create+write audited exactly once, user attributed
verify_no_double_audit 15 checks   now covers GenericViewModel, coverage asserted
full suite             all green
```

---

## Next: phase 3 — units and state derivation

Deriving `rental_unit.state` from the contract lines, the unit list, and the unit grid
(`046` §4) — colour plus glyph, never state by colour alone.

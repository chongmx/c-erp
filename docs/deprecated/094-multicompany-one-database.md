# 094 — One company identity, and several companies in one database

Two pieces of work, in that order, because the second is impossible without the
first.

Suite: **66 passed, 0 failed** (`verify_multicompany_isolation.sh`, 51 checks).

---

## 1. The company had three identities

`res_company` said **Easy Locker Space**. Its own partner row said **My
Company**. `ir_config_parameter` held a third copy, plus the only copy of the
letterhead address, registration number and bank details.

Each editor wrote one store and each consumer read a different one:

| | wrote / read |
|---|---|
| ERP Settings screen | `ir_config_parameter` only |
| Settings ▸ Companies | `res_company` only |
| the real invoice PDF | `res_company` + partner, overlaid with `report.*` |
| the template **preview** | `ir_config_parameter`, falling back to `"Demo Company Sdn. Bhd."` |
| login chooser, admin console | `res_company.name` |

The visible symptom: because `company.email` was blank, **the preview showed
`info@democompany.com` while the invoice it was previewing showed
`admin@easylockerspace.com`.** The preview was not showing you your document.

### Fix

Identity moved onto `res_company`, which gained `reg_number`, `street`,
`street2`, `street3`, `city_country`, the five `bank_*` columns and
`payment_term_days`. `AuthModule::migrateCompanyIdentity_` copies the old values
across, fixes the partner's name to match the company, and deletes the config
rows. It is idempotent twice over: a value is only copied into a column that is
still empty, so an edit made on the company form is never overwritten by a stale
config row.

All three renderers now call one loader, `core::CompanyIdentity::load`.

**Why `res_company` and not config:** `ir_config_parameter` has `UNIQUE(key)` and
no `company_id`. It is single-company *by construction* — it could never hold a
second company's bank account. That is the whole reason this had to happen first.

---

## 2. Several companies in one database

### What a user sees

A **company switcher** in the top bar, listing the companies you belong to.
Switching reloads the app scoped to that company. It shares the dropdown with
the existing cross-*database* switcher (docs/072) under separate headings,
because the two do genuinely different things and you should be able to tell
which one you are about to do.

**Settings ▸ Companies & Access** gained a user × company matrix. Tick a box to
let someone work in a company. A user's last company cannot be revoked.

### The scoping rule

> A record belongs to the company that created it. You see the records of the
> company you are currently switched into, plus records whose `company_id` is
> NULL, which means shared.

### Where it is enforced, and why not in `ir.rule`

In `BaseModel`, on **search, search_read, search_count, read, write and unlink**.
Two deliberate departures from the obvious design:

**It is not an `ir.rule`.** `RuleEngine::buildRuleDomain` returns immediately for
`ctx.isAdmin`. A company rule written as a record rule would therefore not apply
to the administrator — the account nearly everyone actually uses — and company
scoping the main user bypasses is not scoping. It is also automatic: every table
with a `company_id` column is covered, so a new module cannot forget to opt in.

**It is not only on reads.** `write` and `unlink` address rows by id, not by
domain. Filtering only `SELECT` would still let anyone who guesses an id modify
or delete another company's row. Each of the six paths is probed with a
known-good id from the other company in the tests.

### Hand-written report SQL

The dashboard and the financial statements are raw aggregate queries. They never
touch `BaseModel`, so they got none of its scoping for free — and a probe
confirmed it: posting an invoice in a second company **changed the first
company's dashboard**. A ledger figure crossing companies is the most
consequential leak of the set.

Rather than appending a predicate to twenty different `WHERE` clauses, where one
missed branch is a silent cross-company total, `financialReport_` replaces its
two source tables with company-scoped subqueries:

```cpp
const std::string AML = "(SELECT * FROM account_move_line WHERE company_id=N)";
```

Every branch is then scoped identically and a branch added later inherits it.
The dashboard's cards take an explicit predicate each, including
`account_budget_line`, which has no `company_id` of its own and is scoped
through the budget it belongs to.

### Four bugs found by testing rather than by reading

**The scoping never engaged.** It keyed on `fieldRegistry_.has("company_id")` —
but most models with a `company_id` *column* never *declare* it as a field, and
`res.partner` is one of them. The registry is what a model chose to say about
itself; the table is the truth. Now one catalogue query answers it for every
model at once.

**The context never arrived.** `PartnerViewModel` is hand-written and never calls
`setUserContext`, so `res.partner` reached `BaseModel` with an anonymous context
and scoping switched itself off. This is the fifth time this codebase has been
bitten by the same shape of defect — S-35 record rules, S-37 audit, S-38 CSV
rules, S-47 privilege audit — every one of them a cross-cutting concern wired
into `GenericViewModel` and quietly missing from the bespoke ViewModels.

So it no longer depends on a ViewModel remembering. `core::CurrentUser` publishes
the session for the duration of the request and `BaseModel` falls back to it,
mirroring the per-request thread-local `TenantScope` already relies on.

**A new record would have been born shared.** The stamp went into the values
dict, but `create()` builds its INSERT from the field registry — which, again,
usually has no `company_id` — so the stamp was dropped and the row landed NULL,
meaning *visible to every company*. `create()` now appends the column itself when
the registry did not carry it.

### Existing rows

Everything written before this feature carried `company_id NULL`, so the moment
a second company was created it would have seen all of the first company's
customers, products and locations. Correct by the letter of the rule, a leak by
any reasonable reading.

`core::backfillCompanyIds` attributes those rows **while exactly one company
exists** — the window in which NULL and company-1 are indistinguishable and
nothing can observe the change. A no-op today; the difference between a clean
second company and a pre-populated one tomorrow. Once a second company exists,
the guard stops firing and existing data is never guessed at again.

`ir_sequence` is excluded: NULL there means a *global* sequence, enforced by a
partial unique index, and attributing those would change how document numbers
are allocated.

---

## 3. What the tests actually assert

`verify_multicompany_isolation.sh` builds a second company and a non-admin user
inside it, then:

- neither side's `search_read` contains the other's record — **and neither does
  the admin's**, in both directions;
- `read`, `search_count`, `write` and `unlink` against a *known* id from the
  other company return nothing and change nothing (verified in the database, not
  from the response);
- `create` with a foreign `company_id` is refused and writes nothing;
- the switcher refuses a company you are not in, and does not even list it;
- after a grant, switching moves what you see *and* what new records are stamped
  with; switching back restores the first view;
- a shared (`NULL`) record is visible from both;
- the letterhead on a document follows the active company, with no trace of the
  other's;
- identity has exactly one home, and no company disagrees with its own partner.

---

## 4. Files

| File | |
|---|---|
| `core/CompanyIdentity.{hpp,cpp}` | the one identity loader; `backfillCompanyIds` |
| `core/UserContext.hpp` | `allowedCompanyIds`, `mayUseCompany`, `CurrentUser` |
| `modules/base/BaseModel.hpp` | the company clause on all six paths; `stampCompany_` |
| `modules/base/BaseViewModel.hpp` | reads `allowed_company_ids` from context |
| `core/infrastructure/SessionManager.hpp` | allowed companies on the session |
| `core/infrastructure/JsonRpcDispatcher.hpp` | `/web/session/my_companies`, `/web/session/set_active_company`, `/web/company/access`; publishes `CurrentUser` |
| `core/Container.hpp` | stage 2d — backfill after migrations |
| `modules/auth/AuthModule.cpp` | identity columns, `res_company_users_rel`, the migration |
| `modules/report/ReportModule.cpp` | both renderers use the shared loader; dead seeder neutered |
| `modules/portal/PortalModule.cpp` | portal documents use it too |
| `web/static/src/components/UserMenu.js` | the switcher |
| `web/static/src/components/CompanyAdmin.js` | the access matrix |

---

## 5. Deliberately not done

- **Several companies visible at once.** the reference ERP lets you tick multiple companies
  and see them together. Here visibility is always the single active company,
  which is strictly tighter and makes "nothing leaks" a property you can state
  in one sentence and test in one assertion. Multi-select is a refinement, not a
  gap.
- **Per-company sequences, journals or chart of accounts.** The columns exist and
  are scoped; the *seeding* of a second company's accounting is separate work.
  A new company today starts empty and needs its journals set up.
- **Company scoping in the remaining raw-SQL screens.** The accounting dashboard
  and the four financial statements are scoped and asserted. Other hand-written
  reporting queries — the rental dashboard, the SST return, stock valuation —
  were not audited for this and may still read across the database. They are
  admin-only screens and the underlying models are scoped, but the aggregates
  are worth closing next, the same way.

### A note on how the report assertions are written

The first version grepped each report for the amount `777.00` and reported a
leak that was not one: an unrelated entry named `STJ/777` and a running balance
ending in `777.00` both matched. The tests now capture each report *before* the
second company posts and compare byte-for-byte after. That cannot be fooled by
formatting and needs no guess about how a figure is rendered — and it is the
reason a leak that *was* real (the dashboard) and one that was not (the general
ledger) could be told apart.

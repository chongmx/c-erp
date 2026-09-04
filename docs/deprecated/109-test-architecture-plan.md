# 109 — Test architecture: lifecycle groups, and the move to `tests/`

Status: **built.** All 77 scripts now live in `tests/` as folders with a `meta`
each, `tests/run.sh` is the single entry point, and the unit tests are split by
subject. §6 has the current numbers and what is still outstanding; §7 records
the defect the migration itself uncovered.

---

## 1. Built now: create → use → delete

`run_tests.sh` runs three ordered groups instead of one alphabetical pile:

```
— group 1/3: create fixtures —   verify_00_fixture_create   (21 checks)
— group 2/3: use —               every other script
— group 3/3: delete fixtures —   verify_zz_fixture_delete   (17 checks)
```

**Why one canonical set rather than seven self-seeding scripts.** Seven scripts
failed on a clean database because each looked up "the first product" or "the
first sale order" and assumed one existed. Copying a seeding block into seven
files would fix the symptom and leave seven copies to drift. One set, created
once and removed once, fixes it and makes the **lifecycle itself testable**.

**Deletion is the half nobody writes, and it is where a data model tells the
truth.** The delete phase asserts more than "the rows are gone":

- nothing the fixtures owned survives,
- **no orphans** — no child left pointing at a deleted parent, plus a general
  sweep for any dangling `sale_order_line` anywhere,
- deleting twice is not an error,
- and the rest of the database is **untouched** — demo data, other partners,
  categories and part units are counted before and after. Teardown that quietly
  takes reference data with it is far worse than leaving fixtures behind.

The create phase mirrors it: every piece exists, every row is really in the
database, the **shapes the suite actually depends on** resolve (`first product`,
`first sale order line`, an invoice with a positive total, a draft
`out_invoice`), the line links the order and the product, and running it twice
leaves exactly one of each.

> A bug this immediately caught: the fixture used `product_uom` where the column
> is `product_uom_id`. The insert failed, the helper swallowed stderr, and the id
> came back empty — surfacing two assertions later as "sale line row missing".
> The fix is one word; finding it is what the phase is for.

## 2. The target layout

Flat enough to scan, grouped by what a test *is* rather than by which module it
happens to touch. **Two levels, three at most** — logical separation, not a tree.

```
tests/
  run.sh                  single entry point; replaces run_tests.sh
  README.md               how to run one, how to add one
  lib/                    the shared harness, sourced by every test
    assert.sh             ok / no / verdict, and the "no verdict = failure" rule
    api.sh                authenticate, call_kw, HTTP helpers
    db.sh                 psql helpers, snapshot take/restore
    fixtures.sh           fx_create / fx_drop  (moved from scripts/fixtures/)
  unit/                   C++ only. No database, no server, milliseconds.
    money/  tax/  domain/  csv/  qr/  si-units/
  functional/             daily usage, end to end, one folder per journey
    01-sell/  02-buy/  03-make/  04-parts/  05-project/  06-close/
  integration/            per-area technical checks (today's verify_* live here)
    account/  stock/  product/  mrp/  project/  rental/  core/
  security/               penetration tests (§5)
    auth/  access/  injection/  disclosure/  upload/
  scenarios/              named database states a test can demand
    baseline.dump  large-catalogue.dump  year-of-entries.dump
```

**A test is a folder, not a file.** That is what lets it carry whatever it needs:

```
tests/functional/01-sell/
  meta                    declares scenario, group, order, dependencies
  test.sh                 the steps
  helper.cpp              optional — compiled and run if present
  expected/invoice.txt    optional — golden output to diff against
  seed.sql                optional — scenario data beyond the shared fixtures
```

`meta` is what makes ordering and database scenarios explicit rather than
implied by filename:

```
group=functional
order=10
scenario=baseline          # or: current | large-catalogue | <file>.dump
needs=fixtures:core        # shared fixtures, or none
timeout=300
```

`run.sh` reads every `meta`, sorts by `(group, order)`, and restores the named
scenario **only when it changes** — so a run of twenty tests that all want
`baseline` restores once, not twenty times.

## 3. Functional tests — the daily journeys

These are the ones that matter for "is it shippable". Each is one continuous
story that touches many modules, asserting the *state after each step* rather
than one call in isolation.

All six are built, and each covers **both** the happy path and the cancel
path — 213 checks between them.

| Journey | Happy path | Cancel path | |
|---|---|---|---|
| **01-sell** | quotation → confirm → deliver → invoice → post → pay | second order invoiced, then cancelled: reverse to a credit note (RINV), the pair nets to zero, the order refuses to be invoiced again | 40 |
| **02-buy** | PO → confirm → receive → vendor bill → post → pay | bill reversed to a vendor refund (`in_refund`), the **payable** nets back to zero, the PO refuses to be billed again | 40 |
| **03-make** | BOM → MO → confirm → explode → produce | a reserved MO is cancelled: no components consumed, nothing left reserved, no surviving moves | 28 |
| **04-parts** | CSV BOM → resolve against the real catalogue → refuse the unknown row → a human settles it → commit → still findable → labels print | commit is refused while any row is unresolved, and nothing reaches the BOM | 27 |
| **05-project** | project → tasks → board → timesheets → planned vs logged | a task with time on it is deleted: no orphaned timesheet rows, the total moves by exactly that task's hours or not at all; the project archives rather than deletes | 28 |
| **06-close** | post a known entry → run all seven reports → they agree with the ledger | reverse the entry: the receivable returns to its **exact** pre-entry figure, the original stays posted, the trial balance takes up the reversal exactly | 50 |

**Why the cancel paths matter more than the happy ones.** The happy path is
what gets demonstrated; the cancel path is what happens on a Tuesday. It is
also where the accounting rules bite: a posted invoice cannot be deleted, so
the unwind is a *reversal*, and what must be true afterwards is not "the
invoice is gone" but "the two together net to nothing" — a stronger claim, and
a testable one.

Between them these found two more defects (§9).

Each ends by asserting the **books balance** and **stock ties out** — the two
invariants an ERP cannot violate, and the ones a per-module test never checks
because they span modules.

## 4. Unit tests and coverage

`unit/` stays C++ `ERP_TEST` cases and keeps its rule: **no database, ever**.
Subfolders by subject (`money/`, `tax/`, `domain/`, `si-units/`, `csv/`, `qr/`)
so a failure names its area.

**Coverage, honestly.** Line coverage needs a build with
`-fprofile-arcs -ftest-coverage` and `lcov`; that is a CMake option
(`-DENABLE_COVERAGE=ON`) and a report target, not something the current build
produces. Two numbers are worth having and only one is line coverage:

1. **Line/branch coverage** of `core/` and `modules/` from the unit + functional
   runs, via gcov/lcov.
2. **Surface coverage** — which registered RPC methods and HTTP routes were
   exercised at least once. For an ERP this is the more actionable of the two:
   the registries already enumerate ~100 view models and their methods, so the
   denominator is known exactly and the gaps are a list of names.

"Full coverage" should mean **100% of surface** and a stated, enforced floor on
lines — not a single number that can be met by exercising getters.

## 5. Penetration tests

A separate group, because they assert the opposite of everything else: that
things **fail**. Grounded in the controls this codebase actually has, so each
test names the rule it is trying to break.

| Area | What is attacked |
|---|---|
| **auth/** | no session, expired session, another user's session id, session fixation, cookie flags (`Secure`, `HttpOnly`, `SameSite`), logout really invalidates |
| **access/** | a non-admin on admin endpoints (`/web/dbtool`, backups, reset); **cross-company read, write AND delete** — docs/094 enforces all three, so all three are attacked; portal user reaching non-portal records |
| **injection/** | SQL through every place a caller names a column — domain fields, `ORDER BY`, facet keys (`param:…`), sort keys, import mappings. S-49 says these are allowlisted; the test asserts an unregistered name is *refused*, not merely escaped |
| **disclosure/** | SEC-28 — force errors on every route and assert no SQL text, table name or stack trace escapes outside devMode; the masked message must still be useful |
| **upload/** | allowlist bypass (`.php.pdf`, null bytes, unicode look-alikes), path traversal in filenames, oversize, zip bombs, content/extension mismatch |
| **console/** | the SQL console's read-only transaction — `UPDATE`, `DELETE`, DDL, `nextval()`, and a data-modifying CTE must all be refused by the *database*, not by keyword filtering |
| **limits/** | statement timeout holds, pool exhaustion returns 503 rather than hanging, a huge payload is rejected |

Two rules for this group:

- **A pen test that passes proves a control works; it must fail loudly if the
  control is removed.** Each one names the control in its message, so a future
  "why is this failing" has an answer in the output.
- They run against a **disposable scenario**, never the working database, since
  several deliberately try to write where they should not be able to.

## 6. What is done, and what is not

**Done:**

- **all 77 scripts migrated** from `scripts/verify_*.sh` into `tests/` as
  folders — `tests/<group>/<area>/<name>/{test.sh,meta}` — with git history
  preserved where the file was tracked;
- **`tests/run.sh`**: discovery by `meta`, group ordering, per-test scenarios,
  fixture provisioning, timeouts, verdict scoring, snapshot/restore, `--list`,
  `--only`, `--group`, `--unit`;
- **`tests/lib/`** — `assert.sh` (`ok`/`no`/`t_eq`/`t_lacks`/`verdict`),
  `api.sh` (`auth`, `call`, `call_k`, `call_as`, `http_get`), `db.sh`
  (`pg`/`pgid`, `scenario_load`/`scenario_save`), `fixtures.sh` (moved from
  `scripts/fixtures/`), plus the vendored `testlib/`;
- **unit tests split by subject** — `tests/unit/{money,tax,session}/`, with the
  CMake glob scoped to `tests/unit/*/` so a functional test's `helper.cpp`
  cannot be swept into the binary;
- **the security group started**: `auth/unauthenticated` (no session, forged
  session, post-logout session, user-enumeration, cookie flags),
  `disclosure/error-masking` (SEC-28 across six error paths),
  `injection/sql-surfaces` (S-49 across domain, ORDER BY, GROUP BY and facet
  keys), alongside the four pre-existing security scripts now grouped there;
- **five of the six functional journeys** — `01-sell`, `02-buy`, `03-make`,
  `05-project`, `06-close` (124 checks between them), each ending on a
  cross-module invariant: the books balance, stock ties out, conservation
  holds through manufacturing, the project total equals the sum of its tasks,
  the reports agree with the ledger;
- **`integration/core/referential-integrity`** — asserts the state a run starts
  *from* is consistent. See §7.
- `scripts/run_tests.sh` kept as a forwarder, and `audit_test_leaks.sh`
  repointed at `tests/**/test.sh`.

**The preamble.** Every migrated test opens by walking up for `CMakeLists.txt`
rather than counting `../`, so it behaves the same whether the runner invoked
it or you ran it directly from any directory, and it can be nested a folder
deeper without an edit. That is what let 77 files move with a mechanical patch:
only 12 referenced `$0` at all, and each of those became an `$ERP_ROOT` path.

**A lesson the journeys taught about the tests themselves.** `05-project`
passed three checks against **empty strings**: the timesheet column is
`unit_amount`, not `hours`, every `SUM` errored, `pg()` swallowed the error,
and `t_ne "0" ""` is true — an empty value is not equal to zero either. An
assertion that cannot fail on missing data is not an assertion. Prefer `t_ge`,
`t_eq` against an expected value, or an explicit emptiness check; treat an
unexpectedly empty query result as a failed query until proven otherwise.

**Not done:**

- `04-parts`, the sixth journey (see §3),
- the remaining security areas — `upload/` (allowlist bypass, traversal,
  content/extension mismatch), `console/` (the SQL console's read-only
  transaction), `limits/` (statement timeout, pool exhaustion → 503),
- the coverage build option (`-DENABLE_COVERAGE=ON`) and the surface-coverage
  report,
- the 11 leaking scripts from docs/103, which the restore hides but does not
  fix.

## 7. What the migration found: an inconsistent baseline

Re-ordering the suite turned two green tests red — `sale/pricelists` and
`product/product-variants` — and the cause was neither the reordering nor the
tests. **`baseline.dump` was internally inconsistent**: it held 173 products
whose `product_tmpl_id` pointed at `product_template` rows that no longer
existed, and `product_template_id_seq` had restarted at 1.

So the first template any test created took a **low id that a crowd of orphan
demo products already referenced**, and silently adopted them. `pricelists`
then read its "own" variant and got a demo resistor — category rule missing
(90.00 instead of 80.00) and `standard_price` 0.10 instead of 40.00, which is
where `formula gave 5.1` came from. `product-variants` saw its row counts drift
for the same reason.

Two things made this survivable for so long, and both are now closed:

- **`product_product.product_tmpl_id` has no foreign key.** Nothing in the
  database prevented the dangling rows, and nothing noticed them.
- **Nothing asserted the state a run starts from.** Every test checked what it
  did; none checked the ground it stood on. `integration/core/referential-
  integrity` now does, early enough (order=5) to read as "the ground is
  unsound" rather than as an area failure — including the sequence-behind-max-id
  check, which is the other half of the same bug.

The baseline was repaired by synthesising the missing templates from the
products that reference them and pushing the sequence past every id in use; the
previous dump is kept at `db/snapshots/baseline.pre-template-repair.dump`.

**Still open:** adding the real foreign key. It cannot simply be added — any
existing database carrying orphans would fail the migration, so it needs a
migration that repairs before it constrains.

## 8. What the first journey found: deliveries from sale orders could not be validated

`functional/01-sell` failed on its **first run**, at the shipping step, with a
masked internal error. The server log had the real one:

```
ERROR:  invalid input syntax for type bigint: "4e+06"
```

`StockModule::handleButtonValidate` read the delivered quantity as a `double`
and bound it to `qty_delivered`, a BIGINT micro-unit column. 4 units is
4 000 000 micro-units, which the shortest round-trip form of a double writes as
`4e+06` — and PostgreSQL rejects that for a bigint. So **validating any
delivery raised by a sale order failed for every quantity of one unit or more**,
and the identical defect sat in the purchase path, where it broke validating a
receipt against a purchase order.

This is the same class as the invoice-total bug already documented in
SaleModule's `action_create_invoices` ("appending a double like 2e8 serialises
to 2e+08"). Both are fixed by keeping the value integral: `SUM(quantity)::bigint`
read as `long long`.

**And then `02-buy` found two more of them.** Creating a vendor bill from a
purchase order failed twice over: once on the header amounts (`7.5e+08` for a
750.00 order) and again on the line quantity (`6e+06` for 6 units), both in
`PurchaseModule::handleActionCreateBills`. So **the entire purchase-to-bill
path was broken** for any realistic order — you could raise a PO, confirm it
and receive the goods, and then never bill it.

That is four instances of one mistake across three modules, each invisible
until a journey walked the whole path. The rule, now written where each fix
lives: **if a value is going into a micro-unit column, keep it integral the
whole way — never let it become a double in between.** A quick audit for
`as<double>()` feeding a `params.append()` is worth doing in any module that
writes money or quantities.

**Why 78 passing tests never saw it.** Every step was covered. `stock-quant`
validates pickings; `order-totals` confirms orders; `financial-reports` posts
invoices. But the broken line only executes when a picking has a `sale_id` —
that is, when the delivery came from a sale order — and no test had ever
validated one. The gap was not in any module's coverage; it was in the seam
between two of them, which is precisely what a journey is for and precisely
what per-module tests structurally cannot reach.

Two smaller findings from the same run, both fixed:

- **The fixtures left rows with `company_id NULL`**, which means "shared with
  every company". `multicompany-isolation` asserts globally that no such row
  exists, and only passed because some earlier test happened to restart the
  server and trigger the startup backfill. Order-dependent, and the fixtures'
  fault, not the test's.
- **An unrecognised catalogue facet key is ignored rather than refused**, so a
  caller who filters on something the server does not know gets the unfiltered
  list back. Not a security hole — the value is bound, not interpolated — but
  a filter that silently does nothing is worth tightening. Recorded as a NOTE
  in `security/injection/sql-surfaces`.

## 9. The restore was not restoring — and three more defects

Closing the last two red tests (`part-catalog`, `part-lookup`) turned up the
most consequential bug of the whole exercise, and it was in the test machinery
itself.

### 9.1 `pg_restore --clean` silently skipped a table

`db_snapshot.sh restore` used `pg_restore --clean --if-exists`, which drops
objects **one at a time** and treats a failed DROP as a warning.
`product_template` has dependents, so the drop failed, the CREATE then failed
with "already exists", and **its COPY was skipped**:

```
error: cannot drop table public.product_template because other objects depend on it
error: relation "product_template" already exists
```

Every other table was replaced; that one kept its **old rows**. The script saw
"errors but the schema is present" and carried on. The symptom was 163 demo
products pointing at template ids the restore had refused to load, surfacing as
unrelated failures in `pricelists`, `product-variants` and `part-lookup` — and
as a baseline that appeared to be missing data it actually contained.

The fix stops relying on ordered drops: `DROP SCHEMA public CASCADE; CREATE
SCHEMA public;` before restoring, so nothing is left to depend on anything.
The restore now also checks a few dependent-heavy tables came back and warns
if not.

**A restore that silently does not restore is worse than one that fails.** It
hid for so long because every check asked "is the schema present", which was
true throughout.

### 9.2 The baseline was missing the demo parameters

The catalogue held 163 products and **zero parameters** — the master reset that
derives the baseline clears `part_parameter`, exactly as it cleared
`product_template`. `part-catalog` saw 4 facets where it expects 5+, and
neither of its two real assertions could pass.

Re-running `seed_demo_parts.sh` rebuilt it (935 parameters, 163 MPNs, 47
distinct k-notations) — but only after fixing **`product_product_id_seq`, which
sat at 30 against a table whose max id was 2146**, so the seeder's first insert
collided on the primary key. Same sequence-behind-max defect as
`product_template`, different table.

> The audit that "proved" no sequences were behind was itself broken: it joined
> `pg_depend` on `deptype='a'`, matched **nothing at all**, and reported zero
> rows. `pg_get_serial_sequence` is the reliable way to find a column's
> sequence. A check that cannot fail is not a check — the same lesson the
> journeys taught about `t_ne "0" ""`.

One more ordering rule came out of it: **seed, restart, verify, then dump.**
The module's startup normalisation backfills `value_base` and missing
templates, so a dump taken straight after seeding captures a half-built
database.

### 9.3 `product.product create` produced a product with no template

Creating a variant through the API left `product_tmpl_id` NULL — invisible to
the variant screens, unpriceable by any template-keyed pricelist rule, and
counted as broken by every global integrity check.
`ProductProductViewModel` now synthesises a template from the product's own
values on create.

The enabling condition is worth fixing separately: the migration reads
`ALTER TABLE product_product ADD COLUMN IF NOT EXISTS product_tmpl_id INTEGER
REFERENCES product_template(id)`. On a database where the column already
existed the ALTER does nothing — **so the foreign key was never added**, and
dangling values were possible all along.

### 9.4 `action_cancel` reported success without cancelling

Both `sale.order` and `purchase.order` cancelled with
`UPDATE ... WHERE state = 'draft'`. A **confirmed** order — the only kind
anyone actually cancels — was left untouched, while the call returned `true`
and the chatter gained the line "Sales order cancelled." on an order that had
not been.

Found by the new cancel paths on their first run. Both now cancel from `draft`
or confirmed, refuse anything else with a `ValidationError` naming the state,
and write the chatter note only for orders that actually moved.

**Still open:** cancelling does not check for an outstanding posted invoice.
The correct rule is that an order with unreversed posted invoices should refuse
to cancel, and it needs the reconciliation semantics settled before it can be
written — guessing at it would block legitimate cancels.

## 9. The restore was not restoring — and three more defects

Closing the last two red tests (, ) turned up the
most consequential bug in the whole exercise, and it was in the test machinery
itself.

### 9.1  silently skipped a table

 used , which drops
objects **one at a time** and treats a failed DROP as a warning. 
has dependents, so:



Every other table was replaced; that one kept its **old rows**. The script saw
errors

# 104 — The baseline snapshot workflow

Status: **in place and working**. The workflow is live in `run_tests.sh` and
documented in `CLAUDE.md`. It immediately exposed seven non-hermetic scripts,
which are **not yet fixed** — see §4.

---

## 1. What "a clean database" now means

`db/snapshots/baseline.dump` — 724K, and the single definition of clean.

| It contains | It does not contain |
|---|---|
| 112 tables (full schema) | 0 journal entries |
| 137 menus, 16 accounts | 0 products |
| 76 help articles, 45 part units | no orders, no stock moves |
| 1 company, 1 admin user | no demo or QA data |

Both builders refuse to write a baseline that is not simultaneously **clean**
(no transactions, no products) and **complete** (schema present, admin user,
menus, chart of accounts). A snapshot that fails either test is worse than none,
because it fails later and somewhere else.

## 2. Two builders, because of one permission

**`make_baseline.sh`** is the real one: it creates a scratch database on a spare
port, lets the application provision it from empty, dumps that, and drops the
scratch. The result cannot inherit anything, because there was nothing there.

That needs `CREATEDB`, which the `odoo` role does not have here and `sudo` needs
a password. So it detects this and hands over to:

**`derive_baseline.sh`** — snapshots the working database, clears it with the
same reset the Database Tools screen uses at its widest scope, removes demo and
QA remnants, dumps the result, and **restores the working database**. Nothing is
lost; the safety snapshot is taken before anything is touched and restored on
exit even if the script fails.

To upgrade to the stronger baseline, once, as a superuser:

```sql
ALTER ROLE odoo CREATEDB;
```

then re-run `make_baseline.sh`. It will take the provisioned path automatically.

> The derived baseline inherits **configuration** from the working database. If
> a journal was added or a menu renamed there, it is in the baseline. That is
> the one thing the provisioned path would fix.

## 3. What `run_tests.sh` does now

1. snapshot the working database → `log/pretest.dump`
2. **load the baseline** so the run starts from identical rows
3. run the suite
4. **restore `log/pretest.dump`**

Step 2 only ever happens if step 1 succeeded — loading the baseline is
destructive, and doing it with no way back would trade reproducibility for the
user's data.

Flags: `--no-baseline`, `--keep-db`, `--baseline <file>`.

Verified end to end: the suite ran against the baseline and the working database
came back with `account_move` unchanged, demo parts and help articles intact.

## 4. The baseline immediately found seven non-hermetic scripts

Against the working database: **75 passed, 0 failed**.
Against the clean baseline: **68 passed, 7 failed**.

    verify_money_recompute    verify_money_roundtrip
    verify_new_views_smoke    verify_no_double_audit
    verify_product_variants   verify_read_group
    verify_tax_engine

They fail with variations of `could not create line: Invalid JSON` — a shell
variable that was empty because the product or order it expected to find does
not exist in a clean database.

**These are real defects, not baseline problems.** A test that only passes
because an unrelated script left a product behind is not testing what it says it
is, and its result is a coincidence. They were invisible until the data stopped
being shared.

Until they seed their own fixtures, `./scripts/run_tests.sh --no-baseline` is
the green run and `--baseline` is the honest one. Fixing them is the next piece
of work, and each fix is small: create the product or order the script needs
instead of assuming it.

## 5. Synthesised snapshots for corner cases

`db_snapshot.sh take <file>` captures any state, and a test can restore it at the
start. That is the supported way to test against a large, awkward or historical
database — a year of journal entries, a migration from an older schema, a
deliberately corrupted row — without carrying it in the working database.

Keep such snapshots under `db/snapshots/` with a name that says what is odd
about them.

## 6. Not done

- The seven scripts above.
- The baseline is not committed. It is 724K of binary; regenerate it with
  `make_baseline.sh` or commit it if the team wants a shared, pinned one.
- `make_baseline.sh`'s provisioned path is written but has not run to completion
  here, because the role lacks `CREATEDB`. It is exercised only as far as the
  permission check.

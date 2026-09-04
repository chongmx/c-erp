# 103 — Auditing the verification flows, and restoring the database

Status: **done**. `./scripts/run_tests.sh` → 75 passed, 0 failed, and the
database is byte-for-byte back where it started.

---

## 1. The audit measures; it does not read

Reading cleanup blocks is exactly how the invoice leak survived eleven days:
every script *had* a cleanup and one of them silently failed.

`scripts/audit_test_leaks.sh` runs each verification script with an exact row
count of every public table before and after, and reports the delta.

```
./scripts/audit_test_leaks.sh              # all scripts
./scripts/audit_test_leaks.sh bank_recon   # matching subset
```

Append-only bookkeeping (`audit_log`, `mail_message`, …) is excluded — an audit
trail that did *not* grow after a test would be the real bug.

### Result: 11 of 74 scripts leak, 21 tables

| Script | Leaves behind |
|---|---|
| verify_assets | account_move, account_move_line |
| verify_credit_note | account_move |
| verify_ir_primitives | account_move ×2 |
| verify_ir_sequence | account_move ×5 |
| verify_lots_packages | stock_picking, account_move |
| verify_product_inventory | account_move_line ×19 |
| verify_rental_demo | rental_event, account_move |
| verify_reorder | account_move ×3 |
| verify_sst_tax_report | account_move ×8, account_move_line ×16 |
| verify_stock_valuation | account_move, account_move_line |
| verify_supplierinfo | sale_order, account_move |

Almost every leak is `account_move` / `account_move_line` — which is precisely
the 43 orphaned invoices that eventually broke `verify_bank_recon`, now
confirmed by measurement rather than inferred.

## 2. Two bugs in my own audit, both already documented in `run_tests.sh`

The audit hung twice before producing anything, and both times the cause was
written down in a comment I had not applied:

1. **Command substitution.** `OUT=$(bash "$f")` waits for *every* writer to
   close the pipe. A script that restarts the server hands that pipe to a
   process designed never to exit, so the audit hung forever and no `timeout`
   could save it. Output goes to a **file** now.
2. **No `timeout` and no `< /dev/null`.** `run_tests.sh` uses
   `setsid timeout --kill-after=10 300 … < /dev/null` for exactly these reasons.

> I initially blamed `verify_multicompany_hardening.sh` for hanging. That was
> wrong — its `restart()` already does `setsid … < /dev/null &` correctly. Both
> hangs were my harness. Worth recording, because the wrong diagnosis would have
> sent someone to "fix" a script that was fine.

## 3. Restore, not reset

`run_tests.sh` now takes a `pg_dump` snapshot before the integration scripts and
restores it at the very end.

**A restore, not a wipe.** Demo data, the parts catalogue and the help articles
keep their contents *and their ids*; only the debris the run created disappears.
A `TRUNCATE`-based reset could not do this — it cannot put back a configuration
row a test changed (a decimal precision, a sequence, a setting), and it would
take the demo catalogue with it.

Measured across a full run: `account_move` **1253 → 1253**, demo parts 163,
help articles 76. The suite no longer changes the database at all.

```
./scripts/db_snapshot.sh take|restore|verify [file]
./scripts/run_tests.sh --keep-db      # snapshot still taken, restore skipped
```

### How it is made safe

- The snapshot is taken **after** the server is confirmed up, so the schema
  migrations a fresh boot performs are inside it — a restore must not undo them.
- It is **verified readable at snapshot time**, not at restore time. An archive
  that turns out to be corrupt only when it is needed is not a snapshot.
- The restore **refuses** a missing or unreadable file rather than proceeding.
- The server is stopped and other connections terminated first, because
  `--clean` drops objects and open connections hold locks that make that fail
  halfway.
- Afterwards the schema is sanity-checked; if the database does not look intact
  the script says so loudly and keeps the dump.
- The restore runs **before** the summary so a problem cannot hide under a wall
  of PASS lines — but a failed restore does **not** turn a green run red. The
  two report different things.

## 4. `verify_db_restore.sh` — 17 checks

An untested restore is worse than none, so the round trip is proved in both
directions: a row **added** before the snapshot is rolled back, and a row
**deleted** is brought back. Restoring only additions would be a wipe, not a
restore.

It also checks table count, two row counts and a specific pre-existing row's
content across the cycle, that the server comes back and a user can still log
in, and that missing and corrupt snapshots are both refused.

## 5. What this does and does not fix

Restore makes the leaks **harmless**: the suite can no longer poison the
database, and `verify_bank_recon` cannot be broken again by accumulation.

It does not make the eleven scripts **hermetic**. They still abandon rows within
a run, so a script that depends on "no other invoice has this amount" is still
fragile against scripts that ran before it in the same suite. The table above is
the work list; `audit_test_leaks.sh` is how you confirm each fix.

The 145 orphan invoices already in the database predate this and are preserved
by the restore, correctly — restore means "as it was", not "as it should be".
Clear them with **Settings → Database Tools → Reset** (docs/102).

## 6. Not done

- The 11 leaking scripts are not fixed, only measured.
- `verify_credit_note` fails when run standalone; it depends on fixtures
  `run_tests.sh` seeds before the suite. It passes in the suite, but that is a
  hidden dependency worth removing.
- The audit takes ~20 minutes because it runs every script twice-instrumented;
  it is a tool to reach for, not part of `run_tests.sh`.

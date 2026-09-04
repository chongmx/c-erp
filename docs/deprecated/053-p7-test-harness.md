# 053 — P7 (test harness) and the two P3/P1 loose ends

**Date:** 2026-08-04
**Implements:** `045` P7 · `033` §3 · closes the two items left open at the end of `052`
**Status:** ✅ Complete

---

## Part 1 — the two loose ends from `052`

`052` closed P3 and P1 but named two gaps. Both are now closed, and closing them
uncovered three real bugs that the existing tests could not have found.

### The FX settlement UI

The payment dialog now collects what the bank actually credited. When the invoice
is in a foreign currency it shows three fields — **Received (MYR)**, the derived
**effective rate**, and the resulting **FX difference** — and posts
`amount_received_base` alongside the payment. Same-currency invoices show nothing
new; the fields are conditional on `state.payIsForeign`.

The rate is displayed, never typed. The bank's spread makes any quoted rate wrong,
so the user enters the amount that landed and the rate is derived from it
(`048` §4.6).

### Tax on the invoice form

Invoice lines have a tax column with a picker bound to `tax_ids_json`. Selecting a
tax and saving regenerates the tax lines server-side.

One change this forced: the form loaded its editable lines with `[['debit','=',0]]`,
which also matches the tax lines the server generates. Adding a tax would have made
the generated line appear in the grid as an editable product line, and the next save
would have doubled the invoice. The query now carries `['tax_line_id','=',null]`.

> `null`, not `false`. The domain compiler turns null into `IS NULL`, whereas false
> binds the string `'false'` to an integer column.

---

## Three bugs found while verifying the above

Each was found by probing the running system, not by reading the code.

### 1. Every foreign-currency payment entry was out of balance

The journal entry was built like this:

```cpp
insertLine(drAccId, payAmount, 0.0);   // DR bank
insertLine(crAccId, 0.0, payAmount);   // CR receivable
...
// much later, once the allocator has run:
insertLine(fxId, /* the FX difference */);
```

Both legs were written **before** the allocator computed the FX, so both got the
same number. Adding the 7900 line then left the entry unbalanced by exactly the FX
difference — a corrupt journal entry on every foreign settlement.

Under a foreign settlement the two legs are *not* the same number, and the gap
between them **is** the realised FX:

```
100 USD invoice booked at 4.70        receivable carries 470.00 MYR
bank credits                                            448.50 MYR

    DR  Bank             448.50     <- what actually moved
    DR  FX loss           21.50     <- the gap
    CR  Receivable       470.00     <- what the AR was carrying
```

The receivable must be relieved at its **booked** base value or the customer's
ledger never clears; the bank must be debited with **what landed** or the cash book
is wrong. Both cannot be the same figure. The legs are now written after the
allocation, sized from it, and `insertLine` takes micro-units so the FX arithmetic
never round-trips through `double`.

### 2. `tax_ids_json` was silently dropped on write

Migration 1000 added the column and `recomputeTaxLines_` read it — but the field was
never added to `AccountMoveLine::registerFields()`, so `BaseModel::write()` discarded
it. The picker wrote `[1]`; the line came back `[]`. No error anywhere.

**The unit tests could not have caught this.** `TaxEngine` was correct and its 47
assertions passed throughout. The defect was entirely in the wiring between a
migration, a field registry and a model — which only an end-to-end probe touches.
This is the clearest argument for keeping the integration tier.

### 3. A supplier payment could never be allocated

`PaymentAllocation::allocate` hard-coded:

```sql
AND m.move_type IN ('out_invoice','out_refund')
```

So an **outbound** payment matched no document. Paying a supplier left the bill open
forever while the money sat as a permanent unallocated credit on the vendor. The
allocator now selects the document family from the payment's own direction.

This one was only exposed because a new test covered the mirror case. It matters
directly for the rental module, whose recurring expenses are supplier bills.

**The FX sign flips with direction, too.** On a receipt the gap balances on the
debit side and a shortfall is a *loss*; on a payment the legs are reversed, so the
same gap balances on the credit side and is a *gain* — the liability was settled for
less base currency than it was booked at. Both directions are now asserted.

---

## Part 2 — P7, the test harness

### What was wrong

Three test files, three hand-written `main()`s, three `g++` command lines in
comments, and **nothing in the build system referenced any of them**. "Run the
tests" meant remembering three compiler invocations. A test that stopped compiling
would have stayed broken silently, because nothing ever tried to build it.

### The harness

`tests/TestHarness.hpp` — cases register themselves at static-init time:

```cpp
ERP_TEST(Money, rounding) {
    ASSERT_EQ(Money::parse("1.005").roundTo(2), Money::parse("1.01"));
}
```

Two failure styles, both feeding one set of counters:

| | Behaviour | Use when |
|---|---|---|
| `ASSERT_*` | throws, aborting the case | later assertions would be meaningless or would crash |
| `CHECK` / `check()` | records and continues | table-driven sweeps — knowing 3 of 280 combinations fail beats knowing the first one does |

An aborted case prints `ABORTED` rather than a check count, so a case that died
half-way is never mistaken for one that completed.

### The target

```cmake
add_executable(erp_tests EXCLUDE_FROM_ALL ${TEST_SOURCES} core/Money.cpp core/TaxEngine.cpp)
```

`EXCLUDE_FROM_ALL` is deliberate: `cmake --build ./build` stays the fast path for
iterating on the server. Only the units under test are linked in — pulling in the
module set would drag in `Container` and every module's static registration, which
needs a live database to construct. These cases must keep running with no database
at all.

`scripts/test_sessionmanager.cpp` moved to `tests/test_session.cpp`; it now builds
with everything else and can no longer rot unnoticed.

### One command

`scripts/run_tests.sh` runs both tiers and exits non-zero if anything fails.

```
./scripts/run_tests.sh                     unit + integration
./scripts/run_tests.sh --unit              no database, no server, milliseconds
./scripts/run_tests.sh --unit --filter Tax one suite while iterating
```

**A missing verdict counts as a failure.** A script that dies before printing
`All checks passed.` is reported as failed, never skipped silently — the failure
mode that produced two false-pass verifications earlier in this project.

### Four problems found by running it, all in the tests rather than the product

Worth recording, because each one is a way a suite can look green while proving
nothing.

**1. Command substitution deadlocked the suite.** `out=$(bash "$s")` waits for every
writer to close the pipe. Several verify scripts restart the server, and a server
that stays up by design never closes it — the suite hung for the full timeout on the
first script that restarted anything. Output now goes to a file, with `</dev/null`
and a per-script `timeout`.

**2. `( nohup … & )` left the server as a child of the script.** Bash elides the
subshell when the background job is its only command, so `./build/c-erp` stayed a
direct child and the script sat in `do_wait` forever — visible as
`STAT S, WCHAN do_wait` with the server as its only child. Both the runner and
`verify_precision.sh` now use `setsid`, which puts the server in its own session.
This was the actual cause of the "orphaned" processes; they were not orphans at
all, but parents stuck waiting on a server that never exits.

**3. A test restored the database but not the server.** `verify_precision.sh` sets
Product Price to 3, checks that the change propagates, then restores 5 — via raw
SQL, which bypasses the cache-invalidation hook the API write path uses. The process
was left caching 3 while the DB said 5, so the *next* run failed with
`price_unit wrong` against a database that was perfectly correct. It now restarts
after restoring, and again before its first assertion so it cannot inherit a stale
cache from anything else.

**4. Two scripts had no verdict at all.** `verify_money_recompute.sh` and
`verify_s40_buckets.sh` printed values and expectations and left the judging to a
human reader — so they could never fail, and never pass. Both now assert. The runner
scores a missing verdict as a failure, which is what surfaced them.

**5. Cleanup broke exactly when a run had already gone wrong.** Test teardown built
`DELETE ... WHERE id IN ($A,$B)` by interpolation. When an assertion failed earlier,
`$B` was empty, producing `IN (12,)` — a syntax error that the `pg()` helper swallows
along with stderr. So a failing run leaked its rows, and the leaked rows then failed
the ledger-integrity check on every subsequent run, as a stale-data failure that
looks identical to a real accounting bug. Two lineless `P1TEST` invoice headers and
two `FXBILL` moves had accumulated this way. Teardown now filters ids to those that
are actually numeric and additionally sweeps by name prefix, so an interrupted run
is cleaned by the next one.

That last one produced a genuine investigation: ledger check 3 reported a sale order
whose header said 375.00 against a single line of 30.00. The obvious reading — that a
line write does not refresh the parent header — was wrong. `verify_order_totals.sh`
was written to decide it, and the header recomputes correctly; the 375.00 was left by
lines an earlier teardown had removed with raw SQL, which no recompute path would
ever observe. The script stays, because that question should be answerable by running
something rather than by reading `updateOrderTotals_` and hoping.

### The negative control

`scripts/negctl_run_tests.sh` plants two deliberately broken scripts — one that
declares failure, one that dies silently before any verdict — and asserts that the
runner reports both and exits non-zero.

Without this, "16 passed, 0 failed" is an unfalsified claim. Two verifications
earlier in this project passed while proving nothing, because a probe exited before
reaching its assertions and the absence of the word FAIL was read as success. A
suite that cannot be shown to go red is not evidence.

---

## Verification

`./scripts/run_tests.sh` — **17 passed, 0 failed.**

```
unit          3 cases, 120 assertions
                Money::all           52
                Tax::all             47   incl. 280-combination invariant sweep
                Session::lifecycle   21

integration   currency_rate 6    fx_settlement 15   ir_cron 9
              ir_sequence 5      money_display 3    money_recompute 7
              money_roundtrip 8  no_double_audit 9  order_totals 1
              payment_allocation 6  precision 5     precision_live 4
              s40_buckets 3      security_fixes 10  session_fixes 6
              tax_engine 13

negative      negctl_run_tests    3   runner exits 1, both failure shapes caught
control

ledger        verify_ledger_integrity.sql   10/10 exact
```

`verify_fx_settlement.sh` is new and covers all of Part 1: the FX settlement path
end to end (allocation row, derived rate, realised difference, exact-zero residual,
7900 posting, invoice left untouched, **entry balances**), the tax picker path
(value persists through `write()`, tax line generated, header populated, generated
lines excluded from the editable grid), and the vendor-bill mirror case in the
opposite FX direction.

---

## Prerequisite status for the rental module (`045`)

| # | Prerequisite | State |
|---|---|---|
| P1 | Payment allocation + FX | ✅ |
| P2 | Money as int64 + precision + multi-currency | ✅ |
| P3 | Tax engine | ✅ |
| P4 | `ir.sequence` | ✅ |
| P5 | `ir.cron` | ✅ |
| P6 | ViewModel pattern (ARCH-1) | ✅ |
| P7 | Test harness | ✅ |

**All seven prerequisites are complete, with no loose ends outstanding.** The rental
module (`040` §3) is unblocked.

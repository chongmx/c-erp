# 052 — P3 (Tax Engine) and P1 (Payment Allocation)

**Date:** 2026-08-04
**Implements:** `045` P3, P1 · `048` §3, §5, §4.6
**Status:** ✅ Both complete — migrations 1000/1010 applied, full regression green

---

## P3 — Tax engine

### The bug it replaces

`SaleModule` and `PurchaseModule` each carried the same fragment:

```cpp
if (atype == "percent" && !priceIncl)
    taxAmt += subtotal * taxRate / 100.0;
```

A **price-included tax contributed nothing** — the whole gross landed in the subtotal and the
document reported zero tax. Duplicated in two modules, so both were wrong identically. It also
worked in `double` with its own rounding, which P2 had replaced everywhere else.

### Two rules that make it correct

**1. Inclusive tax is derived by subtraction, never by multiplication.**

```
gross 108.00 including 8%:
    base = round(108.00 / 1.08) = 100.00
    tax  = 108.00 - 100.00      =   8.00     <- subtraction
```

Computing the tax as `base × 8%` instead yields 8.01 for some values, and then `base + tax` no
longer equals the price the customer was quoted. Subtraction makes that impossible — asserted
against ten deliberately awkward gross values and a 280-combination sweep.

**2. Round per line, then sum** (`048` §3 option A) — so the printed column always foots to the
printed total.

### Invoice tax lines — new capability

Invoice lines had **no tax association at all**. `account_move_line` carried `tax_line_id` (for
a line that *is* a tax) but nothing recorded which taxes a *product* line was subject to, so
`amount_tax` could only be copied from a sale order or typed by hand. Migration 1000 adds
`account_move_line.tax_ids_json` and `account_tax.account_id`, seeds **2200 Tax Payable**, and
`recomputeTaxLines_` generates one tax line per tax, posted to that tax's account.

**Rebuild, not append**: the function deletes generated tax lines (`tax_line_id IS NOT NULL`)
before regenerating, so calling it on every line edit is idempotent. Appending would have
multiplied the tax on each save.

### Three bugs found while building it

- **Currency guard rejected every foreign-currency line.** The engine built intermediates with
  the default currency 0 while `gross` carried the line's currency, so `Money`'s own guard threw
  *"cannot add amounts in different currencies (0 vs 2)"* — no USD invoice could be computed at
  all. Found by probing rather than reading. Now threaded through, with a regression test.
- **`untaxed` would have double-counted the tax.** Tax lines are credit lines too, so the
  existing `SUM(credit)` picked them up — inflating the invoice by the tax amount on every
  recompute. Fixed with `AND tax_line_id IS NULL`.
- **`amount_tax` was computed but never written.** The original `UPDATE` set untaxed, total and
  residual only — it had only ever *read* `amount_tax`. So the tax reached the ledger and the
  total, while the header still displayed zero. Caught by the end-to-end test.

### Deliberately out of scope

Tax-on-tax (compound), fiscal positions, per-country rules. The `sequence` field is carried so
compound can be added without a schema change, but nothing computes it — pretending otherwise
would be worse than the gap.

---

## P1 — Payment allocation

### What the scalar could not express

`residual = residual - paid` cannot represent:

- **one payment across several invoices** — the normal rental case, a tenant paying one
  transfer for three lockers;
- **an unallocated advance** — paying two months up front, when no invoice exists to decrement;
- **reversal** of a misapplied payment.

`account_partial_reconcile` makes allocations rows, and **residual is derived from them**, so
the two cannot disagree. "Fully paid" is now an exact `isZero()` rather than a `< 0.001`
epsilon — the point of P2 arriving where it matters most.

`account_payment_unallocated` is a **view**, not a column: a customer's credit is derived from
the allocations rather than stored beside them, so it cannot drift.

### Realised FX — the way the bank actually works (`048` §4.6)

The bank converts on receipt, so a USD invoice is settled in MYR. The user does **not** enter a
rate — the bank's spread makes any quoted rate wrong. They enter the MYR that landed, and the
effective rate is derived:

```
invoice 100 USD booked at 4.70   ->  470.00 MYR receivable
bank credits                          448.50 MYR
implied rate                           4.485000
realised FX loss                      -21.50 MYR  -> account 7900
```

Computed **per allocation**, so an invoice paid in two instalments at different rates gets the
right difference each time. The FX line is posted to the **payment's** journal entry, never to
the customer invoice — the customer owes the invoice amount regardless of what the ringgit did.

### Another `COUNT(*)+1` race, fixed

Payment journal entries were named by the same unlocked `COUNT(*) + 1` pattern that P4 removed
from invoice numbering — two concurrent payments produced the same entry name. Now
`ir.sequence` inside the payment transaction.

---

## Verification

```
tests/test_tax.cpp     47 checks (incl. 280-combination invariant sweep)
tests/test_money.cpp   52 checks
tax e2e                exclusive, inclusive, ledger tax lines, idempotent
                       recompute, entry still balances
payment allocation     one payment / two invoices; unallocated advance;
                       reversal restores residual exactly; FX arithmetic
sequence · precision · currency · display · roundtrip · no-double-audit
security · session · ledger integrity 10/10 · compliance 42/42
```

---

## Prerequisite status for the rental module (`045`)

| # | Prerequisite | State |
|---|---|---|
| P2 | Money as int64 + precision + multi-currency | ✅ |
| P6 | ViewModel pattern (ARCH-1) | ✅ |
| P4 | `ir.sequence` | ✅ |
| P5 | `ir.cron` | ✅ |
| P3 | Tax engine | ✅ |
| P1 | Payment allocation + FX | ✅ |
| P7 | Test harness | ✅ in practice — 99 unit assertions + 11 verification suites |

**All six prerequisites are done.** The rental module (`040` §3) is unblocked.

### Two things to finish before building on this

> **Both done — see `053`.** Closing them uncovered three real bugs: every
> foreign-currency payment entry was out of balance, `tax_ids_json` was silently
> dropped on write because the field was never registered, and a supplier payment
> could never be allocated at all.

1. ~~**The FX settlement UI.**~~ The payment dialog now collects what the bank credited and
   derives the rate from it.
2. ~~**Tax on the invoice form.**~~ Invoice lines have a tax picker bound to `tax_ids_json`.

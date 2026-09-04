# 048 — Configurable Precision & Multi-Currency Design

**Date:** 2026-08-03
**Extends:** `047` (int64 money migration — scale and storage decisions there still hold)
**Status:** Decisions recorded 2026-08-03 — ready to implement

> **Decided:** scale 6 (6 dp configurable ceiling) · rounding **option A** (round each line,
> then sum) · **FX realised difference implemented now**, with the rate entered by the user at
> settlement and posted to an FX gain/loss account.

---

## 1. The framing that makes this tractable

**Storage precision and display precision are different things, and only one of them is
configurable.**

| | Value | Configurable? |
|---|---|---|
| **Storage scale** | 6 dp (int64 micro-units) | **No** — internal, never user-visible |
| **Display / rounding precision** | 2, 4, 5 dp per role and per currency | **Yes** — this is what you asked for |

Everything is stored at 6 dp and *rounded on the way out* at defined boundaries. So "invoice at
2 dp, stock at 4 dp, this particular invoice at 4 dp" are all rounding rules applied to the same
exact underlying number — no schema change, no data migration, changeable at runtime.

**Ceiling:** scale 6 means configurable precision tops out at **6 dp**. Beyond that the storage
scale itself would have to change, which is another migration — so if you think you might ever
need 7–8 dp, say so now and I will use scale 8 instead (the trade is a maximum amount of
92 billion instead of 9.2 trillion — both unreachable for this business).

---

## 2. Configurable precision

### 2.1 `decimal_precision` table — user-editable in Settings

```sql
CREATE TABLE decimal_precision (
    id     SERIAL  PRIMARY KEY,
    name   VARCHAR NOT NULL UNIQUE,
    digits INTEGER NOT NULL DEFAULT 2 CHECK (digits BETWEEN 0 AND 6)
);
```

Seeded:

| name | digits | governs |
|---|---|---|
| `Product Price` | 5 | `price_unit`, `list_price`, `standard_price` |
| `Product UoM` | 4 | quantities |
| `Account` | 2 | invoice line subtotals, totals |
| `Discount` | 2 | discount percentages |
| `Stock` | 4 | on-hand quantities, moves |

Cached in `TtlCache`, invalidated on write. Exposed through the existing `ERPSettingsView`.

### 2.2 Per-document override — the resistor invoice case

You said *"some invoice may need 4 dp if they involve selling just resistors and capacitors."*

```sql
ALTER TABLE account_move ADD COLUMN line_precision INTEGER NULL
    CHECK (line_precision IS NULL OR line_precision BETWEEN 0 AND 6);
```

`NULL` (the default) means "use the `Account` global". Set it to 4 on a components invoice and
that document's lines render and round at 4 dp. Same field on `sale_order` and `purchase_order`.

### 2.3 Precedence

```
line amount precision  =  document.line_precision
                       ?? decimal_precision['Account']
unit price precision   =  decimal_precision['Product Price']
quantity precision     =  decimal_precision['Product UoM']
TOTAL precision        =  currency.decimal_places        ← always, never overridden
```

The last line is the important one. **The invoice total always rounds to the currency's
decimal places** — 2 for MYR and USD, 0 for JPY — because that is what can actually be paid.
A 4 dp line precision affects the lines, never the amount due.

---

## 3. The rounding tension — needs your decision

If lines display at 4 dp and the total rounds to 2 dp, **the total will not always equal the
sum of the visible line values.** Ten lines of `0.0425` display as `0.0425` each, sum to
`0.4250`, and the total shows `0.43`. A customer adding up the column gets `0.425`.

This is unavoidable arithmetic, not a bug. Three ways to handle it:

| Option | Behaviour | Trade |
|---|---|---|
| **A ← CHOSEN** | Round each line to document precision, sum the rounded lines, round that to currency precision | Total = sum of what is printed, to within one rounding step |
| B | Keep lines exact internally, round only the total | Total is "more correct" but can differ from the printed column by more |
| C | Option A + an explicit "Rounding" adjustment line when a difference remains | Fully reconciled and auditable; one extra line on some invoices |

**Decided: A.** The printed column always foots to the printed total. Option C (the reference ERP calls it
*cash rounding*) stays available as a per-company setting if an auditor ever requires exact
footing — it is additive to A, not a replacement, so choosing A now does not close that door.

---

## 4. Multi-currency

### 4.1 What exists

`res_currency` already carries `decimal_places` and `rounding` — JPY is correctly seeded 0 / 1.0,
so someone thought about this. What is missing:

- **MYR is not seeded** (only USD, EUR, GBP, JPY, CNY)
- **`res_company.currency_id` is NULL** — no base currency is set
- **No `res_currency_rate` table** and no conversion logic anywhere

Currency today is a display symbol, nothing more.

### 4.2 Base currency

`res_company.currency_id` → **MYR**, seeded and set. Changeable in Settings, with a hard
warning: changing base currency after documents exist invalidates every stored base-currency
amount, so it is effectively a one-time decision.

### 4.3 Rates

**Simplified to a single column** (see §4.6 — a dated rate table is not needed when the bank
converts on receipt and every document snapshots its own rate):

```sql
ALTER TABLE res_currency ADD COLUMN rate BIGINT NOT NULL DEFAULT 1000000;  -- scale 6
```

**Rate convention — stated explicitly because getting this backwards is the classic bug:**

> `rate` = **how many units of base currency equal 1 unit of this currency.**

So with MYR as base: `MYR = 1.000000`, `USD = 4.700000` (1 USD = 4.70 MYR). That reads the way
people speak, which is the point.

The column supplies the *default* when a foreign-currency document is created; the document then
carries its own snapshot (§4.4), which is what all later reporting uses.

### 4.4 Rate snapshot — the non-negotiable part

Every document that carries a foreign currency stores **the rate it used**:

```sql
ALTER TABLE account_move ADD COLUMN currency_rate BIGINT NOT NULL DEFAULT 1000000;
```

Without this, last year's invoices silently change value every time someone updates today's
rate. The snapshot is what makes historical reporting stable.

### 4.5 Two amounts per document

| Column | Meaning |
|---|---|
| `amount_total` | in the **document's** currency — what the customer owes |
| `amount_total_base` | in the **base** currency — what the ledger and dashboard use |

`account_move_line.debit` / `credit` stay **always in base currency** (standard double-entry),
with the existing `amount_currency` column holding the foreign amount. That column already
exists, so the model was anticipated.

Dashboard aggregates sum `*_base`, so mixed-currency figures are meaningful.

### 4.6 FX gain / loss — DECIDED: implement now, inside P1

You invoice **100 USD** when the rate is 4.70 → the ledger records **470 MYR**.
The customer pays two months later at 4.50 → you actually receive **450 MYR**.
That **20 MYR is a realised FX loss** and must be booked, or the ledger will not balance.

**Your specification:** at settlement the user enters the actual rate, and a line is added that
adjusts the invoice's value to base currency, booked to your FX gain/loss account.

That is correct accounting. One clarification on *where* the line goes, because it is a
correctness fork:

> **The FX line belongs in the payment's journal entry, not on the customer invoice.**
>
> Adding a line to the invoice would change what the customer owes — but they owe **100 USD**
> regardless of what the ringgit does. The FX difference is entirely yours. So the invoice is
> untouched; the settlement entry carries the adjustment.

I read "the product is actually my fx gain/losses" as naming the **account** it books to rather
than a product line on the invoice — flag it if you meant otherwise.

#### The settlement entry

Invoice 100 USD @ 4.70 (snapshot) = 470 MYR receivable. Paid in full, user enters 4.50:

| Account | Debit | Credit |
|---|---|---|
| Bank (base) | 450.00 | |
| **FX Gain/Loss** | **20.00** | |
| Accounts Receivable | | 470.00 |

Balances exactly. A rate *above* the snapshot reverses the sign and credits the same account.

```
fx_diff = (allocated_ccy × settlement_rate) − (allocated_ccy × invoice_snapshot_rate)
```

Computed **per allocation**, so a 100 USD invoice paid in two 50 USD instalments at different
rates produces its own correct difference each time. This falls out of P1's allocation model
rather than needing special-casing.

#### Settlement: enter the MYR received, not a rate

Your bank converts incoming foreign currency to MYR before it reaches you, so a USD invoice is
practically settled in MYR. **That makes this the simple case, not the hard one** — and it
changes the input we should ask for.

Asking for a *rate* would be the wrong question: the bank's effective rate includes its spread
and fees, so it is not a number you know. What you do know, exactly, is **how much MYR landed**.

```
Invoice           INV/2026/0042        100.00 USD
Booked at         4.700000    =        470.00 MYR   (snapshot, from the invoice)

Amount received (MYR)   [  448.50  ]                ← straight off the bank statement
Effective rate           4.485000                     (derived, shown read-only)
FX difference          − 21.50 MYR                    (loss → 7900)
```

The system derives `effective_rate = MYR_received ÷ foreign_amount` and stores it on the payment
for audit. Nothing to look up, nothing to get wrong, and the entry reconciles to the statement
by construction.

A rate field stays available for the case where you *do* receive foreign currency directly —
but it is not the default path.

#### What this setup removes

Because every settlement lands in MYR:

| Not needed | Why |
|---|---|
| Foreign-currency bank accounts | All cash is MYR; bank/cash journals are always base currency |
| Currency on `account_journal` | Same reason |
| Unrealised FX on cash balances | There are no foreign cash balances to revalue |
| **A dated `res_currency_rate` table** | See below |
| Second FX difference on unallocated credits | See below |

**Rate storage simplifies to a single column.** Each document already snapshots the rate it
used, so rate *history* lives on the documents. All the table would add is a dated default. For
this scale that is not worth a table plus a maintenance UI:

```sql
ALTER TABLE res_currency ADD COLUMN rate BIGINT NOT NULL DEFAULT 1000000;  -- scale 6
```

User-editable in Settings, used as the default when creating a foreign-currency document, and
overridable per document. A dated `res_currency_rate` table can be added later if you ever want
historical rate reporting — documents keep their snapshots either way, so nothing is lost by
deferring it.

**Unallocated credits stop being an FX problem.** Last turn I flagged that an advance payment in
USD would carry FX exposure between receipt and allocation. Under your setup it does not: the
bank converts on receipt, so the credit is held in **MYR**. Allocating it later is a plain
base-currency allocation with no second FX difference. If a customer overpays a 100 USD invoice
by 50 USD, that excess sits as an MYR credit at the rate you actually got.

**The one limitation to be aware of:** this assumes settlement always arrives converted. If you
ever open a USD bank account, foreign cash balances and their revaluation become real and this
model needs extending. Worth knowing, not worth building for now.

#### Account to seed

Your chart has no FX account (checked: 1000 Cash, 1100 Bank, 1200 AR, 2000 AP, 3000 Equity,
4000 Sales, 5000 COGS, 6000 OpEx, 9999 Undistributed).

Seeding **`7900 — Foreign Exchange Gain/Loss`** (type `expense`), a single account holding both
directions so it nets to the period's FX result — which matches "we can later balance our
account with it". Two separate gain/loss accounts are the alternative; say so if your accountant
prefers that split.

#### Deliberately out of scope

**Unrealised FX revaluation** — restating still-open foreign invoices at period-end rates. It is
a month-end close activity, needs a reversing journal, and is not required to keep the ledger
balanced day to day. Add it when a closing process exists.

---

## 5. How this changes P1 and P3

**P1 — payment allocation.** An allocation now carries both amounts and, if the payment and
invoice currencies differ, computes the FX difference (§4.6). `account_partial_reconcile` gains
`amount_base` and `fx_diff`.

**P3 — tax engine.** Tax computes in the document currency, then converts at the document's
snapshot rate. Rounding order is fixed: **compute tax per line → round to document precision →
sum → round to currency precision → convert to base.** Converting first and taxing second gives
different (wrong) answers.

---

## 6. `Money` becomes currency-aware

`047`'s type gains a currency and rounding helpers, but not a float anywhere:

```cpp
class Money {
    int64_t u_ = 0;          // scale 6, always
    int     ccy_ = 0;        // res_currency.id; 0 = base
public:
    Money roundTo(int dp) const;                 // half-up
    Money roundToCurrency(const Currency&) const; // honours decimal_places AND rounding step
    Money convertTo(const Currency& to, int64_t rate) const;   // uses the snapshot rate

    // Guardrail: adding two different currencies is a programming error.
    Money operator+(Money o) const;   // throws ValidationError on currency mismatch
};
```

That last guardrail matters more than it looks — silently adding USD to MYR is exactly the bug
this design exists to prevent, and it is invisible in a `double`.

`roundToCurrency` honours `rounding` as well as `decimal_places`, so a currency with a 0.05
rounding step (Swiss-franc style cash rounding) works without special-casing.

---

## 7. Effort

| Item | Effort |
|---|---|
| `decimal_precision` table + settings UI + cache | 2 d |
| Per-document `line_precision` override | 1 d |
| MYR seed, base currency, `res_company.currency_id` | 0.5 d |
| ~~`res_currency_rate` table + lookup~~ → single `res_currency.rate` column | 0.5 d *(was 2 d)* |
| `Money` currency-awareness + rounding helpers | 1 d |
| Dual amounts (`*_base`) on documents + recompute | 2 d |
| ~~Rate maintenance UI~~ → one editable field in Settings | 0.25 d *(was 1 d)* |
| Tests | 1.5 d |
| **Subtotal** | **~9 days** *(was ~11)* |
| FX realised difference (§4.6) — in scope, built inside P1 | +2–3 d *(was 3–4)* |

The bank-converts-on-receipt simplification removes the dated rate table, its maintenance UI,
foreign-currency bank accounts, and the unallocated-credit FX case — about **3 days**.

On top of `047`'s ~6 days: **combined P2 ≈ 3 weeks** (FX counted against P1).

---

## 8. Open questions

Decided: scale 6 · rounding A · FX realised difference in scope. Four things remain, none of
them blocking — I will proceed on the stated default and you can correct any of them:

| # | Question | Proceeding on |
|---|---|---|
| 1 | The FX line goes in the **payment journal entry**, not on the customer invoice (§4.6). Confirm I read "the product is my fx gain/losses" as naming the account? | Journal entry |
| 2 | One `7900 Foreign Exchange Gain/Loss` account, or separate gain and loss accounts? | Single account |
| 3 | Seed which currencies now — MYR + USD only, or also EUR/GBP/CNY (already seeded but rateless)? | MYR + USD active; the rest inactive until you set rates |
| 4 | Base = MYR, effectively permanent once documents exist. Confirm? | MYR |

~~One more worth naming: an advance payment in USD carries FX exposure between receipt and
allocation.~~ **Resolved** — the bank converts on receipt, so credits are held in MYR and carry
no further FX exposure (§4.6).

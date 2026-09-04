# 049 — P2 Implementation Progress

**Date:** 2026-08-03
**Implements:** `047` (int64 money) + `048` (precision & multi-currency)
**Status:** ✅ **P2 COMPLETE** — all phases implemented and verified · migration enabled
Deferred by design: per-document `line_precision` (§5), FX settlement UI (belongs to P1)

---

## 1. Done

### Phase 1 — `Money` type ✅

`core/Money.hpp` / `core/Money.cpp`. int64 micro-units (scale 6), currency-aware.

| Capability | Note |
|---|---|
| `parse` / `toString(dp)` / `fromDb` / `toDb` / `fromJson` / `toJson` | JSON stays major-units per `047` §3, so the 69 frontend sites are untouched |
| `+ - < >` with **currency guard** | Adding MYR to USD throws `ValidationError` rather than silently succeeding |
| `mulQty`, `mulInt`, `percent`, `prorate` | `__int128` intermediates — 10,000 cheap parts cannot overflow before rescaling |
| `split(n)` | Distributes the remainder so parts sum back **exactly** — for allocating one payment across invoices |
| `roundTo(dp)` | Half **away from zero** (2.5→3, −2.5→−3) |
| `roundToCurrency` | Honours `roundingStep`, so 0.05 cash rounding and 0-dp JPY work without special-casing |
| `convertTo`, `impliedRate` | Rate always passed in, never looked up — documents use their own snapshot |

**`tests/test_money.cpp` — 52 checks, all passing.** Written against the failures the earlier
demo actually produced, not invented cases:

- `0.00042 × 10,000`: accumulate and multiply now agree (they did **not** under `double`)
- The 7-line BOM sums to `74.0940` exactly
- Rent proration `300 × 17/31` + 8% tax → `177.68` per line; 12 of them foot to `2132.16`
- Both rounding policies computed side by side, asserting they genuinely differ — option A is
  the chosen one and the test says so
- `split(3)` of 10.00 sums back to exactly 10.00; negative amounts too
- Cross-currency addition throws; conversion carries the target currency
- Settlement: 100 USD booked at 4.70, 448.50 MYR received → implied rate `4.485000`, realised
  FX loss `−21.50` (`048` §4.6)
- Overflow throws instead of wrapping; 1 billion is comfortably representable

### Phase 2 — schema migrations ✅ written, dry-run verified

`core/MoneyMigrations.hpp` / `.cpp`, version range **900–999**:

| # | Contents |
|---|---|
| 901 | `decimal_precision` table (seeded 5/4/2/2/4) · `res_currency.rate` · **MYR seeded and set as base** · non-MYR/USD currencies deactivated · **`7900 Foreign Exchange Gain/Loss`** seeded |
| 902 | `line_precision` + `currency_rate` on account_move / sale_order / purchase_order · `amount_total_base`, `amount_residual_base` |
| 910–912 | `account_move_line` (incl. generated `balance` drop/recreate), `account_move`, `account_payment` (+ `amount_base`, `currency_rate`) |
| 920–960 | sale, purchase, stock, product, mrp |

**Verified against live data** via `scripts/test_money_migration.sql`, which runs the real SQL
inside a transaction and rolls back:

```
250.00      → 250000000        list_price 0.5000 → 500000
0.0500      → 50000            balance generated column: 0 rows wrong
round-trip back to major units matches the pre-migration values
MYR base currency set · 7900 created · all target columns BIGINT
*** ROLLED BACK — database unchanged ***   (confirmed: amount_total still numeric)
```

**A real bug was caught by that dry run**, which would otherwise have halted production
startup: `account_account` has its unique constraint on `(code, company_id)`, not `code`, so
`ON CONFLICT (code)` raises *"no unique or exclusion constraint matching"*. Also
`internal_group` defaults to `'asset'` and had to be set explicitly to `'expense'`. Both fixed;
the seed is now per-company and constraint-agnostic.

---

### Phase 3 — the conversion boundary ✅

**Done centrally rather than at 56 call sites.** The generic `BaseModel` path carries almost
all of it:

| Hook | Direction | Effect |
|---|---|---|
| `rowsToJson_` | read | scaled BIGINT → major units before JSON |
| `normalizeForDb_` | write | major units → micro-units before SQL |
| `FieldDef::scaled` + `FieldRegistry::markScaled()` | declaration | 23 columns marked across 10 models |

`FieldType` alone could not carry the distinction — `Float` covers both migrated quantities
(`product_uom_qty`) and untouched physical values (`weight`, `volume`, `purchase_lead_time`),
so it has to be declared. `markScaled()` **throws on an unknown field name**, so a typo aborts
boot rather than silently leaving a money column unscaled.

**A latent bug was found and fixed on the way:** `appendParam_` bound integers with
`v.get<int>()`. In micro-units RM 2,148 is already 2,148,000,000 — past `INT32_MAX` — so every
amount above ~RM 2,147 would have been silently truncated. Now `long long`.

Account payment paths converted by hand (raw SQL bypasses `normalizeForDb_`): the residual
read, the residual write-back, `account_payment.amount`, and both journal lines.

### Verified end to end, against real data

The migration was applied for real to the dev database, exercised, then rolled back by restore:

```
READ    250000000 micros  -> 250.0 on the wire            PASS
        500000 micros     -> 0.5                          PASS
        weight/volume     -> unchanged (not rescaled)      PASS
WRITE   12.34   -> 12340000 micros, reads back 12.34       PASS
        0.00042 -> 420 micros exactly, reads back exactly   PASS
        5000.00 -> 5000000000 micros (no int32 truncation)  PASS
RECOMPUTE  price_unit 10.00 x qty 3 -> subtotal 30.00,
           order total rolled up correctly                 PASS
```

The recompute path works because the architecture happens to be favourable: C++ computes in
major units via JSON, and SQL aggregation (`SUM(price_subtotal)` → `amount_untaxed`) stays
entirely within micro-units on both sides.

Database restored afterwards from `pg_dump`: `amount_total` back to `250.00 NUMERIC`, 1
migration row, 5 products, **zero restore errors**.

---

## 3. Outstanding

**Phase 4 is much smaller than estimated — 21 sites, not 48.** The generic boundary absorbed
the rest. Measured by grepping money columns read as `double` outside BaseModel:

| Module | Sites | Nature |
|---|---|---|
| portal | 14 | display in HTML/PDF (`price_unit` ×5, `amount_total` ×4, `quantity` ×2, `amount_untaxed` ×2, `amount_tax` ×1) |
| mrp | 3 | `product_qty` |
| stock | 2 | `quantity`, `product_uom_qty` |
| sale | 1 | `list_price` |
| purchase | 1 | `standard_price` |

All are `Money::fromMicros(row[...].as<long long>()).toJson()` — mechanical, ~0.5 day.

### Phase 4 ✅ — done, and it found three bugs the grep had missed

All 22 raw-SQL sites converted. **The migration is now enabled.**

| Fix | Where |
|---|---|
| 15 portal reads → `portalMoney()` helper | `PortalModule.cpp` |
| Report money funnelled through `reportMicros()` | `ReportModule.cpp` — `fmtMoneyField` / `fmtPrecF` are the single formatting funnel, so one change covers every document template |
| mrp ×3, stock ×2, sale ×1, purchase ×1 | direct conversions |
| Account raw-SQL writes | payment amount, residual write-back, both journal lines |

**Three things the column-name grep did not find:**

1. **`price_unit * quantity` in SQL — a scale-12 bug.** Three sites multiplied two
   micro-unit columns, giving micros² (a millionfold error). Fixed with `/ 1000000` in
   `PortalModule.cpp` ×2 and `ReportModule.cpp` ×1. This is the class of defect
   `047` §5 Phase 4 predicted: *SQL arithmetic between scaled columns needs rescaling.*

2. **`partner_rental_price.price_unit`** — a money column created by
   `PortalModule::ensureSchema_`, invisible to a FieldRegistry search because it is not
   declared through one. Added as **migration 970** plus its read and write.

3. **The report module reads money via helpers**, not by column name — `fmtMoneyField(f)`
   and `fmtPrecF(f, prec)` call `f.as<double>()` internally, so no amount of grepping for
   `["amount_total"].as<double>()` would have surfaced them.

**And the verification script itself had a false pass.** Its first version checked for
"7+ consecutive digits" and reported PASS while the report was rendering
`10,000,000.00` — comma separators defeated the regex. The human-readable output gave it
away. The check now compares against the DB value converted to major units:

```
before:  10,000,000.00   3,000,000.00   30,000,000.00
after:   10.00           3.00           30.00      (expected total 30.00 — matches DB)
```

Worth recording: an assertion that can pass while the thing it guards is broken is worse
than no assertion.

---

### Configurable display precision ✅ (docs/048 §2)

The framing that made this small: **storage precision and display precision are different
things, and only the second is configurable.** Everything stores at scale 6 and is *rounded on
the way out*, so "invoice 2 dp, stock 4 dp, resistor invoices 4 dp" are rounding rules over one
exact number — no schema change, no migration, changeable at runtime.

**`core/DecimalPrecision.{hpp,cpp}`** — cached reader over the `decimal_precision` table seeded
by migration 901. Singleton lifecycle matching `RuleEngine`/`AuditService`. Never throws: a
formatting lookup that can fail a request is worse than one that falls back to a default.

**`FieldDef::precisionName`** — `markScaled()` infers it from the field type (Monetary →
`Account`, Float → `Product UoM`), and `setPrecision()` overrides the handful that differ. So
23 columns got the right precision from two lines per model rather than 23 declarations.

**`fields_get` now emits `digits: [16, N]`** — the reference ERP convention the OWL client already
understands — plus `precision_name` for debugging.

Verified (`scripts/verify_precision.sh`):

```
price_unit         digits=[16, 5]   precision=Product Price
product_uom_qty    digits=[16, 4]   precision=Product UoM
discount           digits=[16, 2]   precision=Discount
price_subtotal     digits=[16, 2]   precision=Account
price_total        digits=[16, 2]   precision=Account
weight             digits=None      precision=None      <- correctly not a money column

changing Product Price 5 -> 3 dp propagates to fields_get   PASS
```

**One caveat:** the change currently needs a restart to be observed, because `fields_get` is
cached 300 s in the dispatcher *and* `DecimalPrecision` caches its own read. Wiring
`invalidateFieldsGetCache()` + `DecimalPrecision::invalidate()` into the Settings write is the
remaining piece — the invalidation methods exist, they are just not called yet.

---

### Settings UI ✅

**Backend.** `decimal.precision` model + ViewModel (`IrModule`), and `res.currency` promoted
from read-only `LookupViewModel` to a writable `CurrencyViewModel` — the FX rate is now
user-maintained, so read-only no longer fits.

**`core/CacheInvalidation.hpp`** — the missing link. `invalidateFieldsGetCache()` and
`invalidateCurrencyCache()` existed on the dispatcher but **nothing had ever called them**:
there was no path from a ViewModel to the dispatcher instance. A precision change would have
stayed invisible for 300 s, a rate change for 60 s. Container registers the hooks at boot;
ViewModels fire them by name.

**Validation moved into the ViewModel**, because `BaseModel::write()` does **not** call
`validate()`. Without it the only guard was the DB `CHECK`, whose pqxx error is gated behind
devMode by SEC-28 and reached the user as *"An internal error occurred"* — useless on a
settings screen. Now: *"Decimals must be between 0 and 6. Values are stored at 6 decimal
places, so more than that cannot be represented."*

**Frontend.** A "Precision & Currency" tab in `ERPSettingsView`: precision spinners per usage,
rate inputs per active currency with the base currency shown read-only (it is 1.0 by
definition). Optimistic updates roll back on error. Both panels carry a short note explaining
that storage is unaffected and that documents keep the rate they were booked at.

### Phase 5 ✅ — ledger integrity, asserted exactly

`scripts/verify_ledger_integrity.sql` — nine assertions, all **exact equalities** rather than
tolerance checks. That is the point of int64 money: before P2 every one of these would have
needed an epsilon.

```
1.  invoice untaxed = SUM(revenue lines)        6.  no money column still NUMERIC
1b. total = untaxed + tax                       7.  physical/layout columns NOT migrated
2.  every entry balances: SUM(dr) = SUM(cr)     8.  precision within storage scale
3.  sale order total = SUM(lines)               9.  active currencies have usable rates
4.  purchase order total = SUM(lines)
5.  residual within [0, total]
```

**It immediately found two real bugs**, which is the whole reason to write it:

- **`account_move_line.quantity` was marked scaled but never migrated.** The write path would
  have multiplied by 1e6 into a NUMERIC column, and the `price_unit * quantity / 1000000` SQL
  in portal/report assumed a micro-unit quantity that was not there. → migration **971**.
- **`mrp_bom.product_qty` was left NUMERIC while `mrp_bom_line.product_qty` was migrated.** A
  `replace_all` had converted three display sites, but two of them read `mrp_bom` — so those
  were dividing an unmigrated quantity by a million. → migration **972** plus the missing
  `markScaled`.

Check 6 now excludes `account_tax.amount` explicitly: that is a tax *rate* (`8.0` = 8%), not a
money amount, and is deliberately NUMERIC. Left as a standing false positive it would have
trained everyone to ignore the check.

---

## 4. Outstanding (deferred by design)

| Work | Why deferred |
|---|---|
| Per-document `line_precision` on invoices (docs/048 §2.2) | Column and CHECK exist (migration 902); the global precisions cover the stated need. Wire it when a document genuinely needs to differ from the company default |
| Frontend honouring `digits` when rendering | `fields_get` now carries it; `app.js` still formats with fixed decimals. Cosmetic — no correctness impact |
| FX settlement UI (MYR received → derive rate → post to 7900) | Belongs to **P1**: it is part of payment allocation, and the schema (`7900`, `currency_rate`, `amount_base`) is already in place for it |

---

## 6. Two testing lessons worth keeping

**An assertion that can pass while the thing it guards is broken is worse than no assertion.**
The display check tested for "7+ consecutive digits" and reported PASS while the report showed
`10,000,000.00` — comma separators defeated the regex. It now compares against the DB value
converted to major units.

**`pkill -f "build/c-erp"` matches the test script's own command line** and kills the shell
running it, which surfaced as a spurious FAIL. Use `pkill -x c-erp`.

---

## 3. Verification state

```
build                      clean
tests/test_money.cpp       52 checks, 0 failures
migration dry-run          passes, rolls back cleanly
server start               healthz 200, "[migrations] Schema is up to date."
account_move.amount_total  still `numeric` — migration correctly NOT applied
```

No regression: the running server is unaffected by anything in this change.

---

## 5. Deploying this

The migration is **enabled** and runs automatically at startup via `MigrationRunner`.

```bash
# 1. Back up first — this is the only rollback that matters
pg_dump -U odoo odoo | gzip > ~/pre-money-$(date +%F).sql.gz

# 2. Deploy the binary and restart. 13 migrations apply in one transaction each.
sudo systemctl restart c-erp

# 3. Confirm
grep 'Applied .* migration' log/system.log      # expect "Applied 13 migration(s)."

# 4. Verify against the running server
bash scripts/verify_money_roundtrip.sh
bash scripts/verify_money_display.sh
```

A failed migration halts startup rather than half-applying — that is
`MigrationRunner`'s designed behaviour and the right one here.

**Rollback** is restore-from-dump. There are no down-migrations, so the backup in step 1 is
not optional.

---

## 5. Files

**New**
```
core/Money.hpp                      value type — declaration + inline arithmetic
core/Money.cpp                      parse/format/round/convert
core/MoneyMigrations.hpp/.cpp       migrations 901–960
tests/test_money.cpp                52 checks
scripts/test_money_migration.sql    rolled-back dry run against live data
scripts/precision_demo.cpp          the evidence that motivated the change
```

**Modified**
```
modules/base/BaseModule.hpp   registerMigrations() declaration
modules/base/BaseModule.cpp   hook present, call gated off (§4)
```

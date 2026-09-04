# 074 — Numeric input scaling audit (the "330 → 0.00033" bug)

**Date:** 2026-08-11
**Status:** ✅ Root cause fixed at the single write boundary; every numeric input surface
audited; proven by `scripts/verify_money_string_write.sh`.
**Report:** setting a rental storage room's default rate to **330** saved it as **0.00033**
(a factor of exactly 1,000,000 — the scale-6 micro-unit scale).

---

## 1. Root cause — TWO bugs, both about numeric **strings**

Money/quantity columns are `BIGINT` micro-units (scale 6). A value of `330` must be stored as
`330000000` and read back as `330`. The conversion happens at the BaseModel boundary
(`normalizeForDb_` on write, `rowsToJson_` on read). It was gated on the value being a JSON
**number** — but the browser sends numbers as **strings**:

```
<input type="number">  ->  e.target.value  ->  "330"   (a STRING, not 330)
```

So two paths mishandled the string:

1. **write() / edit** — `normalizeForDb_` scaled only `if (val.is_number())`. A string skipped
   scaling and landed `"330"` in the BIGINT column raw → read back ÷1e6 = **0.00033**. *(This is
   exactly what the user hit: they edited an existing rate.)*
2. **create()** — routes the request through typed members; each model's `deserializeFields` reads
   values only `if (j[...].is_number())`, so a **string was silently dropped** and the member kept
   its `0` default → a new record stored **0**.

The backend was actually correct when a JSON *number* was sent (a curl probe with `330` stored
`330000000`); it was the string form the UI sends that broke.

## 2. The fix — one model-agnostic boundary (`modules/base/BaseModel.hpp`)

- **`coerceNumericStrings_(json&)`** — before either write path processes values, every registered
  **numeric** field (`Monetary` / `Float` / `Integer` / any `scaled`) whose value is a numeric
  string is coerced to a JSON number (`"330"` → `330`, `""` → `0`). Non-numeric strings and
  non-numeric fields are untouched. Called at the top of **both** `create()` and `write()`.
- **`normalizeForDb_`** also learned to scale a numeric string directly (via `Money::parse`), as
  defence in depth for any path that reaches it with a string.

One place, every model, every write path (generic form, CSV import, API clients).

## 3. Exhaustive checklist — every numeric input surface

| Surface | How it sends numbers | Before | After |
|---|---|---|---|
| **Generic FormView** (rental.unit.type, product, res.currency.rate, account.tax, uom, …) | `e.target.value` → **string** | ✗ create stored 0, edit stored ÷1e6 | ✅ coerced → scaled |
| Sale / Purchase / Invoice **line editors** | `parseFloat(...)` → **number** | ✅ worked | ✅ still works |
| Invoice/journal **manual lines**, payment register | `parseFloat(...)` → number | ✅ | ✅ |
| **CSV import** | strings | ✗ (same bug) | ✅ coerced |
| **API / scripts** sending JSON numbers | number | ✅ | ✅ |
| **API / scripts** sending numeric strings | string | ✗ | ✅ coerced |
| Custom OWL screens (BankReconcile, PartSearch, Barcode) | ids / non-money | n/a | n/a |
| Control-plane admin shared-product price | multiplies ×1e6 to a raw endpoint | ✅ (separate path) | ✅ |

## 4. Every scaled field is `BIGINT` (read side verified)

An audit (`information_schema`) of all `markScaled` columns confirmed **every one is `bigint`**, so
the read side scales correctly everywhere. The full set:

- **account** — move: `amount_untaxed/tax/total/residual`; move.line: `debit, credit,
  amount_currency, quantity, price_unit`; payment/analytic.line/bank.statement.line: `amount`;
  bank.statement: `balance_start/end`.
- **sale / purchase** — order: `amount_untaxed/tax/total`; line: `price_unit, product_uom_qty
  (product_qty), discount, price_subtotal/tax/total`.
- **product** — `list_price, standard_price, qty_available, quantity_svl, value_svl`; supplierinfo:
  `min_qty, price`.
- **stock** — move: `product_uom_qty, quantity`; quant: `quantity, reserved_quantity`; valuation:
  `quantity, unit_cost, value`; orderpoint: `product_min_qty/max_qty, qty_multiple`.
- **mrp** — `product_qty, qty_producing, qty_produced, costs_hour, min_to_replenish, forecast_qty`.
- **rental** — unit.type: `default_rate`; contract: `deposit_amount`; contract.line: `unit_price,
  discount_pct`; expense: `amount`.
- **base** — currency.rate: `rate`.

> Note: `account_tax.amount` is a **Float rate** (a percentage), correctly `numeric` and **not**
> scaled — flagged as a false positive by the name-based scan and confirmed benign.

## 5. Verification

`scripts/verify_money_string_write.sh` writes **string** values (exactly the browser's payload) and
asserts raw DB micros = `human × 1e6` AND read-back = `human`, on **create and write**, for
`rental.unit.type.default_rate`, `product.list_price/standard_price`, with a non-scaled tax rate as
the negative control. All green.

## 6. ⚠ Existing data already saved wrong must be re-entered

The fix prevents new corruption; it does **not** repair values already stored raw. Your rate that
saved as `0.00033` is physically `330` micros in the database — re-key it as `330` and it will now
store correctly. To find affected rental rates on the live DB:

```sql
-- suspiciously tiny rates (stored raw instead of ×1e6):
SELECT id, name, default_rate, default_rate/1e6 AS shown
FROM rental_unit_type WHERE default_rate > 0 AND default_rate < 1000;
```

The same pattern (`< 1000` micros = less than one-hundredth of a unit) finds any money column that
was written raw before the fix; I can prepare per-table repair statements if you want them.

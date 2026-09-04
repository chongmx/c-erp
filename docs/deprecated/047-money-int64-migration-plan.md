# 047 — Money as int64: Impact Assessment & Migration Plan

**Date:** 2026-08-03
**Decision:** int64 minor units for money, with precision sufficient for the cheapest components
**Supersedes:** `045` P2 (which proposed a 2-dp `Money` type — too narrow, see `046` discussion)
**Status:** Proposed — for review before implementation

---

## 1. Scale: 6 decimal places (micro-units)

| Scale | Resolution | int64 range | Verdict |
|---|---|---|---|
| 4 | 0.0001 | ±922 trillion | Too coarse — a 0.00042 resistor rounds to 0.0004 (−5%) |
| **6** | **0.000001** | **±9.22 trillion** | **Recommended** |
| 8 | 0.00000001 | ±92 billion | More precision than component pricing needs |

At scale 6 a resistor at RM 0.00042 is **420** micro-units — three orders of magnitude above the
resolution floor, so even a component ten times cheaper is still represented exactly. The
±9.22 trillion ceiling is far beyond any realistic amount.

**One internal scale for everything.** Money, unit prices and quantities all use scale 6. Mixed
scales are where conversion bugs live, and there is no storage saving — `BIGINT` is 8 bytes
regardless.

---

## 2. Impact assessment (measured, not estimated)

### 2.1 Database — 22 columns actually change, not 55

Of 55 `NUMERIC` columns, most are not money:

| Class | Columns | Action |
|---|---|---|
| **Money** (12) | `amount`, `amount_currency`, `amount_residual`, `amount_tax`, `amount_total`, `amount_untaxed`, `balance`, `credit`, `debit`, `price_subtotal`, `price_tax`, `price_total` | → `BIGINT` |
| **Price** (3) | `price_unit`, `list_price`, `standard_price` | → `BIGINT` |
| **Quantity** (6) | `product_qty`, `product_uom_qty`, `qty_delivered`, `qty_invoiced`, `qty_received`, `quantity` | → `BIGINT` |
| **Percent** (1) | `discount` | → `BIGINT` |
| **Not money — leave alone** (33) | `margin_top/right/bottom/left`, `line_height`, `footer_line_width`, `hours_per_day`, `purchase_lead_time`, `rounding`, `factor`, `volume`, `weight` | unchanged |

Quantities convert too, deliberately: `price_unit × quantity` must stay exact, and an int64
price multiplied by a `double` quantity reintroduces the float that the exercise is meant to
remove.

`balance` is a generated column (`GENERATED ALWAYS AS (debit - credit) STORED`) and must be
dropped and recreated as part of the type change.

### 2.2 Live data volume — trivial

```
sale_order_line     4 rows
account_move_line  19 rows
account_move        9 rows
product_product     5 rows
```

You said price data is disposable. It turns out it does not need to be — at this volume the
values convert losslessly in the same statement, so the migration can **preserve** them at no
cost and no risk. Discarding stays available as a flag if you would rather start clean.

### 2.3 Code

| Surface | Sites | Notes |
|---|---|---|
| C++ `as<double>()` on money | **56** | portal 19, sale 14, purchase 11, stock 4, account 4, report 3, BaseModel 1 |
| SQL arithmetic (`SUM`/`ROUND`/`COALESCE`) | **48** | `SUM(BIGINT)` returns `NUMERIC` in PG — still correct, but the result type changes |
| `FieldType::Monetary` / `Float` registrations | **46** | metadata only; drive display precision |
| **Frontend money sites** | **69** | app.js 66, portal.js 3 |

---

## 3. The decision that halves the work: keep the JSON boundary as-is

The 69 frontend sites are the single largest cost. They do not have to change.

**Proposal: int64 is server-side and storage-side. JSON continues to carry major units as a
number.**

```
PostgreSQL BIGINT (micro)  ──►  C++ int64_t (exact arithmetic)  ──►  JSON 164.52  ──►  browser
        ▲                                                                              │
        └──────────────────── int64 parsed from JSON, re-validated ────────────────────┘
```

Why this is sound rather than a shortcut:

- **The guarantee is kept where it matters.** Drift came from *accumulating* in `double` —
  summing lines, applying tax, splitting payments. All of that moves to int64. What crosses the
  wire is a finished value.
- **A `double` represents any single money value exactly enough for display.** 164.52 and
  0.000420 both round-trip through IEEE-754 to more digits than we display.
- **The frontend's arithmetic is provisional anyway.** Live line subtotals are a UX affordance;
  the server recomputes on save and its answer wins.
- **Zero changes to 69 sites**, and no risk of a half-migrated frontend showing 42000000 where
  it should show 42.00.

If frontend arithmetic ever becomes authoritative, switch JSON to scaled integers then — safe,
because JS integers are exact to 2^53, which at scale 6 is 9.007 billion currency units.

**Net: the change is ~22 columns + ~56 C++ sites, not ~150 sites.**

---

## 4. What gets built

### 4.1 `core/Money.hpp` / `.cpp` (PERF-E split)

```cpp
class Money {
    int64_t u_ = 0;                       // value × 1'000'000
public:
    static constexpr int SCALE = 6;

    static Money fromMicros(int64_t);
    static Money fromDb(std::string_view);      // BIGINT text → Money
    static Money fromJson(double);              // major units → Money (rounds at SCALE)
    static Money parse(std::string_view);       // "0.00042" → Money

    int64_t     micros()  const;
    int64_t     toDb()    const;
    double      toJson()  const;                // major units, for the wire
    std::string toString(int dp) const;         // display

    Money operator+(Money) const;
    Money operator-(Money) const;
    Money mul(Money) const;                     // __int128 intermediate, then rescale
    Money percent(Money pct) const;             // tax
    Money prorate(int days, int total) const;   // partial period
    Money roundTo(int dp) const;                // half-up, for invoice lines

    bool isZero() const;                        // replaces every `< 0.001` epsilon
};
```

`mul` and `percent` use `__int128` for the intermediate so `price × qty` cannot overflow before
rescaling.

### 4.2 Rounding policy — written down, applied once

**Round per invoice line, after tax, half-up to the currency's decimal places.** The invoice
total is then the exact sum of its rounded lines, so the ledger always reconciles with the
documents. (The earlier demo showed sum-of-rounded and round-of-sum differing by RM 0.03 over
12 invoices — that is a policy choice, not a type problem, and this is the choice.)

### 4.3 Tests, alongside (per your instruction)

- resistor pricing: 0.00042 × 10,000 exact; accumulate-vs-multiply identical
- overflow: `price × qty` at the int64 edge
- proration across 28/29/30/31-day months
- tax at 6/8/10%, inclusive and exclusive
- one payment split across three invoices summing to exactly zero residual
- round-trip: `Money → toDb → fromDb → Money` is identity

---

## 5. Migration plan

### Phase 0 — safety (before touching anything)

```bash
pg_dump -U odoo odoo | gzip > ~/pre-money-migration-$(date +%F).sql.gz
```

Restore this into a scratch database and confirm it restores **before** proceeding. The
production data is small, so this is cheap and it is the only rollback that matters.

### Phase 1 — `Money` type + tests (2 days)

Standalone. Nothing else changes; the type is unused until Phase 3. Build and test in
isolation.

### Phase 2 — schema migration (0.5 day)

Registered through the existing `MigrationRunner`, **version range 900–999** (extending the
table in `036`, which currently stops at 799). One migration per table, each in its own
transaction.

Values are preserved by the `USING` clause — no data loss despite the values being disposable:

```sql
-- account_move_line: balance is generated, so it must be dropped first
ALTER TABLE account_move_line DROP COLUMN balance;

ALTER TABLE account_move_line
    ALTER COLUMN debit      TYPE BIGINT USING ROUND(debit      * 1000000)::BIGINT,
    ALTER COLUMN credit     TYPE BIGINT USING ROUND(credit     * 1000000)::BIGINT,
    ALTER COLUMN price_unit TYPE BIGINT USING ROUND(price_unit * 1000000)::BIGINT,
    ALTER COLUMN quantity   TYPE BIGINT USING ROUND(quantity   * 1000000)::BIGINT;

ALTER TABLE account_move_line
    ADD COLUMN balance BIGINT GENERATED ALWAYS AS (debit - credit) STORED;
```

To discard instead, swap `USING ROUND(col * 1000000)::BIGINT` for `USING 0`.

**Order matters:** `account_move_line` before `account_move`, because the header totals are
recomputed from the lines afterwards.

### Phase 3 — C++ read/write sites (2 days)

56 sites, mechanically:

```cpp
double amt = row["amount_total"].as<double>();          // before
Money  amt = Money::fromDb(row["amount_total"].c_str()); // after
```

Order: `account` (the ledger, and where `isZero()` replaces the `< 0.001` epsilon) → `sale` →
`purchase` → `stock` → `portal` → `report`.

Serialization stays put — `serializeFields` emits `j["amount_total"] = amt.toJson()`, so the
JSON shape is unchanged and the frontend does not move.

### Phase 4 — SQL arithmetic review (1 day)

48 sites. Most need nothing: `SUM(price_subtotal)` over `BIGINT` returns `NUMERIC` and is still
exact. What does need attention:

- any SQL doing `ROUND(x, 2)` on a money column — now rounding micro-units, so the constant changes
- any comparison against a literal (`WHERE amount_residual > 0.001`) — becomes `> 0`
- any division for display in SQL — must divide by 1e6

I will grep and review each rather than assume.

### Phase 5 — recompute + verify (0.5 day)

Recompute stored totals from lines, then assert:

```sql
-- every invoice total must equal the sum of its lines, exactly
SELECT m.id, m.amount_total, SUM(l.price_total)
FROM   account_move m JOIN account_move_line l ON l.move_id = m.id
GROUP  BY m.id, m.amount_total
HAVING m.amount_total <> SUM(l.price_total);   -- must return zero rows
```

With int64 this is an exact equality, not a tolerance check — which is the point of the whole
exercise.

### Phase 6 — deploy

The production server runs the migration automatically at startup via `MigrationRunner`. So:
`pg_dump` first, deploy the binary, restart, confirm the migration logged, then run the Phase 5
assertion against production.

---

## 6. Risk

| Risk | Mitigation |
|---|---|
| A missed `as<double>()` silently reads micro-units as a currency amount (42000000 instead of 42.00) | Wrong by 10⁶ — visually obvious, not subtle. Phase 5 assertion catches it; grep for `as<double>()` on the money column list must return zero after Phase 3 |
| Generated `balance` column blocks the type change | Explicitly dropped and recreated in the migration |
| `ROUND(x, 2)` left in SQL now rounds micro-units | Phase 4 reviews all 48 sites individually |
| Migration half-applies | One transaction per table; `MigrationRunner` halts startup on failure |
| Frontend shows raw micro-units | Cannot happen — the JSON boundary is unchanged (§3) |

**Effort: ~6 days.** Slightly above the 2–3 days in `045`, because that estimate assumed a
2-dp money type and no schema change.

---

## 7. Confirm before I start

1. **Scale 6** — enough headroom below your cheapest component?
2. **Preserve existing values** (default) or discard with `USING 0`?
3. **JSON boundary unchanged** (§3) — this is what keeps the frontend out of scope.
4. **Quantities convert too** — needed to keep `price × qty` exact.
5. **Rounding: per line, after tax, half-up** (§4.2).

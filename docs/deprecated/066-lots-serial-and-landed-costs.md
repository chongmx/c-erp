# 066 — Lots & serial numbers, and landed costs

**Date:** 2026-08-08
**Status:** ✅ Complete and verified (2 new integration suites, all green; full suite green)
**Builds on:** `064` (on-hand), `065` (costing). This adds per-unit **traceability** and the
ability to **capitalise freight/duty/handling into stock value**.

---

## 0. What this adds

| Feature | Capability | Suite |
|---|---|---|
| Lots & serial | on-hand + reservation + traceability per lot/serial; enforcement | `verify_lot_serial.sh` |
| Landed costs | freight/duty/handling on a receipt, distributed into inventory value + GL | `verify_landed_cost.sh` |

Answers the two open items from `062`/`065`: lots/serials were the last inventory-traceability
gap, and "can I add transportation/tax/handling per batch?" — now yes, via landed costs.

---

## 1. Lots & serial numbers

- **`product.tracking`** = `none` / `lot` / `serial`.
- **`stock.production.lot`** — a lot/serial-number record (code + product), with a **traceability**
  action returning the lot's current on-hand by location and its full move history.
- The quant engine gained a **lot dimension**: on-hand is now keyed per **(product, location,
  lot)** — `stock_quant.lot_id` (0 = untracked), the unique key widened accordingly, and
  `applyMove`/`reserve`/`release`/`availableAt` all thread the lot. **Valuation stays per
  product** (a lot doesn't change cost method), so costing is untouched. `qty_available` still
  sums across a product's lots.
- **Picking flows** carry the lot on `stock_move.lot_id` and pass it through reserve/validate.
- **Enforcement on validate**: a `lot`/`serial` product refuses to validate without a lot, and a
  `serial` move must be exactly one unit.

Verified: two lots of one product hold separate on-hand (10 + 5, total 15); a delivery draws the
named lot down (LOT-A → 6, LOT-B untouched); traceability returns on-hand 6 and the receipt +
delivery history; a serial holds exactly 1; and validation is refused without a lot / for a
2-unit serial.

---

## 2. Landed costs

**`stock.landed.cost`** (linked to a receipt) + **`stock.landed.cost.line`** (the extra costs —
freight, duty, handling — each with an amount, a **split method**, and an account). On
`button_validate` the engine distributes every cost line across the products received on that
receipt and **capitalises** each product's share into its inventory value.

Split methods, all verified in one document:

| Method | Basis | Example (Freight 60, Duty 30, Handling 20, Insurance 30) |
|---|---|---|
| `by_quantity` | received quantity | Freight 60 over 10:5 → **A +40, B +20** |
| `by_price` | received value (qty × cost) | Duty 30 over 100:100 → **A +15, B +15** |
| `equal` | one share per product | Handling 20 over 2 → **A +10, B +10** |
| `by_weight` | qty × weight | Insurance 30 over 20:5 → **A +24, B +6** |
| `by_volume` | qty × volume | (same shape as weight) |

Each allocation calls **`StockQuant::revalue`** — a new pure value adjustment (no quantity
change): it writes a zero-quantity valuation layer, raises `value_svl`, and for **average**
re-derives `standard_price`, for **FIFO** pushes the added value onto the remaining cost layers
so it flows out with the right units. Then a **journal entry** posts **Dr Stock Valuation / Cr
Landed Costs (5200)** — capitalising the cost out of expense and into the balance-sheet stock
value — and links back to the layer.

Verified: A capitalises 100 → 189, B 100 → 151 (sum of shares = 89 + 51 = the 140 total), the
landed cost is `done`, and the GL shows Dr 1400 = 140 / Cr 5200 = 140.

---

## 3. Deliberately simpler than the reference ERP

- Lots: one lot per move (no split of a single move across several lots); reservation is
  per-lot but there is no automatic lot **assignment** strategy (FEFO/expiry) — the operator/API
  names the lot. No expiry-date fields yet.
- Landed costs: one receipt per landed-cost document (the reference ERP allows several); the whole cost is
  capitalised into stock value (no split to a price-difference account for already-sold units);
  the counterpart is a single Landed Costs account, not per-cost-type accrual accounts.

Neither limits the core capability: trace a lot/serial end-to-end, and get true landed cost into
inventory value and the ledger.

---

## 4. Security & conventions

- **ACL.** `stock.production.lot`, `stock.landed.cost`, `stock.landed.cost.line` are all behind
  `INVENTORY_USER` (11), deny-by-default.
- **Single writer.** Value still changes only through `core::StockQuant` — now `applyMove` (moves)
  and `revalue` (landed costs) — so on-hand, per-lot quants, and inventory value stay consistent.
- **Money discipline.** Quantities/costs/values are BIGINT micro-units; allocation ratios use
  `double` only for the split proportion, with the rounding remainder folded onto the last
  product so the distributed total always equals the cost exactly.
- **Bound SQL** throughout; the only interpolation is fixed column/account codes.

---

## 5. Verification

```
verify_lot_serial.sh    per-lot on-hand; deliver the named lot; traceability (on-hand + history);
                        serial = 1 unit; validation refused without a lot / for a 2-unit serial
verify_landed_cost.sh   four split methods distribute correctly; value_svl capitalised per share;
                        GL Dr Stock Valuation / Cr Landed Costs ties to the total

full suite              ./scripts/run_tests.sh — all green; the lot dimension on applyMove and the
                        widened quant key regressed none of the existing stock/mrp/costing flows
```

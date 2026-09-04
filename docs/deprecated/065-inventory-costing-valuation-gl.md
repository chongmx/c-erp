# 065 — Inventory costing: valuation layers, three cost methods, real-time GL

**Date:** 2026-08-08
**Status:** ✅ Complete and verified (2 new integration suites, all green; full suite green)
**Builds on:** `064` (on-hand `stock.quant`). This adds the *value* on top of the *quantity*
— the remaining inventory gap `062`/`064` flagged.

---

## 0. What this adds

| Phase | Capability | Suite |
|---|---|---|
| A | `stock.valuation.layer` + three cost methods (standard / average / FIFO) | `verify_stock_valuation.sh` |
| B | Real-time GL: each move posts a balanced journal entry | `verify_stock_valuation_gl.sh` |

Before this, on-hand was a *quantity* only. Now every product carries an **inventory value**
that moves correctly with each receipt, delivery, and manufacture, and (Phase B) that value is
mirrored into the general ledger in real time.

---

## 1. The valuation engine — one hook, three methods

The valuation logic lives in `core/StockQuant` (`valuateMove_`), fired by `applyMove` — so the
**same choke point that moves quantity also moves value**, and the two can never drift. It runs
only when a move crosses the **owned-stock boundary** (an `internal` location on exactly one
side); an internal↔internal transfer changes neither on-hand nor value.

Each qualifying move writes a `stock_valuation_layer` (signed `quantity` + `value`, micro-units)
and maintains two caches on the product: `quantity_svl` (valued quantity) and `value_svl`
(inventory value = Σ layers).

| `cost_method` | Incoming values at | Outgoing values at |
|---|---|---|
| `standard` | input cost, else `standard_price` | `standard_price` |
| `average` (AVCO) | input cost; then `standard_price` is refreshed to the new weighted average | current average (`standard_price`) |
| `fifo` | input cost; layer carries `remaining_qty`/`remaining_value` | oldest layers first, until the quantity is filled (shortfall at `standard_price`) |

**Input cost** (what an incoming move is valued at) is threaded from where it is actually known:

- **purchase receipt** → the PO line's `price_unit`, so AVCO/FIFO blend the real purchase cost;
- **manufacturing** → the finished good is valued at its **build cost** (total consumed
  component value ÷ produced quantity), so an MO conserves inventory value — component value
  flows out of components and into the finished product;
- everything else (inventory adjustment, subcontract backflush) → the product's `standard_price`.

Verified (`verify_stock_valuation.sh`): standard holds a fixed cost; average blends 10@5 + 10@7
into an average of 6 and issues there; FIFO consumes 10@5 then 5@7 on a 15-unit issue, leaving
5@7 = 35. A read-only **Inventory Valuation** report (`stock.valuation.layer`) browses the ledger.

---

## 2. Real-time GL — value on the balance sheet

Phase B posts a **balanced two-line journal entry** for every valuation layer, in a dedicated
**Inventory journal (STJ)**: the **Stock Valuation** account on one side, and a counterpart
chosen by the virtual location's `usage` on the other —

| Move | Counterpart | Entry |
|---|---|---|
| receipt (from `supplier`) | Stock Interim (Received) | Dr Stock Valuation / Cr Interim |
| delivery (to `customer`) | COGS | Dr COGS / Cr Stock Valuation |
| manufacture (`production`) | Stock Interim (Production/WIP) | consume Cr / produce Dr — nets to zero |
| adjustment (`inventory`) | Inventory Adjustment | Dr/Cr by direction |

Accounts resolve from the **product category** (`property_stock_*` override fields) and fall back
to seeded defaults by code (`1400` Stock Valuation, `1410` Interim Received, `1430`
Interim Production, `5100` Inventory Adjustment, `5000` COGS) in a new **Inventory journal**.
If the journal or an account is missing, the posting **silently skips** — valuation still tracks
without the GL, so the feature is safe on an un-configured company.

The load-bearing property, verified in `verify_stock_valuation_gl.sh`: after a receipt and a
delivery the **Stock Valuation account balance equals the product's `value_svl`** — inventory on
the balance sheet ties out to the inventory value exactly. Every entry balances, is `posted`,
and links back to its layer (`account_move_id`).

> **Seed gotcha, fixed.** `seedChartOfAccounts_`/`seedJournals_` guard on "no rows yet", so on an
> existing database they never add new accounts. The valuation accounts + STJ journal are seeded
> by a separate `seedStockValuationAccounts_()` that runs **unconditionally and idempotently**
> every boot, so an already-populated DB gains them too.

---

## 3. Deliberately simpler than the reference ERP

- No landed costs, no revaluation wizard, no separate price-difference account on receipt
  (the receipt is valued at PO price directly).
- Manufacturing GL uses a single Production/WIP interim account (consume + produce net to zero),
  not per-operation WIP tracking.
- Delivery posts COGS immediately (perpetual), not a deferred "interim delivered" that clears on
  invoice — there is no Anglo-Saxon interim-delivered step.
- FIFO shortfalls (issuing more than any layer holds, under allow-negative) are valued at
  `standard_price`; there is no negative-layer back-correction when stock is later replenished.

None of these blocks the core "what is my stock worth, and is it on the balance sheet" question.

---

## 4. Security & conventions

- **ACL.** `stock.valuation.layer` is read-only (engine-written) and gated behind `INVENTORY_USER`
  (11); the category account fields ride on the existing `product.category` access.
- **Single writer.** Value is only ever mutated through `core::StockQuant`, alongside quantity,
  so on-hand and inventory value are always consistent within one transaction.
- **Money discipline.** Every quantity/cost/value is BIGINT micro-units; the layer maths uses a
  128-bit intermediate (`mulMicros`/`divMicros`) so `qty × cost` cannot overflow.
- **Bound SQL.** All values are `$N`-bound; the only interpolation is a fixed account-property
  column name chosen from a closed set.

---

## 5. Verification

```
verify_stock_valuation.sh     standard / average / FIFO value correctly through receipts,
                              deliveries and re-priced restocks; value_svl + FIFO remainders
                              exact; ledger queryable
verify_stock_valuation_gl.sh  receipt Dr Valuation/Cr Interim; delivery Dr COGS/Cr Valuation;
                              Stock Valuation balance == product value_svl; every entry
                              balanced, posted, and linked to its layer

full suite                    ./scripts/run_tests.sh — all green; the applyMove valuation hook
                              regressed none of the existing stock/mrp/accounting flows
```

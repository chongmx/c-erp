# 064 — stock.quant, manufacturing (MO / work orders / subcontracting), MPS

**Date:** 2026-08-07
**Status:** ✅ Complete and verified (5 new integration suites, all green; full suite green)
**Closes:** the two largest feature gaps called out in `062` §2 — real on-hand
(`stock.quant`) and manufacturing depth (work orders, routings, MPS, subcontracting).

---

## 0. What this adds

Five phases, each dependency-ordered on the one before and each with its own
integration suite:

| Phase | Capability | Suite |
|---|---|---|
| 1 | `stock.quant` — real on-hand, reservation, inventory adjustment | `verify_stock_quant.sh` |
| 2 | `mrp.production` (MO) + work centers + BOM routing operations | `verify_mrp_production.sh` |
| 3 | `mrp.workorder` — per-operation execution + MO close-gate | `verify_mrp_workorder.sh` |
| 4 | Subcontracting — the receipt IS the manufacturing event | `verify_mrp_subcontract.sh` |
| 5 | MPS — forecast → projected stock → suggested replenishment | `verify_mrp_mps.sh` |

Before this, on-hand quantity existed **nowhere** in the system (`062` §2: "there is no
on-hand quantity anywhere"), and MRP was `mrp.bom` + `mrp.bom.line` only — BOMs that
could not be manufactured. Both are now real.

---

## 1. `stock.quant` — the on-hand foundation

The quant engine (`core/StockQuant`) is the **single writer** of on-hand, beside
`TaxEngine`/`PaymentAllocation` as a core service so both stock and mrp call it without a
module dependency. All quantities are int64 **micro-units** (scale 6), matching
`stock_move`.

- **One row per (product, location)** in `stock_quant` — `quantity` and
  `reserved_quantity`. No lot/package/owner split (we have no lots), so `UNIQUE
  (product_id, location_id)` and a simple upsert replace the reference ERP's merge logic.
- **On-hand = Σ quantity over `usage='internal'` locations.** Virtual locations
  (supplier/customer/production/inventory/subcontract) hold quants too — they are the
  other end of every move — but are not counted as owned stock.
- **`applyMove(product, src, dest, qty)`** decrements the source quant, increments the
  destination, and refreshes `product_product.qty_available` (a denormalised on-hand
  cache, kept transactionally consistent so the product list/MPS read it without a join).
- **Reservation.** `action_assign` reserves up to the available quantity at the source and
  records it on `stock_move.reserved_qty`; `button_unreserve`/`action_cancel` release the
  exact amount; `button_validate` releases then applies the real delta. This replaced the
  three stubs the module shipped with (`action_assign` was literally commented
  "availability check stub").
- **Allow-negative** (the chosen policy): a validate never refuses; a move that drives a
  location below zero records a negative quant, reconciled later by an inventory
  adjustment. `reserve()` still only reserves what is physically present.
- **Inventory adjustment** (`stock.quant.set_on_hand`) books the difference between counted
  and current on-hand as a move to/from the *Inventory Adjustments* location — every
  correction leaves an auditable ledger entry.
- **On Hand report** (`stock.quant` read-only viewmodel) lists internal-location quants
  with a derived `available_quantity`. Quants are engine-managed, so no create/write/unlink
  is exposed.

---

## 2. Manufacturing core — MO, work centers, routing

- **`mrp.workcenter`** — a machine/station (cost/hr, efficiency, capacity).
- **`mrp.routing.workcenter`** — operations attached **directly to the BOM**, the reference ERP-14
  design (there is no standalone routing model). Each row is one step on one work center
  with a per-unit cycle time.
- **`mrp.production`** (the MO). On `action_confirm` it **explodes the BOM**: one raw-material
  `stock_move` per component (`WH/Stock → Production`) plus one finished-good move
  (`Production → WH/Stock`), each scaled `line_qty × MO_qty ÷ BOM_qty` in integer
  micro-arithmetic. It draws a gapless **`MO/00001`** number from a new `ir.sequence`.
  `button_mark_done` consumes and produces every move through the quant engine.
- A new **Production** virtual location (`usage='production'`) is the transient place
  components are consumed into and finished goods produced from. `stock_move.picking_id`
  became nullable so a move can belong to an MO instead of a picking; moves carry
  `production_id` + `is_production_raw`.

---

## 3. Work orders

`action_confirm` also creates one **`mrp.workorder`** per BOM operation, with an expected
duration = per-unit cycle time × order quantity. `button_start` → `progress` (and moves the
MO to `progress`); `button_finish` → `done`, logs the duration, and when the last open work
order finishes the MO becomes `to_close`. **`button_mark_done` is gated**: it refuses while
any work order is still open, so the shop-floor sequence is enforced, not optional.

---

## 4. Subcontracting — the receipt is the event

A BOM may be `bom_type='subcontract'` with a `subcontractor_id`. When a **vendor receipt**
is validated for a product that has a subcontract BOM, the finished good lands on-hand
normally *and* its components are **backflushed** — consumed from `WH/Stock` into a new
**Subcontracting** virtual location (`usage='subcontract'`, so it is not counted as owned
on-hand: the stock is "out at the vendor"). Receiving the finished product *is* the
manufacturing event, exactly as the reference ERP models it. A normal receipt backflushes nothing.

The hook lives in `stock.picking.button_validate` and touches the `mrp_bom` tables with
plain SQL — no reverse module dependency (stock does not include an mrp header).

---

## 5. MPS — master production schedule

`mrp.production.schedule` (one row per product, with a safety-stock `min_to_replenish`) plus
`mrp.forecast` (demand per month). `get_mps_grid` runs the **time-phased projection**: for
each month, on-hand carries forward, the month's forecast demand is subtracted, and a
replenishment is suggested whenever the projection would fall below the safety level —

```
projected = incoming − forecast
replenish = max(0, min_to_replenish − projected)
ending    = projected + replenish   (carries to next month)
```

`action_replenish` turns a suggested quantity into a **draft** MO (origin `MPS`) for the
planner to review before launching — the proactive layer above reactive reordering.

---

## 6. Security & conventions

- **S-49 (column allowlist).** The `mrp.production` custom `search_read` compiles its domain
  with an explicit stored-column allowlist (`toSql(&kCols)`), so a filter can never name an
  unregistered column. The other new read paths filter by a single parsed integer
  (`production_id` / `bom_id`), not a free domain.
- **ACL (deny-by-default).** Every new manufacturing model is gated behind `MRP_USER` (group
  13) in `JsonRpcDispatcher`; `stock.quant` behind `INVENTORY_USER` (11). No model relies on
  the base-internal default.
- **Injection.** All values are `$N`-bound `pqxx::params`; the only interpolation is
  integer id-lists and the fixed-set allowlist. No `std::system`, no string-built SQL from
  user input.
- **Money/quantity discipline.** Every quantity is BIGINT micro-units + `markScaled`;
  conversions happen at the model boundary, so the wire format is unchanged.
- **Single-writer quant.** On-hand is only ever mutated through `core::StockQuant`, so stock
  validation, manufacturing consume/produce, and subcontract backflush cannot drift apart.

---

## 7. Deliberately simpler than the reference ERP

Honest scope, so nobody is surprised later:

- Quants are not split by lot/serial/package/owner (we have no lots) — one row per
  (product, location).
- Work-order scheduling is duration-only; there is no work-center capacity calendar or
  finite-scheduling Gantt.
- Subcontracting uses one shared Subcontracting location, not a per-subcontractor
  sub-location tree, and consumes components from `WH/Stock` at receipt (backflush) rather
  than tracking a prior component transfer to the vendor.
- MPS periods are monthly and single-warehouse; replenishment suggests manufacture (a draft
  MO), not purchase.

None of these blocks the core flows; each is an additive extension.

---

## 8. Verification

```
verify_stock_quant.sh      receipt raises on-hand + qty_available; reserve/unreserve;
                           validate consumes; inventory adjustment; allow-negative;
                           On Hand report excludes virtual locations
verify_mrp_production.sh   confirm explodes BOM (scaled raw+finished moves); MO/#####;
                           mark-done consumes+produces via quant; workcenter/routing CRUD
verify_mrp_workorder.sh    WO created from operation; MO close blocked while WO open;
                           start→finish→to_close; then mark-done consumes+produces
verify_mrp_subcontract.sh  subcontracted receipt backflushes components into Subcontracting;
                           normal receipt backflushes nothing
verify_mrp_mps.sh          time-phased projection (3 months) + suggested replenishment;
                           action_replenish creates a draft MO

full suite                 ./scripts/run_tests.sh — all green (the stock rewire regressed
                           nothing: sale/purchase delivery/receipt flows still pass)
```

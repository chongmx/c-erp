# 067 — Warehouse automation: reorder rules, putaway, barcode

**Date:** 2026-08-08
**Status:** ✅ Complete and verified (3 new integration suites, all green; full suite green)
**Builds on:** `064` (on-hand), `065` (costing), `066` (lots/landed costs). This is the
warehouse-automation tier `062` listed as the remaining inventory gap.

---

## 0. What this adds

| Feature | Capability | Suite |
|---|---|---|
| Reorder rules | per-product min/max → auto draft PO (buy) or MO (make) | `verify_reorder.sh` |
| Putaway | route incoming products to designated sub-locations on receipt | `verify_putaway.sh` |
| Barcode | barcode fields on product/location/lot + a scan resolver | `verify_barcode.sh` |

---

## 1. Reorder rules — `stock.warehouse.orderpoint`

A per-product rule: **min / max**, an order **multiple**, a **route** (buy or manufacture),
and (for buy) a **vendor**. The scheduler — `run_scheduler`, and an `ir.cron` job
(`stock.reorder`, seeded inactive) — for each active rule:

1. computes **forecasted stock** = on-hand at the location **+ open incoming** (undelivered PO
   lines + open MOs), so a replenishment already on the way is not re-ordered;
2. if that is below the **minimum**, drafts a replenishment up to the **maximum**, rounded up
   to the multiple:
   - **buy** → a draft **purchase order** to the vendor, at the product's cost;
   - **manufacture** → a draft **manufacturing order**.

Verified: min 20 / max 100 / ×10, on-hand 15 → drafts a PO for **90** (85 rounded up); a make
rule drafts an MO for 45; and a second run does **not** re-order the buy product, because the
open PO now counts as incoming. Both documents are left **draft** for a human to confirm.

> The "buy" route names the vendor on the rule (there is no `product.supplierinfo` /
> per-vendor pricelist yet), and prices the line at the product's standard cost.

---

## 2. Putaway — `stock.putaway.rule`

A rule maps a **product** *or* a **product category**, arriving at a location, to the
sub-location it should be stored in. On receipt validation, each incoming move is redirected
to its designated sub-location before the quant is applied — a **product-specific rule wins
over a category rule**, and both must target an internal location.

Verified: with rules *PP→Shelf A*, *category→Shelf B*, *PP2→Shelf C*, a receipt lands PP at
Shelf A (product rule), PC at Shelf B (category rule), and PP2 at Shelf C (product beats
category) — nothing left at the generic WH/Stock.

The redirect happens in `stock.picking.button_validate`; with no rules configured the lookup
returns nothing and the move lands where it was headed, so it is a no-op for simple setups.

---

## 3. Barcode

Barcode fields on **product** (existing), **stock.location**, and **stock.production.lot**,
plus a **resolver** — `stock.quant.resolve_barcode` — that turns a scanned code into what it
is: `{type: product | location | lot | unknown, id, name}`. This is the data + lookup
foundation the scanning workflows sit on.

Verified: a product / location / lot barcode each resolve to the right record type and id, and
an unrecognised code resolves to `unknown`.

> The **scanning UI** (a barcode-driven receipt/delivery/inventory screen) is a front-end app
> and is not built — this delivers the backend it needs: the fields and the resolver.

---

## 4. Deliberately simpler than the reference ERP

- Reorder: vendor is named on the rule (no `product.supplierinfo` seller list / price breaks);
  forecast is on-hand + open PO/MO (no full stock-move forecast horizon); one rule per product/
  location.
- Putaway: rule matches product or its (direct) category at one arrival location — no
  storage-category capacity/removal strategies, no package-level putaway.
- Barcode: fields + resolver only; no scanning UI, no GS1/barcode-nomenclature parsing.

Each is the mechanism the reference ERP has, at a level that is configurable and testable, without the
front-end/edge surface.

---

## 5. Security & conventions

- **ACL.** `stock.warehouse.orderpoint`, `stock.putaway.rule` are behind `INVENTORY_USER` (11);
  the barcode resolver rides on `stock.quant` (also 11). Deny-by-default throughout.
- **Cron.** The reorder job is bound by code (`IrCron::registerJob`) and its row is seeded
  **inactive** — it never runs until an admin enables it, and it is idempotent-friendly (an
  open replenishment suppresses a duplicate).
- **Money discipline & bound SQL** as before: min/max/multiple are BIGINT micro-units; all
  values `$N`-bound.

---

## 6. Verification

```
verify_reorder.sh   buy route drafts a PO (rounded to the multiple); manufacture drafts an MO;
                    an open replenishment counts as incoming, so no double-order
verify_putaway.sh   product rule, category rule, and product-beats-category all route a
                    receipt to the right sub-location
verify_barcode.sh   product/location/lot codes resolve to the right record; unknown otherwise

full suite          ./scripts/run_tests.sh — all green; the putaway hook on button_validate is
                    a no-op without rules, so existing receipt/delivery flows are unchanged
```

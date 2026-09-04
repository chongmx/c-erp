# 076 — Sales UI polish + `ir_act_window` id-collision fix

## Context

Reviewing the Sales section ("how a user creates a sales order → invoice → delivery →
down payment") surfaced two things: the sales **entry point was broken**, and the
down-payment control was an unpolished inline bar. Both are fixed here. The dark theme is
kept — this is a polish pass, not a re-skin.

## 1. The real bug: menus opened the wrong model

`ir_act_window` ids are hardcoded per module and **overlapped**. `ProductModule` seeds ids
9–15 (Products, Vendor Pricelists, Footprints, Part Units, Parametric Search) with
`ON CONFLICT (id) DO UPDATE`, so it **wins** every collision. Sale/Purchase/HR hardcoded
the same ids with `ON CONFLICT DO NOTHING`, so their actions were silently clobbered:

| Menu | Wanted | Resolved to (before) |
|------|--------|----------------------|
| Sales Orders (menu 62 → act 11) | `sale.order` | `product.supplierinfo` (Vendor Pricelists) |
| Purchase Orders (menu 72 → act 12) | `purchase.order` | `part.footprint` (Footprints) |
| Employees (menu 81 → act 13) | `hr.employee` | `part.unit` (Part Units) |
| Job Positions (menu 84 → act 15) | `hr.job` | `part.search` (Parametric Search) |

So clicking **Sales → Orders → Sales Orders** opened a product-supplier list with the wrong
columns and no records — there was no working way to reach the sale.order list/form at all.
(HR's Departments/Working Schedules survived only because Product doesn't claim ids 14/16.)

### Fix

Move the four clobbered actions to unique, free ids and point their menus at them:

- Sales Orders → **48**, Purchase Orders → **49**, Employees → **50**, Job Positions → **51**.
- Both the `ir_act_window` and the referencing `ir_ui_menu` now use
  `ON CONFLICT (id) DO UPDATE`, so **existing databases self-heal on the next startup**
  (the stale `menu.action_id` is rewritten). Product/part keep ids 11–15 unchanged.

Files: `modules/sale/SaleModule.cpp`, `modules/purchase/PurchaseModule.cpp`,
`modules/hr/HrModule.cpp`.

Verified on the live dev DB after restart:

```
menu 62 Sales Orders    → act 48 sale.order
menu 72 Purchase Orders → act 49 purchase.order
menu 81 Employees       → act 50 hr.employee
menu 84 Job Positions   → act 51 hr.job
act 11/12/13/15 still = Vendor Pricelists / Footprints / Part Units / Parametric Search
```

> **Latent risk noted:** id allocation across modules is still manual and uncoordinated.
> A follow-up could give each module a reserved block (or a name-keyed upsert) so new
> actions can't re-collide. Not done here to keep the change surgical.

## 2. Down-payment / advance-bill modal

The Sales down-payment and Purchase advance-bill shared one inline `.dp-form-bar` (a green
gradient strip). It is now a **centred modal wizard** — title, amount + pill presets
(30/50/100%) on one row, full-width description, right-aligned Create/Cancel — over a dimmed
backdrop. Implemented as a **pure-CSS** change to `.dp-form-bar` (a `0 0 0 100vmax` spread
box-shadow is the backdrop), so both forms upgrade with **no template/handler change**.

## 3. Order-lines polish

Subtle row-hover highlight on `.so-lines-table tbody tr` for editing clarity.

## Regression

Unit: 3 cases / 120 assertions pass. Integration: initially 40 pass / 7 fail — none of
the 7 touched menus, actions, or CSS; they were the non-hermetic baseline from docs/070
(tests assuming hand-seeded data). Those have since been made hermetic — see **docs/077** —
so the suite is now **47 passed, 0 failed**, green on two consecutive runs.
`verify_supplierinfo` and `verify_order_totals` pass, confirming the product/part actions
and sale totals are intact.

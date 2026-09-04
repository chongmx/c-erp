# 069 — Vendor pricelists, PartKeepr PK2–PK4, analytic accounting, bank reconciliation (+ UI)

**Date:** 2026-08-08
**Status:** ✅ Complete and verified (5 new backend suites, all green; full suite green)
**Closes:** the remaining feature gaps from `068` §2.2 — `product.supplierinfo`, PartKeepr
PK2–PK4, analytic accounting, and bank reconciliation — each with a UI.

---

## 0. What this adds

| Feature | Backend | UI |
|---|---|---|
| `product.supplierinfo` | vendor pricelists; reorder picks vendor+price from them | Vendor Pricelists menu |
| PK2 footprints | `part.footprint` + `product.footprint_id` | Footprints menu; field on product |
| PK3 parameters + units | `part.parameter`, `part.unit`, `search_parts` | **Parametric Search** screen; Part Units menu |
| PK4 manufacturer parts | `part.manufacturer.info` | list (product-scoped) |
| Analytic accounting | `account.analytic.account/.line`; auto lines on post | Analytic Accounts / Items menus |
| Bank reconciliation | `account.bank.statement/.line`; `reconcile`, `suggest_matches` | **Bank Reconciliation** screen |
| Barcode | (resolver from `067`) | **Barcode** scan screen |

---

## 1. `product.supplierinfo` — and the reorder gap closed

A vendor pricelist line (`product.supplierinfo`): which vendor sells a product, at what price,
MOQ and lead time. The **reorder "buy" route now sources the vendor and price from it** — with no
vendor named on the rule, the scheduler drafts the PO to the product's first vendor at that
vendor's listed price (not `standard_price`). This closes the finding in `068`/`067`. It also
turns `068`'s remaining dead ACL entry (`product.supplierinfo`) into a real model.

Verified (`verify_supplierinfo.sh`): a line reads back; a reorder with no vendor named drafts a PO
to the supplierinfo vendor at price 12 (the listed price), not the standard cost 8.

## 2. PartKeepr PK2–PK4 — the parts catalogue

- **PK2 Footprints** — `part.footprint` (SOIC-8, 0805, …) + `product.footprint_id`.
- **PK3 Parametric parameters + SI units** — `part.parameter` (a product's named numeric spec) and
  `part.unit` (Ohm/Ω, Farad/F). Parameter values are stored as `NUMERIC(24,9)`, **not**
  micro-fixed-point, so the scientific range (pico-farads to mega-ohms) is preserved. The
  differentiator is **`search_parts(name, min, max)`** — find parts whose parameter is in a range.
- **PK4 Manufacturer part numbers** — `part.manufacturer.info` (manufacturer + MPN + notes).

Verified (`verify_partkeepr.sh`): footprint on a product; a Resistance parameter with a unit;
`search_parts` for Resistance in [1000, 5000] returns the 4700 part and excludes 100 and 10000;
an MPN reads back.

## 3. Analytic accounting — cost centres

`account.analytic.account` (a cost centre, with a computed balance) and `account.analytic.line`
(its entries). A journal item carries an `analytic_account_id`; on **post**, every tagged item
generates an analytic line — amount = `credit − debit` (revenue +, cost −), idempotently (a
re-post never duplicates).

Verified (`verify_analytic.sh`): a 300 cost tagged to "Project X" posts a −300 analytic line, the
account balance reads −300, and untagged postings generate nothing.

## 4. Bank reconciliation

`account.bank.statement` + `.line`. Two methods drive it:
- **`suggest_matches(line)`** — open invoices/bills that could clear the line (equal residual, then
  same partner, first).
- **`reconcile(line, invoice)`** — posts the bank entry (**Dr Bank / Cr Receivable** on an inflow),
  pays down the invoice residual (→ `paid`), and marks the line reconciled.

Verified (`verify_bank_recon.sh`): a statement line of 100 suggests the open invoice; reconciling
posts Dr Bank 100 / Cr AR 100, sets the invoice to paid (residual 0), and flags the line.

> Fix found while testing: `BaseModel::create` inserts every registered stored column, so a
> `date` field left empty was inserted as NULL and hit the NOT-NULL constraint (the DB DEFAULT
> only applies when the column is omitted). Models with a required date now default it to today in
> their constructor.

---

## 5. UI

Three **custom full-screen OWL components** (loaded before `app.js`, registered in `CUSTOM_VIEWS`
against pseudo-models, styled by `partkeepr.css`), each driving a tested backend method:

- **Barcode** (`barcode.scan` → `BarcodeScan`) — a focus-holding scan input that resolves each code
  to product / location / lot via `resolve_barcode`, with a recent-scan log (hardware scanners that
  type + Enter drive it hands-free).
- **Parametric Search** (`part.search` → `PartSearch`) — parameter name (autocompleted) + min/max,
  results link to the product form.
- **Bank Reconciliation** (`bank.reconcile` → `BankReconcile`) — pick a statement; each unreconciled
  line shows its suggested matches; one click reconciles and the line drops off.

The catalogue/config models (vendor pricelists, footprints, part units, analytic accounts, analytic
items, bank statements) are exposed through **generic list/form menus** — the standard way to
manage them.

> **UI verification is limited to what tooling allows here**: the component JS is delimiter-balanced
> and loaded by `index.html`; the `CUSTOM_VIEWS` entries and menus/actions exist; and every backend
> method the screens call is covered by an integration test. There is no headless browser in this
> environment, so the rendered screens were not click-tested. Deep product-form tab integration
> (embedding vendor/spec/MPN lists inside the product form, and wiring its stale "On Hand — coming
> soon" placeholders) is a follow-up on the 9.5k-line single-file frontend.

---

## 6. Security & conventions

- **ACL.** New models gated deny-by-default: `product.supplierinfo` + the `part.*` catalogue behind
  the product-adjacent allowlist (internal users); `account.analytic.*` and `account.bank.statement*`
  behind `ACCOUNT_BILLING` (5). The pseudo-model screens render via `ir.actions.act_window` (already
  allowlisted) and call real, ACL-checked models.
- **S-49.** Custom `search_read`s filter by a single parsed integer (`product_id` / `statement_id` /
  `account_id`) or `search_parts`' bound name — no raw user domain compiled without an allowlist.
- **Bound SQL, money discipline** as before. `search_parts` binds the parameter name and the numeric
  bounds; parameter values are NUMERIC (scientific range), everything monetary stays micro-units.

---

## 7. Verification

```
verify_supplierinfo.sh   vendor line; reorder auto-picks vendor + price from supplierinfo
verify_partkeepr.sh      footprint on product; parameter + unit; parametric range search; MPN
verify_analytic.sh       tagged item posts an analytic line; balance; no noise when untagged
verify_bank_recon.sh     suggest_matches offers the invoice; reconcile posts Dr Bank/Cr AR + pays it
(verify_barcode.sh)      the resolver the scan screen uses (from 067)

full suite               ./scripts/run_tests.sh — all green
```

# Transfer Form (stock.picking) — Deferred / Stub Items

Items visible in the Transfer detail page that are currently hardcoded,
stubbed, or not yet implemented. All are candidates for future phases.

---

## 1. Additional Info tab — Responsible field

**Location:** Additional Info tab, left column
**Current state:** Label shows "Responsible", value is `— (not implemented)`
**What it should be:** A Many2one → `res.users` dropdown showing the user
responsible for this transfer (maps to `stock.picking.user_id`).

**Work needed:**
- Add `user_id INTEGER REFERENCES res_users(id)` column to `stock_picking`
  via `ALTER TABLE … ADD COLUMN IF NOT EXISTS` migration
- Expose it in `StockPicking::registerFields()` and `serializeFields()`
- Add it to `StockPickingFormView::fields()`
- In `TransferFormView`: load users list (`res.users` `search_read`) and
  render as a `<select>` similar to the Delivery Address dropdown

---

## 2. Additional Info tab — Company field

**Location:** Additional Info tab, left column
**Current state:** Hardcoded string `"My Company"` — not read from DB
**What it should be:** The actual company name from `stock_picking.company_id`
→ `res_company.name`

**Work needed:**
- `company_id` is already stored on `stock_picking`; just resolve the name
  in `TransferFormView.load()` (same JOIN pattern used in `handleSearchRead`)
  and display it as a read-only span

---

## 3. Additional Info tab — Lot/Serial tracking

**Location:** Additional Info tab, right column
**Current state:** `— (not implemented)`
**What it should be:** Indicates whether the product on each move is tracked
by lot or serial number. In Odoo this comes from
`product.product.tracking` (`none | lot | serial`).

**Work needed (full lot/serial support):**
- Add `stock_lot` table: `id, name, product_id, company_id`
- Add `stock_move_line` table: `id, move_id, lot_id, qty_done, …`
- Show lot input on each move line when state = `assigned` and
  product tracking ≠ `none`
- `button_validate` must check lot numbers are filled before proceeding

**This is a significant sub-feature — suggest its own phase (17g or 27).**

---

## 4. Additional Info tab — Package tracking

**Location:** Additional Info tab, right column
**Current state:** `— (not implemented)`
**What it should be:** Package (quant package) tracking — group move lines
into physical boxes/pallets. Maps to `stock.quant.package`.

**Work needed:** Requires `stock_quant_package` table and
`stock_move_line.result_package_id`. The "Put in Pack" button on the
Operations tab is the entry point.

**This is a significant sub-feature — suggest its own phase (28).**

---

## 5. Operations tab — "Put in Pack" button

**Location:** Below the Operations move lines table
**Current state:** Button is visible but disabled (`so-stat-btn-disabled`)
with tooltip "Put in Pack — coming soon"
**What it should be:** Groups selected move lines into a `stock.quant.package`
(physical box/pallet). Only relevant when package tracking is enabled.

**Depends on:** Item 4 (Package tracking) above.

---

## 6. Chatter / Log panel

**Location:** Below the card, visible in all states
**Current state:** Hardcoded single entry — `System: "Transfer created"`.
Always shows regardless of actual history.
**What it should be:** A real audit trail with timestamped entries for each
state transition (Confirmed, Availability Checked, Validated, Cancelled,
Reset to Draft) and any user notes/messages.

**Work needed:**
- Add `mail_message` table (or a simpler `stock_picking_log` table):
  `id, picking_id, author_id, body, date, subtype (activity|note|state_change)`
- `StockPickingViewModel` workflow methods write a log entry on each
  state transition
- `TransferFormView.load()` fetches log entries and renders them in the
  chatter feed with real author, timestamp, and message

**Suggest as part of a broader `mail.message` / chatter infrastructure
phase (Phase 27 or cross-cutting).**

---

## 7. Source Location / Destination — read-only, not editable

**Location:** Main info grid, left column
**Current state:** Displayed as read-only `<span>` — cannot be changed
**What it should be:** In Odoo, draft transfers allow changing the source
and destination locations via Many2one dropdowns.

**Work needed:**
- When `isDraft`: render `<select>` dropdowns for `location_id` and
  `location_dest_id` (load all active locations)
- Include the selected value in `onSave()` write call

---

## Summary Table

| Item | Effort | Suggests Phase |
|------|--------|---------------|
| Responsible (user_id) | Small | can do with Phase 18 or standalone |
| Company name (read from DB) | Tiny | fix anytime |
| Lot/Serial tracking | Large | Phase 27 |
| Package tracking | Large | Phase 28 |
| Put in Pack button | Large | Phase 28 (depends on packages) |
| Chatter / audit log | Medium | Phase 27 (mail infrastructure) |
| Editable src/dest locations | Small | can do with Phase 18 or standalone |

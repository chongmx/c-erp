# Account Module Progress

## Summary
Implemented Phase 6 — double-entry bookkeeping module. Covers 7 models, a generic CRTP ViewModel template, 14 views, and IR menu wiring. `action_post` generates balanced journal entries; `AccountPaymentViewModel::action_post` creates the corresponding `account.move` with two offsetting lines.

---

## Build Status
- Build: ✅ clean (`[100%] Built target c-erp`)

---

## New Files

| File | Purpose |
|------|---------|
| `modules/account/AccountModule.hpp` | All 7 models, ViewModels, module class (~1 250 lines) |
| `modules/account/AccountViews.hpp` | 14 view classes (list + form × 7 models) |
| `docs/account-module-plan.md` | Design plan written before implementation |

---

## Models

| Model | C++ Class | Table | Key fields |
|-------|-----------|-------|------------|
| `account.account` | `AccountAccount` | `account_account` | code, name, account_type, currency_id |
| `account.journal` | `AccountJournal` | `account_journal` | name, code, type (SAL/PUR/BNK/CSH), currency_id, company_id |
| `account.tax` | `AccountTax` | `account_tax` | name, amount, amount_type, type_tax_use |
| `account.move` | `AccountMove` | `account_move` | name, date, journal_id, partner_id, state (draft/posted/cancel), move_type |
| `account.move.line` | `AccountMoveLine` | `account_move_line` | move_id, account_id, debit, credit, **balance** (generated column) |
| `account.payment` | `AccountPayment` | `account_payment` | partner_id, amount, currency_id, journal_id, state |
| `account.payment.term` | `AccountPaymentTerm` | `account_payment_term` | name, note |

### Notable: generated column
`account_move_line.balance` is `GENERATED ALWAYS AS (debit - credit) STORED`. It is **not** registered in `fieldRegistry_` and must never appear in INSERT.

---

## ViewModels

### `AccountViewModel<TModel>` (CRTP template)
Generic ViewModel providing: `search_read`, `read`, `create`, `write`, `unlink`, `fields_get`, `search_count`, `search`. Later extracted to `modules/base/GenericViewModel.hpp`.

### `AccountMoveViewModel` (extends template)
Extra methods:
- `action_post` — validates debit == credit for entries, assigns name `JCODE/YEAR/NNNN`, sets state='posted'
- `button_cancel` — sets state='cancel' (draft only)
- `action_reverse` — alias for button_cancel (simplified)

### `AccountPaymentViewModel` (extends template)
Extra methods:
- `action_post` — creates `account_move` + 2 balanced `account_move_line` rows (DR receivable / CR journal account), sets state='posted'
- `action_cancel` — sets state='cancelled'

---

## Schema

```sql
account_account      — chart of accounts
account_journal      — SAL/PUR/BNK/CSH journals
account_tax          — sales/purchase taxes
account_move         — journal entries / invoices
account_move_line    — individual debit/credit lines
account_payment      — customer/vendor payments
account_payment_term — net-30, immediate, etc.
```

All DDL in `AccountModule::ensureSchema_()` via `CREATE TABLE IF NOT EXISTS`.

---

## Seeds

| Table | Rows | Notes |
|-------|------|-------|
| `account_account` | 9 | COA: 1100 Cash, 1200 AR, 2000 AP, 3000 Equity, 4000 Sales, 5000 COGS, 6000 Expenses, 7000 Other Income, 8000 Tax Payable |
| `account_journal` | 4 | SAL (Sales), PUR (Purchase), BNK (Bank), CSH (Cash) |
| `account_tax` | 2 | VAT 10% (sale), VAT 10% (purchase) |
| `account_payment_term` | 2 | Immediate, Net 30 |
| `ir_act_window` | 4 | IDs 4–7: accounts, journals, moves, payments |
| `ir_ui_menu` | 7 | IDs 11–17: hierarchical entries under Accounting app (id=10) |

---

## IR Menu Hierarchy (after UI redesign)

```
10  Accounting (app tile)
  11  Journal Entries   → action 6
  12  Customers         (section)
    15  Payments        → action 7
  13  Vendors           (section, TBD)
  14  Configuration     (section)
    16  Chart of Accounts → action 4
    17  Journals          → action 5
```

---

## Bugs Fixed During Implementation

| Bug | Fix |
|-----|-----|
| `IrModule::seedActions_()` COUNT guard blocked account seeds | Removed COUNT guards; use `ON CONFLICT (id) DO NOTHING` only |
| `ir_act_window_id_seq` hardcoded after IrModule seeds | Changed to `setval('seq', (SELECT MAX(id) FROM table), true)` |
| `account_move_line.balance` as generated column in INSERT | Not registered in `fieldRegistry_` — excluded from `storedColumnNames()` |
| Date field NOT NULL on create with empty string | `serializeFields` defaults to `currentDate_()` when `date` is empty |
| Many2one `[id,"Name"]` rejected by `validate()` | Added `m2oToId_()` helper in `deserializeFields()` |

---

## Module & Table Inventory (cumulative after Phase 6)

| Module | Tables |
|--------|--------|
| base | `res_lang`, `res_currency`, `res_country`, `res_country_state`, `res_partner` |
| auth | `res_company`, `res_groups`, `res_users`, `res_groups_users_rel` |
| ir | `ir_act_window`, `ir_ui_menu` |
| account | `account_account`, `account_journal`, `account_tax`, `account_move`, `account_move_line`, `account_payment`, `account_payment_term` |

---

## Next Steps
Per `next-phases-plan.md` Phase 7: UOM module (`uom.uom`), which unblocks the Product module.

# Phase 6 — Account Module Plan

Reference: `zzref/odoo/addons/account/models/`
Target: `modules/account/AccountModule.hpp` + views in `AuthViews`-style file

---

## Current State Summary

| Phase | Status | What was built |
|-------|--------|---------------|
| Infrastructure | ✅ | Container, DI, ORM (BaseModel CRTP), JsonRpcDispatcher, SessionManager |
| Base (3) | ✅ | res_lang, res_currency, res_country, res_country_state, res_partner (extended) |
| Auth (4) | ✅ | res_users, res_groups, res_company, PBKDF2 auth, session cookies |
| IR (5) | ✅ | ir_ui_menu, ir_act_window, ir_model (in-memory), OWL list/form frontend |
| Session hardening | ✅ | get_session_info returns full Odoo-compatible shape |

**Bug fixes applied this session:**
- Pure-virtual crash in BaseModel CRTP constructor → fixed with `TDerived::registerFields()`
- `view->fields().items()` dangling reference → fixed with named local variable
- `false` / `[id,"Name"]` M2o params sent to Postgres INTEGER → fixed with `normalizeForDb_()`
- Missing `search_read/create/write/unlink` on res.users, res.company VMs
- PartnerViewModel wrapping records in `{arch,fields,record}` → removed, returns plain records
- Missing UsersListView / CompanyListView → added

---

## What to Build: Minimal Viable Account Module

Odoo's account module is 52 models / ~300K lines. We port only what's needed for
basic double-entry bookkeeping + invoice/payment UI. No: reconciliation engine,
tax repartition lines, analytic distribution, multi-currency moves, bank sync.

### Models in dependency order

| Step | C++ Class | Odoo model | Table | Key simplification |
|------|-----------|------------|-------|--------------------|
| 6a | `AccountAccount` | account.account | account_account | Drop: code_mapping, placeholder_code, tax_ids M2M |
| 6b | `AccountJournal` | account.journal | account_journal | Drop: payment methods, bank account, statement source |
| 6c | `AccountTax` | account.tax | account_tax | Drop: repartition lines, tax groups, children |
| 6d | `AccountMove` | account.move | account_move | Drop: auto_post, locking, portal mixin, mail thread |
| 6e | `AccountMoveLine` | account.move.line | account_move_line | Drop: full reconciliation, analytic, product |
| 6f | `AccountPayment` | account.payment | account_payment | Drop: partner bank, payment method, SEPA |
| 6g | `AccountPaymentTerm` | account.payment.term | account_payment_term | Drop: lines table (store as JSON) |

---

## Schema

### 6a — account_account
```sql
CREATE TABLE IF NOT EXISTS account_account (
    id           SERIAL PRIMARY KEY,
    name         VARCHAR NOT NULL,
    code         VARCHAR NOT NULL,
    account_type VARCHAR NOT NULL DEFAULT 'asset_current',
    -- asset_receivable | asset_cash | asset_current | asset_non_current |
    -- asset_prepayments | asset_fixed | liability_payable | liability_current |
    -- liability_non_current | equity | equity_unaffected | income | income_other |
    -- expense | expense_other | expense_depreciation | off_balance
    internal_group VARCHAR NOT NULL DEFAULT 'asset',
    -- asset | liability | equity | income | expense | off
    currency_id  INTEGER REFERENCES res_currency(id),
    company_id   INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    reconcile    BOOLEAN NOT NULL DEFAULT FALSE,
    active       BOOLEAN NOT NULL DEFAULT TRUE,
    note         TEXT,
    create_date  TIMESTAMP DEFAULT now(),
    write_date   TIMESTAMP DEFAULT now(),
    UNIQUE (code, company_id)
)
```
Seeds (minimal chart): 9 accounts covering the major types.

### 6b — account_journal
```sql
CREATE TABLE IF NOT EXISTS account_journal (
    id                  SERIAL PRIMARY KEY,
    name                VARCHAR NOT NULL,
    code                VARCHAR(10) NOT NULL,
    type                VARCHAR NOT NULL DEFAULT 'general',
    -- sale | purchase | bank | cash | general
    currency_id         INTEGER REFERENCES res_currency(id),
    company_id          INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    default_account_id  INTEGER REFERENCES account_account(id),
    sequence            INTEGER NOT NULL DEFAULT 10,
    active              BOOLEAN NOT NULL DEFAULT TRUE,
    create_date         TIMESTAMP DEFAULT now(),
    write_date          TIMESTAMP DEFAULT now(),
    UNIQUE (code, company_id)
)
```
Seeds: Sales (SAL), Purchases (PUR), Bank (BNK), Cash (CSH).

### 6c — account_tax
```sql
CREATE TABLE IF NOT EXISTS account_tax (
    id            SERIAL PRIMARY KEY,
    name          VARCHAR NOT NULL,
    amount        NUMERIC(16,4) NOT NULL DEFAULT 0,
    amount_type   VARCHAR NOT NULL DEFAULT 'percent',
    -- percent | fixed | division
    type_tax_use  VARCHAR NOT NULL DEFAULT 'sale',
    -- sale | purchase | none
    price_include BOOLEAN NOT NULL DEFAULT FALSE,
    company_id    INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    active        BOOLEAN NOT NULL DEFAULT TRUE,
    description   VARCHAR,
    create_date   TIMESTAMP DEFAULT now(),
    write_date    TIMESTAMP DEFAULT now()
)
```
Seeds: 15% Sales Tax, 15% Purchase Tax.

### 6d — account_move
```sql
CREATE TABLE IF NOT EXISTS account_move (
    id              SERIAL PRIMARY KEY,
    name            VARCHAR NOT NULL DEFAULT '/',
    ref             VARCHAR,
    narration       TEXT,
    move_type       VARCHAR NOT NULL DEFAULT 'entry',
    -- entry | out_invoice | out_refund | in_invoice | in_refund
    state           VARCHAR NOT NULL DEFAULT 'draft',
    -- draft | posted | cancel
    date            DATE NOT NULL DEFAULT CURRENT_DATE,
    invoice_date    DATE,
    due_date        DATE,
    journal_id      INTEGER NOT NULL REFERENCES account_journal(id),
    partner_id      INTEGER REFERENCES res_partner(id),
    company_id      INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    currency_id     INTEGER REFERENCES res_currency(id),
    payment_state   VARCHAR NOT NULL DEFAULT 'not_paid',
    -- not_paid | in_payment | paid | partial | reversed | cancelled
    amount_untaxed  NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_tax      NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_total    NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_residual NUMERIC(16,2) NOT NULL DEFAULT 0,
    create_date     TIMESTAMP DEFAULT now(),
    write_date      TIMESTAMP DEFAULT now()
)
```

### 6e — account_move_line
```sql
CREATE TABLE IF NOT EXISTS account_move_line (
    id          SERIAL PRIMARY KEY,
    move_id     INTEGER NOT NULL REFERENCES account_move(id) ON DELETE CASCADE,
    account_id  INTEGER NOT NULL REFERENCES account_account(id),
    journal_id  INTEGER REFERENCES account_journal(id),
    company_id  INTEGER REFERENCES res_company(id),
    date        DATE,
    name        VARCHAR,
    ref         VARCHAR,
    partner_id  INTEGER REFERENCES res_partner(id),
    debit       NUMERIC(16,2) NOT NULL DEFAULT 0,
    credit      NUMERIC(16,2) NOT NULL DEFAULT 0,
    balance     NUMERIC(16,2) GENERATED ALWAYS AS (debit - credit) STORED,
    amount_currency  NUMERIC(16,2) NOT NULL DEFAULT 0,
    quantity    NUMERIC(16,4) NOT NULL DEFAULT 1,
    tax_line_id INTEGER REFERENCES account_tax(id),
    reconciled  BOOLEAN NOT NULL DEFAULT FALSE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
)
```

### 6f — account_payment
```sql
CREATE TABLE IF NOT EXISTS account_payment (
    id                SERIAL PRIMARY KEY,
    name              VARCHAR NOT NULL DEFAULT '/',
    date              DATE NOT NULL DEFAULT CURRENT_DATE,
    journal_id        INTEGER NOT NULL REFERENCES account_journal(id),
    partner_id        INTEGER REFERENCES res_partner(id),
    company_id        INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    currency_id       INTEGER REFERENCES res_currency(id),
    amount            NUMERIC(16,2) NOT NULL DEFAULT 0,
    payment_type      VARCHAR NOT NULL DEFAULT 'inbound',
    -- inbound | outbound
    partner_type      VARCHAR NOT NULL DEFAULT 'customer',
    -- customer | supplier
    state             VARCHAR NOT NULL DEFAULT 'draft',
    -- draft | posted | cancelled
    move_id           INTEGER REFERENCES account_move(id),
    memo              VARCHAR,
    create_date       TIMESTAMP DEFAULT now(),
    write_date        TIMESTAMP DEFAULT now()
)
```

### 6g — account_payment_term
```sql
CREATE TABLE IF NOT EXISTS account_payment_term (
    id         SERIAL PRIMARY KEY,
    name       VARCHAR NOT NULL,
    note       TEXT,
    -- line_ids stored as JSON: [{"days":30,"value":"percent","value_amount":100}]
    lines_json TEXT NOT NULL DEFAULT '[{"days":0,"value":"balance","value_amount":0}]',
    active     BOOLEAN NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
)
```
Seeds: "Immediate Payment" (0 days), "30 Days" (30 days).

---

## C++ Model Simplifications vs Odoo

### account.move — state machine (simplified)
```
draft → [action_post()] → posted → [button_cancel()] → cancel
```
- On post: validate lines balance (debit == credit for type='entry'), assign sequence name
- Name format: `{JOURNAL_CODE}/{YEAR}/{4-digit-seq}` e.g. `SAL/2026/0001`
- amount_total = sum of credit lines (for invoices) or sum of debit lines (for bills)
- No: auto-post, locking periods, tax computation on save, mail thread

### account.payment — simplified
```
draft → [action_post()] → posted → [action_cancel()] → cancelled
```
- On post: create corresponding account_move + 2 account_move_lines
  - Debit: partner receivable/payable account
  - Credit: journal's default account (bank/cash)
- No: SEPA, bank reconciliation, payment batches

### Computed fields (server-side only, not stored computed)
- `amount_total` on move: recompute from lines when lines change (in `write()`)
- `balance` on move_line: stored GENERATED column in Postgres (debit - credit)
- `internal_group` on account: derived from `account_type` (mapping)

---

## ViewModels Needed

| ViewModel class | Model | Methods |
|-----------------|-------|---------|
| `AccountViewModel<T>` | generic CRTP template | search_read, read, create, write, unlink, fields_get, search_count |
| `AccountMoveViewModel` | account.move | + `action_post`, `button_cancel`, `action_reverse` |
| `AccountPaymentViewModel` | account.payment | + `action_post`, `action_cancel` |

Use `AccountViewModel<T>` template (like `LookupViewModel<T>` in BaseModule) for
account.account, account.journal, account.tax, account.payment.term.

---

## Views Needed

### List views (columns to show)
| Model | List columns |
|-------|-------------|
| account.account | code, name, account_type, active |
| account.journal | code, name, type, currency_id |
| account.tax | name, amount, amount_type, type_tax_use |
| account.move | name, date, partner_id, amount_total, state, move_type |
| account.payment | name, date, partner_id, amount, payment_type, state |
| account.payment.term | name |

### Form views (fields to edit)
| Model | Form fields |
|-------|------------|
| account.account | code, name, account_type, currency_id, reconcile, active, note |
| account.journal | name, code, type, currency_id, default_account_id, sequence, active |
| account.tax | name, amount, amount_type, type_tax_use, price_include, description, active |
| account.move | date, move_type, journal_id, partner_id, ref, narration, state (read-only) |
| account.payment | date, payment_type, partner_type, journal_id, partner_id, amount, memo |
| account.payment.term | name, note |

---

## Menu Additions (ir_act_window + ir_ui_menu seeds)

Add to IrModule seeds:

| id | name | res_model | path |
|----|------|-----------|------|
| 4 | Chart of Accounts | account.account | accounts |
| 5 | Journals | account.journal | journals |
| 6 | Journal Entries | account.move | moves |
| 7 | Payments | account.payment | payments |

Menu items: ids 4-7 pointing at actions 4-7.

---

## File Layout

```
modules/account/
    AccountModule.hpp     — module class + all models + viewmodels
    AccountViews.hpp      — list + form view classes for all 7 models
```

Single-file-per-module pattern (same as IrModule).

---

## Implementation Order

1. `AccountModule.hpp`:
   - All 7 model classes (AccountAccount through AccountPaymentTerm)
   - `AccountViewModel<T>` CRTP template viewmodel
   - `AccountMoveViewModel` with action_post / button_cancel
   - `AccountPaymentViewModel` with action_post (creates move + lines)
   - `AccountModule` class with ensureSchema_(), seeds, registerAll()

2. `AccountViews.hpp`:
   - 7 list views + 7 form views (14 classes, same pattern as AuthViews)

3. Wire into `main.cpp`:
   - `#include "modules/account/AccountModule.hpp"`
   - `g_container->addModule<odoo::modules::account::AccountModule>();`

4. Extend IrModule seeds to add 4 new menu items + actions (idempotent).

---

## Seed Chart of Accounts (minimal)

| code | name | account_type | reconcile |
|------|------|-------------|-----------|
| 1000 | Cash | asset_cash | false |
| 1100 | Bank | asset_cash | false |
| 1200 | Accounts Receivable | asset_receivable | true |
| 2000 | Accounts Payable | liability_payable | true |
| 3000 | Share Capital | equity | false |
| 4000 | Sales Revenue | income | false |
| 5000 | Cost of Goods Sold | expense | false |
| 6000 | Operating Expenses | expense | false |
| 9999 | Undistributed Profit | equity_unaffected | false |

---

## Out of Scope (not in this phase)

- Tax computation on move lines (user enters debit/credit directly)
- Full reconciliation engine (tracked but not matched)
- Multi-currency moves (all in company currency)
- Bank statement import / sync
- Account.analytic, account.budget, account.asset
- Report generation (trial balance, P&L, balance sheet)
- Payment batches / SEPA files
- Locking accounting periods
- `account.full.reconcile` / `account.partial.reconcile`

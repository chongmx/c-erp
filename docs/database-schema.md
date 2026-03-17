# Database Schema — Implemented Tables

All DDL is idempotent (`CREATE TABLE IF NOT EXISTS`, `ALTER TABLE … ADD COLUMN IF NOT EXISTS`).
Safe to run on every boot.

**Creation source:** each table's `ensureSchema_()` is called from its module's `initialize()`.

---

## Module: base  (`modules/base/BaseModule.hpp`)

### res_lang

```sql
CREATE TABLE IF NOT EXISTS res_lang (
    id          SERIAL      PRIMARY KEY,
    name        VARCHAR     NOT NULL,
    code        VARCHAR     NOT NULL UNIQUE,   -- 'en_US', 'fr_FR'
    iso_code    VARCHAR,                       -- 'en', 'fr'
    url_code    VARCHAR     NOT NULL DEFAULT '',
    active      BOOLEAN     NOT NULL DEFAULT TRUE,
    direction   VARCHAR     NOT NULL DEFAULT 'ltr',  -- 'ltr' | 'rtl'
    date_format VARCHAR     NOT NULL DEFAULT '%m/%d/%Y',
    time_format VARCHAR     NOT NULL DEFAULT '%H:%M:%S',
    create_date TIMESTAMP   DEFAULT now(),
    write_date  TIMESTAMP   DEFAULT now()
);
```

**Seeds (idempotent):**

| id | name    | code  | iso_code | url_code | active | direction |
|----|---------|-------|----------|----------|--------|-----------|
| 1  | English | en_US | en       | en       | true   | ltr       |

---

### res_currency

```sql
CREATE TABLE IF NOT EXISTS res_currency (
    id             SERIAL        PRIMARY KEY,
    name           VARCHAR(3)    NOT NULL UNIQUE,   -- ISO 4217: 'USD', 'EUR'
    symbol         VARCHAR       NOT NULL,
    position       VARCHAR       NOT NULL DEFAULT 'after',  -- 'before' | 'after'
    rounding       NUMERIC(12,6) NOT NULL DEFAULT 0.01,
    decimal_places INTEGER       NOT NULL DEFAULT 2,
    active         BOOLEAN       NOT NULL DEFAULT TRUE,
    create_date    TIMESTAMP     DEFAULT now(),
    write_date     TIMESTAMP     DEFAULT now()
);
```

**Seeds (idempotent):**

| id | name | symbol | position | rounding | decimal_places | active |
|----|------|--------|----------|----------|----------------|--------|
| 1  | USD  | $      | before   | 0.01     | 2              | true   |
| 2  | EUR  | €      | after    | 0.01     | 2              | true   |
| 3  | GBP  | £      | before   | 0.01     | 2              | true   |
| 4  | JPY  | ¥      | before   | 1.00     | 0              | true   |
| 5  | CNY  | ¥      | after    | 0.01     | 2              | true   |

---

### res_country

```sql
CREATE TABLE IF NOT EXISTS res_country (
    id          SERIAL     PRIMARY KEY,
    name        VARCHAR    NOT NULL,
    code        VARCHAR(2) NOT NULL UNIQUE,   -- ISO 3166-1 alpha-2
    currency_id INTEGER    REFERENCES res_currency(id) ON DELETE SET NULL,
    phone_code  INTEGER,
    create_date TIMESTAMP  DEFAULT now(),
    write_date  TIMESTAMP  DEFAULT now()
);
```

**Seeds (idempotent — 15 rows):**

| id | name           | code | currency_id | phone_code |
|----|----------------|------|-------------|------------|
| 1  | United States  | US   | 1 (USD)     | 1          |
| 2  | United Kingdom | GB   | 3 (GBP)     | 44         |
| 3  | Germany        | DE   | 2 (EUR)     | 49         |
| 4  | France         | FR   | 2 (EUR)     | 33         |
| 5  | Japan          | JP   | 4 (JPY)     | 81         |
| 6  | China          | CN   | 5 (CNY)     | 86         |
| 7  | Canada         | CA   | 1 (USD)     | 1          |
| 8  | Australia      | AU   | 1 (USD)     | 61         |
| 9  | Netherlands    | NL   | 2 (EUR)     | 31         |
| 10 | Singapore      | SG   | 1 (USD)     | 65         |
| 11 | Switzerland    | CH   | 3 (GBP)     | 41         |
| 12 | Sweden         | SE   | 2 (EUR)     | 46         |
| 13 | Spain          | ES   | 2 (EUR)     | 34         |
| 14 | Italy          | IT   | 2 (EUR)     | 39         |
| 15 | Brazil         | BR   | NULL        | 55         |

---

### res_country_state

```sql
CREATE TABLE IF NOT EXISTS res_country_state (
    id          SERIAL  PRIMARY KEY,
    country_id  INTEGER NOT NULL REFERENCES res_country(id) ON DELETE CASCADE,
    name        VARCHAR NOT NULL,
    code        VARCHAR NOT NULL,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);
```

**Seeds:** None — empty table (states added on demand).

---

### res_partner

Base table created first, then extended with address columns via `ALTER TABLE … ADD COLUMN IF NOT EXISTS`.

```sql
-- Base table
CREATE TABLE IF NOT EXISTS res_partner (
    id          SERIAL    PRIMARY KEY,
    name        VARCHAR   NOT NULL,
    email       VARCHAR,
    phone       VARCHAR,
    is_company  BOOLEAN   NOT NULL DEFAULT FALSE,
    company_id  INTEGER,                        -- self-ref, no FK constraint (added later)
    active      BOOLEAN   NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);

-- Address + localisation columns (idempotent extensions)
ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS street     VARCHAR;
ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS city       VARCHAR;
ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS zip        VARCHAR;
ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS lang       VARCHAR;
ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS country_id INTEGER REFERENCES res_country(id);
ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS state_id   INTEGER REFERENCES res_country_state(id);
```

**Full column list (after extensions):**

| column     | type     | nullable | default | notes |
|------------|----------|----------|---------|-------|
| id         | SERIAL   | NOT NULL | —       | PK |
| name       | VARCHAR  | NOT NULL | —       | required |
| email      | VARCHAR  | yes      | —       | |
| phone      | VARCHAR  | yes      | —       | |
| is_company | BOOLEAN  | NOT NULL | false   | |
| company_id | INTEGER  | yes      | —       | unvalidated FK → res_partner |
| active     | BOOLEAN  | NOT NULL | true    | |
| create_date| TIMESTAMP| yes      | now()   | |
| write_date | TIMESTAMP| yes      | now()   | |
| street     | VARCHAR  | yes      | —       | address ext |
| city       | VARCHAR  | yes      | —       | address ext |
| zip        | VARCHAR  | yes      | —       | address ext |
| lang       | VARCHAR  | yes      | —       | language code, e.g. 'en_US' |
| country_id | INTEGER  | yes      | —       | FK → res_country |
| state_id   | INTEGER  | yes      | —       | FK → res_country_state |

**Seeds:** Two rows created by `AuthModule::seedAdminUser_()` (runs once):
- Company partner: name='My Company', email='company@example.com', is_company=true
- Admin partner: name='Administrator', email='admin@example.com'

---

## Module: auth  (`modules/auth/AuthModule.hpp`)

### res_company

```sql
CREATE TABLE IF NOT EXISTS res_company (
    id          SERIAL    PRIMARY KEY,
    name        VARCHAR   NOT NULL,
    email       VARCHAR,
    phone       VARCHAR,
    website     VARCHAR,
    vat         VARCHAR,
    parent_id   INTEGER   REFERENCES res_company(id) ON DELETE SET NULL,
    active      BOOLEAN   NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);

-- Added after base module creates res_partner / res_currency
ALTER TABLE res_company ADD COLUMN IF NOT EXISTS
    partner_id  INTEGER REFERENCES res_partner(id)  ON DELETE SET NULL;
ALTER TABLE res_company ADD COLUMN IF NOT EXISTS
    currency_id INTEGER REFERENCES res_currency(id) ON DELETE SET NULL;
```

**Full column list (after extensions):**

| column     | type     | nullable | default | notes |
|------------|----------|----------|---------|-------|
| id         | SERIAL   | NOT NULL | —       | PK |
| name       | VARCHAR  | NOT NULL | —       | required |
| email      | VARCHAR  | yes      | —       | |
| phone      | VARCHAR  | yes      | —       | |
| website    | VARCHAR  | yes      | —       | |
| vat        | VARCHAR  | yes      | —       | tax ID |
| parent_id  | INTEGER  | yes      | —       | FK → res_company (branch hierarchy) |
| active     | BOOLEAN  | NOT NULL | true    | |
| create_date| TIMESTAMP| yes      | now()   | |
| write_date | TIMESTAMP| yes      | now()   | |
| partner_id | INTEGER  | yes      | —       | FK → res_partner (backing partner) |
| currency_id| INTEGER  | yes      | —       | FK → res_currency |

**Seeds (idempotent):**

| id | name       | partner_id       | currency_id |
|----|------------|------------------|-------------|
| 1  | My Company | (company partner) | 1 (USD)     |

---

### res_groups

```sql
CREATE TABLE IF NOT EXISTS res_groups (
    id          SERIAL  PRIMARY KEY,
    name        VARCHAR NOT NULL,
    full_name   VARCHAR,
    share       BOOLEAN NOT NULL DEFAULT FALSE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);
```

**Seeds (idempotent):**

| id | name          | full_name       | share |
|----|---------------|-----------------|-------|
| 1  | Public        | Base / Public   | true  |
| 2  | Internal User | Base / Internal | false |
| 3  | Administrator | Base / Admin    | false |

---

### res_users

```sql
CREATE TABLE IF NOT EXISTS res_users (
    id          SERIAL  PRIMARY KEY,
    login       VARCHAR NOT NULL UNIQUE,
    password    VARCHAR NOT NULL,   -- PBKDF2-SHA512 hash, never plaintext
    partner_id  INTEGER REFERENCES res_partner(id),
    company_id  INTEGER REFERENCES res_company(id),
    lang        VARCHAR NOT NULL DEFAULT 'en_US',
    tz          VARCHAR NOT NULL DEFAULT 'UTC',
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    share       BOOLEAN NOT NULL DEFAULT FALSE,  -- portal/external user flag
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);
```

**Password format:** `$pbkdf2-sha512$<rounds>$<base64-salt>$<base64-hash>` (passlib-compatible, verified via OpenSSL HMAC).

**Seeds (idempotent):**

| id | login | password         | partner_id      | company_id | lang  | tz  | active | share |
|----|-------|------------------|-----------------|------------|-------|-----|--------|-------|
| 1  | admin | (pbkdf2 hash)    | (admin partner) | 1          | en_US | UTC | true   | false |

---

### res_groups_users_rel

```sql
CREATE TABLE IF NOT EXISTS res_groups_users_rel (
    gid INTEGER NOT NULL REFERENCES res_groups(id) ON DELETE CASCADE,
    uid INTEGER NOT NULL REFERENCES res_users(id)  ON DELETE CASCADE,
    PRIMARY KEY (gid, uid)
);
```

Many2many junction between users and groups. No extra columns.

**Seeds (idempotent):**

| gid | uid | meaning |
|-----|-----|---------|
| 3   | 1   | admin user → Administrator group |

---

## Module: ir  (`modules/ir/IrModule.hpp`)

### ir_act_window

```sql
CREATE TABLE IF NOT EXISTS ir_act_window (
    id          SERIAL  PRIMARY KEY,
    name        VARCHAR NOT NULL,
    res_model   VARCHAR NOT NULL,    -- Odoo model name, e.g. 'res.partner'
    view_mode   VARCHAR NOT NULL DEFAULT 'list,form',
    domain      VARCHAR,             -- domain expression string, NULL = no filter
    context     VARCHAR NOT NULL DEFAULT '{}',
    target      VARCHAR NOT NULL DEFAULT 'current',  -- 'current' | 'new' | 'fullscreen'
    path        VARCHAR UNIQUE,      -- URL path fragment, e.g. 'contacts'
    help        TEXT,                -- empty-state help text
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);
```

**Seeds (idempotent):**

| id | name      | res_model   | view_mode | path      |
|----|-----------|-------------|-----------|-----------|
| 1  | Contacts  | res.partner | list,form | contacts  |
| 2  | Users     | res.users   | list,form | users     |
| 3  | Companies | res.company | list,form | companies |

Sequence reset to `setval('ir_act_window_id_seq', 3, true)` after seed.

---

### ir_ui_menu

```sql
CREATE TABLE IF NOT EXISTS ir_ui_menu (
    id          SERIAL  PRIMARY KEY,
    name        VARCHAR NOT NULL,
    parent_id   INTEGER REFERENCES ir_ui_menu(id) ON DELETE CASCADE,
    sequence    INTEGER NOT NULL DEFAULT 10,
    action_id   INTEGER REFERENCES ir_act_window(id) ON DELETE SET NULL,
    web_icon    VARCHAR,
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);
```

**Seeds (idempotent):**

| id | name      | parent_id | sequence | action_id |
|----|-----------|-----------|----------|-----------|
| 1  | Contacts  | NULL      | 10       | 1         |
| 2  | Users     | NULL      | 20       | 2         |
| 3  | Companies | NULL      | 30       | 3         |

Sequence reset to `setval('ir_ui_menu_id_seq', 3, true)` after seed.

---

## Table Inventory

| # | Table | Module | Rows (seeded) | Notes |
|---|-------|--------|---------------|-------|
| 1 | res_lang | base | 1 | en_US |
| 2 | res_currency | base | 5 | USD, EUR, GBP, JPY, CNY |
| 3 | res_country | base | 15 | Major countries |
| 4 | res_country_state | base | 0 | Empty, add on demand |
| 5 | res_partner | base | 2 | Company + admin partners (from auth seed) |
| 6 | res_company | auth | 1 | My Company |
| 7 | res_groups | auth | 3 | Public, Internal, Admin |
| 8 | res_users | auth | 1 | admin / admin |
| 9 | res_groups_users_rel | auth | 1 | admin → Admin group |
| 10 | ir_act_window | ir | 10 | actions 1-10 (base/account/uom/product) |
| 11 | ir_ui_menu | ir | 17 | 3-level hierarchy: apps 10/20/30/50 + leaves |
| 12 | account_account | account | 9 | Minimal chart of accounts |
| 13 | account_journal | account | 4 | SAL, PUR, BNK, CSH |
| 14 | account_tax | account | 2 | 15% Sales Tax, 15% Purchase Tax |
| 15 | account_move | account | 0 | Journal entries (user-created) |
| 16 | account_move_line | account | 0 | Journal entry lines |
| 17 | account_payment | account | 0 | Payments (user-created) |
| 18 | account_payment_term | account | 2 | Immediate Payment, 30 Days |
| 19 | uom_uom | uom | 15 | Units, kg, L, Hours, m, etc. |
| 20 | product_category | product | 3 | All, Goods, Services |
| 21 | product_product | product | 0 | Products (user-created) |

**Total: 21 tables**

---

## Module: account  (`modules/account/AccountModule.hpp`)

### account_account

```sql
CREATE TABLE IF NOT EXISTS account_account (
    id             SERIAL PRIMARY KEY,
    name           VARCHAR NOT NULL,
    code           VARCHAR NOT NULL,
    account_type   VARCHAR NOT NULL DEFAULT 'asset_current',
    internal_group VARCHAR NOT NULL DEFAULT 'asset',
    currency_id    INTEGER REFERENCES res_currency(id),
    company_id     INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    reconcile      BOOLEAN NOT NULL DEFAULT FALSE,
    active         BOOLEAN NOT NULL DEFAULT TRUE,
    note           TEXT,
    create_date    TIMESTAMP DEFAULT now(),
    write_date     TIMESTAMP DEFAULT now(),
    UNIQUE (code, company_id)
)
```

**Seeds (9 rows):**

| code | name | account_type | internal_group | reconcile |
|------|------|-------------|----------------|-----------|
| 1000 | Cash | asset_cash | asset | false |
| 1100 | Bank | asset_cash | asset | false |
| 1200 | Accounts Receivable | asset_receivable | asset | true |
| 2000 | Accounts Payable | liability_payable | liability | true |
| 3000 | Share Capital | equity | equity | false |
| 4000 | Sales Revenue | income | income | false |
| 5000 | Cost of Goods Sold | expense | expense | false |
| 6000 | Operating Expenses | expense | expense | false |
| 9999 | Undistributed Profit | equity_unaffected | equity | false |

---

### account_journal

```sql
CREATE TABLE IF NOT EXISTS account_journal (
    id                 SERIAL PRIMARY KEY,
    name               VARCHAR NOT NULL,
    code               VARCHAR(10) NOT NULL,
    type               VARCHAR NOT NULL DEFAULT 'general',
    currency_id        INTEGER REFERENCES res_currency(id),
    company_id         INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    default_account_id INTEGER REFERENCES account_account(id),
    sequence           INTEGER NOT NULL DEFAULT 10,
    active             BOOLEAN NOT NULL DEFAULT TRUE,
    create_date        TIMESTAMP DEFAULT now(),
    write_date         TIMESTAMP DEFAULT now(),
    UNIQUE (code, company_id)
)
```

**Seeds:**

| code | name | type | default_account_id |
|------|------|------|--------------------|
| SAL | Sales | sale | NULL |
| PUR | Purchases | purchase | NULL |
| BNK | Bank | bank | 1100 (Bank) |
| CSH | Cash | cash | 1000 (Cash) |

---

### account_tax

```sql
CREATE TABLE IF NOT EXISTS account_tax (
    id            SERIAL PRIMARY KEY,
    name          VARCHAR NOT NULL,
    amount        NUMERIC(16,4) NOT NULL DEFAULT 0,
    amount_type   VARCHAR NOT NULL DEFAULT 'percent',
    type_tax_use  VARCHAR NOT NULL DEFAULT 'sale',
    price_include BOOLEAN NOT NULL DEFAULT FALSE,
    company_id    INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    active        BOOLEAN NOT NULL DEFAULT TRUE,
    description   VARCHAR,
    create_date   TIMESTAMP DEFAULT now(),
    write_date    TIMESTAMP DEFAULT now()
)
```

**Seeds:** 15% Sales Tax, 15% Purchase Tax.

---

### account_move

```sql
CREATE TABLE IF NOT EXISTS account_move (
    id              SERIAL PRIMARY KEY,
    name            VARCHAR NOT NULL DEFAULT '/',
    ref             VARCHAR,
    narration       TEXT,
    move_type       VARCHAR NOT NULL DEFAULT 'entry',
    state           VARCHAR NOT NULL DEFAULT 'draft',
    date            DATE NOT NULL DEFAULT CURRENT_DATE,
    invoice_date    DATE,
    due_date        DATE,
    journal_id      INTEGER NOT NULL REFERENCES account_journal(id),
    partner_id      INTEGER REFERENCES res_partner(id),
    company_id      INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    currency_id     INTEGER REFERENCES res_currency(id),
    payment_state   VARCHAR NOT NULL DEFAULT 'not_paid',
    amount_untaxed  NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_tax      NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_total    NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_residual NUMERIC(16,2) NOT NULL DEFAULT 0,
    create_date     TIMESTAMP DEFAULT now(),
    write_date      TIMESTAMP DEFAULT now()
)
```

**State machine:** `draft → [action_post()] → posted → [button_cancel()] → cancel`

Name format on post: `{JOURNAL_CODE}/{YEAR}/{4-digit-seq}` (e.g. `SAL/2026/0001`).

---

### account_move_line

```sql
CREATE TABLE IF NOT EXISTS account_move_line (
    id               SERIAL PRIMARY KEY,
    move_id          INTEGER NOT NULL REFERENCES account_move(id) ON DELETE CASCADE,
    account_id       INTEGER NOT NULL REFERENCES account_account(id),
    journal_id       INTEGER REFERENCES account_journal(id),
    company_id       INTEGER REFERENCES res_company(id),
    date             DATE,
    name             VARCHAR,
    ref              VARCHAR,
    partner_id       INTEGER REFERENCES res_partner(id),
    debit            NUMERIC(16,2) NOT NULL DEFAULT 0,
    credit           NUMERIC(16,2) NOT NULL DEFAULT 0,
    balance          NUMERIC(16,2) GENERATED ALWAYS AS (debit - credit) STORED,
    amount_currency  NUMERIC(16,2) NOT NULL DEFAULT 0,
    quantity         NUMERIC(16,4) NOT NULL DEFAULT 1,
    tax_line_id      INTEGER REFERENCES account_tax(id),
    reconciled       BOOLEAN NOT NULL DEFAULT FALSE,
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
)
```

Note: `balance` is a PostgreSQL `GENERATED ALWAYS AS` column — never included in INSERT.

---

### account_payment

```sql
CREATE TABLE IF NOT EXISTS account_payment (
    id            SERIAL PRIMARY KEY,
    name          VARCHAR NOT NULL DEFAULT '/',
    date          DATE NOT NULL DEFAULT CURRENT_DATE,
    journal_id    INTEGER NOT NULL REFERENCES account_journal(id),
    partner_id    INTEGER REFERENCES res_partner(id),
    company_id    INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    currency_id   INTEGER REFERENCES res_currency(id),
    amount        NUMERIC(16,2) NOT NULL DEFAULT 0,
    payment_type  VARCHAR NOT NULL DEFAULT 'inbound',
    partner_type  VARCHAR NOT NULL DEFAULT 'customer',
    state         VARCHAR NOT NULL DEFAULT 'draft',
    move_id       INTEGER REFERENCES account_move(id),
    memo          VARCHAR,
    create_date   TIMESTAMP DEFAULT now(),
    write_date    TIMESTAMP DEFAULT now()
)
```

**State machine:** `draft → [action_post()] → posted → [action_cancel()] → cancelled`

On post: creates `account_move` + 2 `account_move_line` rows (DR/CR sides).

---

### account_payment_term

```sql
CREATE TABLE IF NOT EXISTS account_payment_term (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR NOT NULL,
    note        TEXT,
    lines_json  TEXT NOT NULL DEFAULT '[{"days":0,"value":"balance","value_amount":0}]',
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
)
```

**Seeds:** "Immediate Payment" (0 days), "30 Days" (30 days).

---

## Foreign Key Graph

```
res_currency  ←── res_country
                       └──── res_country_state
                                   └──── res_partner ──── res_company ──── res_users
                                                └───────── res_groups ←─ res_groups_users_rel ─→ res_users
res_currency  ←── res_company
res_partner   ←── res_company

ir_act_window ←── ir_ui_menu

res_currency  ←── account_account ←── account_journal
res_company   ←── account_account
res_company   ←── account_journal
res_company   ←── account_tax
res_company   ←── account_move
res_partner   ←── account_move
account_journal ←── account_move ←── account_move_line ──→ account_account
                                                         └──→ account_tax
account_journal ←── account_payment ──→ account_move
res_partner   ←── account_payment
```

---

## Sequence Conventions

- All PKs use `SERIAL` (auto-increment via `<table>_id_seq`).
- After bulk seeds that hard-code IDs, the sequence is reset:
  ```sql
  SELECT setval('<table>_id_seq', <max_id>, true);
  ```
- `create_date` and `write_date` default to `now()` at insert; `write_date` is not auto-updated on UPDATE (manual update in write() if needed).

---

## ORM Conventions

- Model name → table name: dots replaced by underscores (`res.partner` → `res_partner`).
- Declared via `ODOO_MODEL("res.partner", "res_partner")` macro in each model class.
- Many2one fields stored as bare `INTEGER` FK column (the `[id, "Name"]` tuple used in API responses is assembled at serialization time, not stored).
- `BaseModel<T>::normalizeForDb_()` converts JSON `false` → `NULL` and `[id,"Name"]` → `id` before binding SQL params.
- Boolean fields use PostgreSQL native `BOOLEAN` (`t`/`f` wire format, parsed in `rowsToJson_`).

---

## Module: uom  (`modules/uom/UomModule.hpp`)

### uom_uom

```sql
CREATE TABLE IF NOT EXISTS uom_uom (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR NOT NULL,
    category    VARCHAR NOT NULL DEFAULT 'Unit',
    uom_type    VARCHAR NOT NULL DEFAULT 'reference',
    factor      NUMERIC(12,6) NOT NULL DEFAULT 1.0,
    rounding    NUMERIC(12,6) NOT NULL DEFAULT 0.01,
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
)
```

---

## Module: product  (`modules/product/ProductModule.hpp`)

### product_category

```sql
CREATE TABLE IF NOT EXISTS product_category (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR NOT NULL,
    parent_id   INTEGER REFERENCES product_category(id) ON DELETE SET NULL,
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
)
```

### product_product

```sql
CREATE TABLE IF NOT EXISTS product_product (
    id               SERIAL PRIMARY KEY,
    name             VARCHAR NOT NULL,
    default_code     VARCHAR,
    barcode          VARCHAR,
    description      TEXT,
    type             VARCHAR NOT NULL DEFAULT 'consu',
    categ_id         INTEGER REFERENCES product_category(id) ON DELETE SET NULL,
    uom_id           INTEGER NOT NULL REFERENCES uom_uom(id) DEFAULT 1,
    uom_po_id        INTEGER NOT NULL REFERENCES uom_uom(id) DEFAULT 1,
    list_price       NUMERIC(16,4) NOT NULL DEFAULT 0,
    standard_price   NUMERIC(16,4) NOT NULL DEFAULT 0,
    volume           NUMERIC(16,4) NOT NULL DEFAULT 0,
    weight           NUMERIC(16,4) NOT NULL DEFAULT 0,
    sale_ok          BOOLEAN NOT NULL DEFAULT TRUE,
    purchase_ok      BOOLEAN NOT NULL DEFAULT TRUE,
    company_id       INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
    active           BOOLEAN NOT NULL DEFAULT TRUE,
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
)
```

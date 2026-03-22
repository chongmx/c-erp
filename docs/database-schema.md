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

-- Migration (idempotent)
ALTER TABLE res_country ADD COLUMN IF NOT EXISTS active BOOLEAN NOT NULL DEFAULT TRUE;
```

**Seeds:** 250 world countries via `WorldData.hpp` → `seedWorldData_(txn)`, inserted as:
```sql
INSERT INTO res_country (name, code, phone_code) VALUES (...) ON CONFLICT (code) DO NOTHING;
```
Countries with states seeded: MY, US, CA, GB, AU, DE, FR, IN, JP, CN, ID, BR, MX, ZA, AE, SA, SG, NZ, NL, BE, CH, ES, IT, NG, EG, PK, BD, TH, VN, PH (~700 states total).

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

**Migration (idempotent):**
```sql
-- Required for WorldData ON CONFLICT (country_id, code) DO NOTHING seeding
DO $$ BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname='res_country_state_country_id_code_key') THEN
        ALTER TABLE res_country_state ADD CONSTRAINT res_country_state_country_id_code_key
            UNIQUE (country_id, code);
    END IF;
END $$;
```

**Seeds:** ~700 state rows across 30+ countries, inserted by `WorldData.hpp`.

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

**Idempotent migrations:**
```sql
ALTER TABLE account_move ADD COLUMN IF NOT EXISTS payment_term_id INTEGER REFERENCES account_payment_term(id);
ALTER TABLE account_move ADD COLUMN IF NOT EXISTS invoice_origin  VARCHAR;
```

**State machine:** `draft → [action_post()] → posted → [button_cancel()] → cancel → [button_draft()] → draft`

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

**Idempotent migrations:**
```sql
ALTER TABLE account_move_line ADD COLUMN IF NOT EXISTS price_unit    NUMERIC(16,4) NOT NULL DEFAULT 0;
ALTER TABLE account_move_line ADD COLUMN IF NOT EXISTS display_type  VARCHAR;
```

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

**Idempotent column migrations (applied at startup):**

```sql
-- Phase 17g additions
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS expense_ok   BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS image_1920   TEXT;

-- Phase A3 tab content
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS description_sale        TEXT;
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS description_purchase    TEXT;
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS income_account_id       INTEGER REFERENCES account_account(id);
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS expense_account_id      INTEGER REFERENCES account_account(id);

-- Sales tab (Phase A3 extended)
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS invoice_policy          VARCHAR NOT NULL DEFAULT 'order';
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS sale_line_warn          VARCHAR NOT NULL DEFAULT 'no-message';
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS sale_line_warn_msg      TEXT;

-- Purchase tab (Phase A3 extended)
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS purchase_method         VARCHAR NOT NULL DEFAULT 'purchase';
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS purchase_lead_time      NUMERIC(8,2) NOT NULL DEFAULT 0;
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS purchase_line_warn      VARCHAR NOT NULL DEFAULT 'no-message';
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS purchase_line_warn_msg  TEXT;
```

**Full extended column list:**

| column | type | default | notes |
|--------|------|---------|-------|
| expense_ok | BOOLEAN | false | Can be Expensed |
| image_1920 | TEXT | NULL | Base64 product image |
| description_sale | TEXT | NULL | Sales tab description |
| description_purchase | TEXT | NULL | Purchase tab description |
| income_account_id | INTEGER | NULL | FK → account_account |
| expense_account_id | INTEGER | NULL | FK → account_account |
| invoice_policy | VARCHAR | 'order' | 'order' \| 'delivery' |
| sale_line_warn | VARCHAR | 'no-message' | 'no-message' \| 'warning' \| 'block' |
| sale_line_warn_msg | TEXT | NULL | shown when warn ≠ no-message |
| purchase_method | VARCHAR | 'purchase' | 'purchase' \| 'receive' |
| purchase_lead_time | NUMERIC(8,2) | 0 | days |
| purchase_line_warn | VARCHAR | 'no-message' | same values as sale_line_warn |
| purchase_line_warn_msg | TEXT | NULL | shown when warn ≠ no-message |

---

## Module: sale  (`modules/sale/SaleModule.hpp`)

### sale_order

```sql
CREATE TABLE IF NOT EXISTS sale_order (
    id              SERIAL PRIMARY KEY,
    name            VARCHAR NOT NULL DEFAULT '/',
    state           VARCHAR NOT NULL DEFAULT 'draft',
    partner_id      INTEGER NOT NULL REFERENCES res_partner(id),
    company_id      INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    currency_id     INTEGER REFERENCES res_currency(id),
    payment_term_id INTEGER REFERENCES account_payment_term(id),
    date_order      TIMESTAMP NOT NULL DEFAULT now(),
    validity_date   DATE,
    note            TEXT,
    amount_untaxed  NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_tax      NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_total    NUMERIC(16,2) NOT NULL DEFAULT 0,
    invoice_status  VARCHAR NOT NULL DEFAULT 'nothing',
    create_date     TIMESTAMP DEFAULT now(),
    write_date      TIMESTAMP DEFAULT now()
)
```

**State machine:** `draft → [action_confirm()] → sale → [action_cancel()] → cancel`

On confirm: auto-creates `stock_picking` (WH/OUT) with move lines for each SO line.

### sale_order_line

```sql
CREATE TABLE IF NOT EXISTS sale_order_line (
    id           SERIAL PRIMARY KEY,
    order_id     INTEGER NOT NULL REFERENCES sale_order(id) ON DELETE CASCADE,
    product_id   INTEGER REFERENCES product_product(id),
    name         TEXT NOT NULL DEFAULT '',
    product_uom  INTEGER REFERENCES uom_uom(id),
    product_qty  NUMERIC(16,4) NOT NULL DEFAULT 1,
    price_unit   NUMERIC(16,4) NOT NULL DEFAULT 0,
    price_subtotal NUMERIC(16,4) NOT NULL DEFAULT 0,
    qty_delivered  NUMERIC(16,4) NOT NULL DEFAULT 0,
    qty_invoiced   NUMERIC(16,4) NOT NULL DEFAULT 0,
    sequence     INTEGER NOT NULL DEFAULT 10,
    create_date  TIMESTAMP DEFAULT now(),
    write_date   TIMESTAMP DEFAULT now()
)
```

---

## Module: purchase  (`modules/purchase/PurchaseModule.hpp`)

### purchase_order

```sql
CREATE TABLE IF NOT EXISTS purchase_order (
    id              SERIAL PRIMARY KEY,
    name            VARCHAR NOT NULL DEFAULT '/',
    state           VARCHAR NOT NULL DEFAULT 'draft',
    partner_id      INTEGER NOT NULL REFERENCES res_partner(id),
    company_id      INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    currency_id     INTEGER REFERENCES res_currency(id),
    payment_term_id INTEGER REFERENCES account_payment_term(id),
    date_order      TIMESTAMP NOT NULL DEFAULT now(),
    date_planned    TIMESTAMP,
    note            TEXT,
    amount_untaxed  NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_tax      NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_total    NUMERIC(16,2) NOT NULL DEFAULT 0,
    invoice_status  VARCHAR NOT NULL DEFAULT 'nothing',
    create_date     TIMESTAMP DEFAULT now(),
    write_date      TIMESTAMP DEFAULT now()
)
```

**State machine:** `draft → [button_confirm()] → purchase → [button_cancel()] → cancel`

On confirm: auto-creates `stock_picking` (WH/IN).

### purchase_order_line

```sql
CREATE TABLE IF NOT EXISTS purchase_order_line (
    id           SERIAL PRIMARY KEY,
    order_id     INTEGER NOT NULL REFERENCES purchase_order(id) ON DELETE CASCADE,
    product_id   INTEGER REFERENCES product_product(id),
    name         TEXT NOT NULL DEFAULT '',
    product_uom  INTEGER REFERENCES uom_uom(id),
    product_qty  NUMERIC(16,4) NOT NULL DEFAULT 1,
    price_unit   NUMERIC(16,4) NOT NULL DEFAULT 0,
    price_subtotal NUMERIC(16,4) NOT NULL DEFAULT 0,
    qty_received   NUMERIC(16,4) NOT NULL DEFAULT 0,
    qty_billed     NUMERIC(16,4) NOT NULL DEFAULT 0,
    date_planned TIMESTAMP,
    sequence     INTEGER NOT NULL DEFAULT 10,
    create_date  TIMESTAMP DEFAULT now(),
    write_date   TIMESTAMP DEFAULT now()
)
```

---

## Module: stock  (`modules/stock/StockModule.hpp`)

### stock_location

```sql
CREATE TABLE IF NOT EXISTS stock_location (
    id              SERIAL PRIMARY KEY,
    name            VARCHAR NOT NULL,
    complete_name   VARCHAR,
    usage           VARCHAR NOT NULL DEFAULT 'internal',
    location_id     INTEGER REFERENCES stock_location(id) ON DELETE SET NULL,
    active          BOOLEAN NOT NULL DEFAULT TRUE,
    company_id      INTEGER REFERENCES res_company(id),
    create_date     TIMESTAMP DEFAULT now(),
    write_date      TIMESTAMP DEFAULT now()
)
```

**Seeds (7 rows):**

| id | name | usage | parent |
|----|------|-------|--------|
| 1  | Physical Locations | view | NULL |
| 2  | My Company | view | 1 |
| 3  | WH | view | 2 |
| 4  | Stock | internal | 3 |
| 5  | Vendors | supplier | NULL |
| 6  | Customers | customer | NULL |
| 7  | Inventory Adjustments | inventory | NULL |

### stock_picking_type

```sql
CREATE TABLE IF NOT EXISTS stock_picking_type (
    id               SERIAL PRIMARY KEY,
    name             VARCHAR NOT NULL,
    code             VARCHAR NOT NULL DEFAULT 'internal',
    sequence_code    VARCHAR NOT NULL DEFAULT 'WH',
    warehouse_id     INTEGER REFERENCES stock_warehouse(id) ON DELETE SET NULL,
    default_location_id      INTEGER REFERENCES stock_location(id),
    default_location_dest_id INTEGER REFERENCES stock_location(id),
    active           BOOLEAN NOT NULL DEFAULT TRUE,
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
)
```

**Seeds (3 rows):**

| id | name | code | sequence_code |
|----|------|------|---------------|
| 1  | Receipts | incoming | WH/IN |
| 2  | Delivery Orders | outgoing | WH/OUT |
| 3  | Internal Transfers | internal | WH/INT |

### stock_warehouse

```sql
CREATE TABLE IF NOT EXISTS stock_warehouse (
    id               SERIAL PRIMARY KEY,
    name             VARCHAR NOT NULL,
    code             VARCHAR(5) NOT NULL UNIQUE,
    company_id       INTEGER REFERENCES res_company(id)        ON DELETE SET NULL,
    lot_stock_id     INTEGER REFERENCES stock_location(id)     ON DELETE SET NULL,
    view_location_id INTEGER REFERENCES stock_location(id)     ON DELETE SET NULL,
    in_type_id       INTEGER REFERENCES stock_picking_type(id) ON DELETE SET NULL,
    out_type_id      INTEGER REFERENCES stock_picking_type(id) ON DELETE SET NULL,
    int_type_id      INTEGER REFERENCES stock_picking_type(id) ON DELETE SET NULL,
    active           BOOLEAN NOT NULL DEFAULT TRUE,
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
)
```

**Seeds (1 row):**

| id | name | code | company_id | lot_stock_id | view_location_id | in_type_id | out_type_id | int_type_id |
|----|------|------|------------|--------------|------------------|------------|-------------|-------------|
| 1  | Main Warehouse | WH | 1 | 4 (Stock) | 3 (WH view) | 1 | 2 | 3 |

### stock_picking

```sql
CREATE TABLE IF NOT EXISTS stock_picking (
    id               SERIAL PRIMARY KEY,
    name             VARCHAR NOT NULL DEFAULT '/',
    state            VARCHAR NOT NULL DEFAULT 'draft',
    picking_type_id  INTEGER NOT NULL REFERENCES stock_picking_type(id),
    partner_id       INTEGER REFERENCES res_partner(id),
    location_id      INTEGER NOT NULL REFERENCES stock_location(id),
    location_dest_id INTEGER NOT NULL REFERENCES stock_location(id),
    scheduled_date   TIMESTAMP,
    date_done        TIMESTAMP,
    origin           VARCHAR,
    note             TEXT,
    company_id       INTEGER REFERENCES res_company(id),
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
)
```

**State machine:** `draft → [action_confirm()] → confirmed → [button_validate()] → done`

Also adds `user_id INTEGER REFERENCES res_users(id)` via migration.

### stock_move

```sql
CREATE TABLE IF NOT EXISTS stock_move (
    id               SERIAL PRIMARY KEY,
    name             VARCHAR NOT NULL DEFAULT '/',
    state            VARCHAR NOT NULL DEFAULT 'draft',
    picking_id       INTEGER REFERENCES stock_picking(id) ON DELETE CASCADE,
    product_id       INTEGER NOT NULL REFERENCES product_product(id),
    product_uom_qty  NUMERIC(16,4) NOT NULL DEFAULT 0,
    quantity_done    NUMERIC(16,4) NOT NULL DEFAULT 0,
    location_id      INTEGER NOT NULL REFERENCES stock_location(id),
    location_dest_id INTEGER NOT NULL REFERENCES stock_location(id),
    origin           VARCHAR,
    company_id       INTEGER REFERENCES res_company(id),
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
)
```

---

## Module: mrp  (`modules/mrp/MrpModule.hpp`)

### mrp_bom

```sql
CREATE TABLE IF NOT EXISTS mrp_bom (
    id          SERIAL PRIMARY KEY,
    product_id  INTEGER NOT NULL REFERENCES product_product(id),
    product_qty NUMERIC(16,4) NOT NULL DEFAULT 1,
    type        VARCHAR NOT NULL DEFAULT 'normal',
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    company_id  INTEGER REFERENCES res_company(id),
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
)
```

### mrp_bom_line

```sql
CREATE TABLE IF NOT EXISTS mrp_bom_line (
    id          SERIAL PRIMARY KEY,
    bom_id      INTEGER NOT NULL REFERENCES mrp_bom(id) ON DELETE CASCADE,
    product_id  INTEGER NOT NULL REFERENCES product_product(id),
    product_qty NUMERIC(16,4) NOT NULL DEFAULT 1,
    sequence    INTEGER NOT NULL DEFAULT 10
)
```

---

## Module: mail  (`modules/mail/MailModule.hpp`)

### mail_message

```sql
CREATE TABLE IF NOT EXISTS mail_message (
    id        SERIAL PRIMARY KEY,
    res_model VARCHAR NOT NULL,
    res_id    INTEGER NOT NULL,
    author_id INTEGER REFERENCES res_users(id),
    body      TEXT NOT NULL DEFAULT '',
    subtype   VARCHAR NOT NULL DEFAULT 'note',
    date      TIMESTAMP NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS mail_message_res_idx ON mail_message(res_model, res_id);
```

---

## Module: hr  (`modules/hr/HrModule.hpp`)

### resource_calendar

```sql
CREATE TABLE IF NOT EXISTS resource_calendar (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR NOT NULL,
    hours_per_day NUMERIC(4,2) NOT NULL DEFAULT 8,
    company_id  INTEGER REFERENCES res_company(id),
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
)
```

### hr_department

```sql
CREATE TABLE IF NOT EXISTS hr_department (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR NOT NULL,
    company_id  INTEGER REFERENCES res_company(id),
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
)
```

### hr_job

```sql
CREATE TABLE IF NOT EXISTS hr_job (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR NOT NULL,
    department_id INTEGER REFERENCES hr_department(id),
    company_id  INTEGER REFERENCES res_company(id),
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
)
```

### hr_employee

```sql
CREATE TABLE IF NOT EXISTS hr_employee (
    id                SERIAL PRIMARY KEY,
    name              VARCHAR NOT NULL,
    job_id            INTEGER REFERENCES hr_job(id),
    department_id     INTEGER REFERENCES hr_department(id),
    resource_calendar_id INTEGER REFERENCES resource_calendar(id),
    work_email        VARCHAR,
    work_phone        VARCHAR,
    company_id        INTEGER REFERENCES res_company(id),
    active            BOOLEAN NOT NULL DEFAULT TRUE,
    create_date       TIMESTAMP DEFAULT now(),
    write_date        TIMESTAMP DEFAULT now()
)
```

---

## Table Inventory (current)

| # | Table | Module | Notes |
|---|-------|--------|-------|
| 1 | res_lang | base | 1 seed (en_US) |
| 2 | res_currency | base | 5 seeds |
| 3 | res_country | base | 250 seeds (WorldData.hpp), active flag |
| 4 | res_country_state | base | ~700 seeds, UNIQUE(country_id,code) |
| 5 | res_partner | base | 2 seeds (admin+company) |
| 6 | res_company | auth | 1 seed |
| 7 | res_groups | auth | 3 seeds |
| 8 | res_users | auth | 1 seed (admin) |
| 9 | res_groups_users_rel | auth | junction |
| 10 | ir_act_window | ir | actions 1-25 + 34 used |
| 11 | ir_ui_menu | ir | 3-level hierarchy |
| 12 | ir_config_parameter | ir | key-value settings |
| 13 | account_account | account | 9 seeds |
| 14 | account_journal | account | 4 seeds |
| 15 | account_tax | account | 2 seeds |
| 16 | account_move | account | user-created |
| 17 | account_move_line | account | user-created |
| 18 | account_payment | account | user-created |
| 19 | account_payment_term | account | 2 seeds |
| 20 | uom_uom | uom | 15 seeds |
| 21 | product_category | product | 3 seeds |
| 22 | product_product | product | user-created |
| 23 | sale_order | sale | user-created |
| 24 | sale_order_line | sale | user-created |
| 25 | purchase_order | purchase | user-created |
| 26 | purchase_order_line | purchase | user-created |
| 27 | stock_location | stock | 7 seeds |
| 28 | stock_picking_type | stock | 3 seeds |
| 29 | stock_warehouse | stock | 1 seed (Main Warehouse) |
| 30 | stock_picking | stock | user-created |
| 31 | stock_move | stock | user-created |
| 32 | mrp_bom | mrp | user-created |
| 33 | mrp_bom_line | mrp | user-created |
| 34 | mail_message | mail | chatter entries |
| 35 | resource_calendar | hr | user-created |
| 36 | hr_department | hr | user-created |
| 37 | hr_job | hr | user-created |
| 38 | hr_employee | hr | user-created |

**Total: 38 tables** (plus junction tables and generated columns)

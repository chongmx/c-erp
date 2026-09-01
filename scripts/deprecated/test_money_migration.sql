-- =============================================================
-- Dry-run of the P2 money migrations against live data.
-- Everything happens inside a transaction that is ROLLED BACK,
-- so the database is unchanged when this finishes.
--
--   PGPASSWORD=odoo psql -h localhost -U odoo -d odoo -f scripts/test_money_migration.sql
-- =============================================================
\set ON_ERROR_STOP on
BEGIN;

\echo '=== BEFORE ==='
SELECT id, amount_untaxed, amount_total, amount_residual FROM account_move ORDER BY id LIMIT 3;
SELECT id, list_price, standard_price FROM product_product ORDER BY id LIMIT 3;
SELECT id, debit, credit, balance FROM account_move_line ORDER BY id LIMIT 3;

-- ── 901 reference data ───────────────────────────────────────
CREATE TABLE IF NOT EXISTS decimal_precision (
    id     SERIAL  PRIMARY KEY,
    name   VARCHAR NOT NULL UNIQUE,
    digits INTEGER NOT NULL DEFAULT 2 CHECK (digits BETWEEN 0 AND 6)
);
INSERT INTO decimal_precision (name, digits) VALUES
    ('Product Price', 5), ('Product UoM', 4), ('Account', 2),
    ('Discount', 2), ('Stock', 4)
ON CONFLICT (name) DO NOTHING;

ALTER TABLE res_currency ADD COLUMN IF NOT EXISTS rate BIGINT NOT NULL DEFAULT 1000000;

INSERT INTO res_currency (name, symbol, position, rounding, decimal_places, active, rate)
VALUES ('MYR', 'RM', 'before', 0.01, 2, TRUE, 1000000)
ON CONFLICT (name) DO UPDATE SET active = TRUE, rate = 1000000;

ALTER TABLE res_company ADD COLUMN IF NOT EXISTS currency_id INTEGER REFERENCES res_currency(id);
UPDATE res_company SET currency_id = (SELECT id FROM res_currency WHERE name='MYR')
 WHERE currency_id IS NULL;
UPDATE res_currency SET active = FALSE WHERE name NOT IN ('MYR','USD');

INSERT INTO account_account (code, name, account_type, internal_group, company_id)
SELECT '7900', 'Foreign Exchange Gain/Loss', 'expense', 'expense', c.id
  FROM res_company c
 WHERE NOT EXISTS (
     SELECT 1 FROM account_account a WHERE a.code='7900' AND a.company_id=c.id
 );

-- ── 902 document columns ─────────────────────────────────────
ALTER TABLE account_move
    ADD COLUMN IF NOT EXISTS line_precision INTEGER NULL
        CHECK (line_precision IS NULL OR line_precision BETWEEN 0 AND 6),
    ADD COLUMN IF NOT EXISTS currency_rate BIGINT NOT NULL DEFAULT 1000000,
    ADD COLUMN IF NOT EXISTS amount_total_base    BIGINT NOT NULL DEFAULT 0,
    ADD COLUMN IF NOT EXISTS amount_residual_base BIGINT NOT NULL DEFAULT 0;

-- ── 910 account_move_line (generated column drop/recreate) ───
ALTER TABLE account_move_line DROP COLUMN IF EXISTS balance;
ALTER TABLE account_move_line
    ALTER COLUMN debit           TYPE BIGINT USING ROUND(COALESCE(debit,0)           * 1000000)::BIGINT,
    ALTER COLUMN credit          TYPE BIGINT USING ROUND(COALESCE(credit,0)          * 1000000)::BIGINT,
    ALTER COLUMN amount_currency TYPE BIGINT USING ROUND(COALESCE(amount_currency,0) * 1000000)::BIGINT,
    ALTER COLUMN price_unit      TYPE BIGINT USING ROUND(COALESCE(price_unit,0)      * 1000000)::BIGINT;
ALTER TABLE account_move_line
    ADD COLUMN balance BIGINT GENERATED ALWAYS AS (debit - credit) STORED;

-- ── 911 account_move ─────────────────────────────────────────
ALTER TABLE account_move
    ALTER COLUMN amount_untaxed  TYPE BIGINT USING ROUND(COALESCE(amount_untaxed,0)  * 1000000)::BIGINT,
    ALTER COLUMN amount_tax      TYPE BIGINT USING ROUND(COALESCE(amount_tax,0)      * 1000000)::BIGINT,
    ALTER COLUMN amount_total    TYPE BIGINT USING ROUND(COALESCE(amount_total,0)    * 1000000)::BIGINT,
    ALTER COLUMN amount_residual TYPE BIGINT USING ROUND(COALESCE(amount_residual,0) * 1000000)::BIGINT;

-- ── 912 account_payment ──────────────────────────────────────
ALTER TABLE account_payment
    ALTER COLUMN amount TYPE BIGINT USING ROUND(COALESCE(amount,0) * 1000000)::BIGINT;
ALTER TABLE account_payment
    ADD COLUMN IF NOT EXISTS amount_base   BIGINT NOT NULL DEFAULT 0,
    ADD COLUMN IF NOT EXISTS currency_rate BIGINT NOT NULL DEFAULT 1000000;
UPDATE account_payment SET amount_base = amount WHERE amount_base = 0;

-- ── 950 product ──────────────────────────────────────────────
ALTER TABLE product_product
    ALTER COLUMN list_price     TYPE BIGINT USING ROUND(COALESCE(list_price,0)     * 1000000)::BIGINT,
    ALTER COLUMN standard_price TYPE BIGINT USING ROUND(COALESCE(standard_price,0) * 1000000)::BIGINT;

\echo ''
\echo '=== AFTER (values are now micro-units) ==='
SELECT id, amount_untaxed, amount_total, amount_residual FROM account_move ORDER BY id LIMIT 3;
SELECT id, list_price, standard_price FROM product_product ORDER BY id LIMIT 3;
SELECT id, debit, credit, balance FROM account_move_line ORDER BY id LIMIT 3;

\echo ''
\echo '=== VERIFY: round-trip back to major units must match the BEFORE values ==='
SELECT id,
       (amount_total::numeric / 1000000)::numeric(16,2) AS total_major,
       (amount_residual::numeric / 1000000)::numeric(16,2) AS residual_major
  FROM account_move ORDER BY id LIMIT 3;

\echo ''
\echo '=== VERIFY: generated balance column still computes ==='
SELECT count(*) AS rows_where_balance_wrong
  FROM account_move_line WHERE balance <> debit - credit;

\echo ''
\echo '=== VERIFY: reference data ==='
SELECT name, digits FROM decimal_precision ORDER BY name;
SELECT name, symbol, decimal_places, active, rate FROM res_currency ORDER BY name;
SELECT c.id, c.name, cur.name AS base_currency FROM res_company c
  LEFT JOIN res_currency cur ON cur.id = c.currency_id;
SELECT code, name FROM account_account WHERE code = '7900';

\echo ''
\echo '=== VERIFY: column types are BIGINT ==='
SELECT table_name, column_name, data_type
  FROM information_schema.columns
 WHERE table_name IN ('account_move','account_move_line','product_product','account_payment')
   AND column_name IN ('amount_total','debit','credit','balance','list_price','amount','price_unit')
 ORDER BY table_name, column_name;

ROLLBACK;
\echo ''
\echo '*** ROLLED BACK — database unchanged ***'

// ============================================================
// core/MoneyMigrations.cpp — see MoneyMigrations.hpp
// ============================================================
#include "MoneyMigrations.hpp"
#include "infrastructure/MigrationRunner.hpp"

namespace odoo::core {

using odoo::infrastructure::MigrationRunner;

namespace {

// Every money/price/quantity column becomes BIGINT micro-units (scale 6).
// Columns NOT touched, deliberately: margin_top/right/bottom/left,
// line_height, footer_line_width (report layout — points, not money),
// hours_per_day, purchase_lead_time, rounding, factor, volume, weight
// (physical/config quantities that never enter the ledger).
constexpr const char* kToMicros = " TYPE BIGINT USING ROUND(COALESCE(";

/// ALTER one column NUMERIC -> BIGINT preserving the value.
std::string toMicros(const std::string& col) {
    return "    ALTER COLUMN " + col + kToMicros + col + ", 0) * 1000000)::BIGINT";
}

std::string alterCols(const std::string& table, std::initializer_list<const char*> cols) {
    std::string sql = "ALTER TABLE " + table + "\n";
    bool first = true;
    for (const char* c : cols) {
        if (!first) sql += ",\n";
        sql += toMicros(c);
        first = false;
    }
    return sql + ";\n";
}

} // namespace


void registerMoneyMigrations(MigrationRunner& runner) {

    // ── 901 — reference data ──────────────────────────────────
    runner.registerMigration({901, "money_reference_data", R"(
        -- User-configurable DISPLAY precision (docs/048 §2.1). Storage is
        -- always scale 6; these govern rounding and rendering only.
        CREATE TABLE IF NOT EXISTS decimal_precision (
            id     SERIAL  PRIMARY KEY,
            name   VARCHAR NOT NULL UNIQUE,
            digits INTEGER NOT NULL DEFAULT 2 CHECK (digits BETWEEN 0 AND 6)
        );
        INSERT INTO decimal_precision (name, digits) VALUES
            ('Product Price', 5),
            ('Product UoM',   4),
            ('Account',       2),
            ('Discount',      2),
            ('Stock',         4)
        ON CONFLICT (name) DO NOTHING;

        -- Current FX rate per currency, scale 6. docs/048 §4.3 convention:
        -- rate = how many BASE units equal 1 unit of THIS currency.
        -- A dated rate table is deliberately not used: every document
        -- snapshots its own rate, so history lives on the documents.
        ALTER TABLE res_currency
            ADD COLUMN IF NOT EXISTS rate BIGINT NOT NULL DEFAULT 1000000;

        -- Base currency: MYR.
        INSERT INTO res_currency (name, symbol, position, rounding, decimal_places, active, rate)
        VALUES ('MYR', 'RM', 'before', 0.01, 2, TRUE, 1000000)
        ON CONFLICT (name) DO UPDATE SET active = TRUE, rate = 1000000;

        ALTER TABLE res_company
            ADD COLUMN IF NOT EXISTS currency_id INTEGER REFERENCES res_currency(id);
        UPDATE res_company
           SET currency_id = (SELECT id FROM res_currency WHERE name = 'MYR')
         WHERE currency_id IS NULL;

        -- Currencies without a maintained rate stay inactive so they cannot
        -- be picked by accident (docs/048 §8 Q3).
        UPDATE res_currency SET active = FALSE
         WHERE name NOT IN ('MYR', 'USD');

        -- Realised FX difference lands here (docs/048 §4.6). One account
        -- holding both directions, so it nets to the period's FX result.
        --
        -- Not ON CONFLICT (code): the unique constraint is on
        -- (code, company_id), so a bare (code) target raises "no unique or
        -- exclusion constraint matching" and halts startup. Seeded per
        -- company, since the chart of accounts is per company.
        -- internal_group must be set explicitly — it defaults to 'asset'.
        INSERT INTO account_account (code, name, account_type, internal_group, company_id)
        SELECT '7900', 'Foreign Exchange Gain/Loss', 'expense', 'expense', c.id
          FROM res_company c
         WHERE NOT EXISTS (
             SELECT 1 FROM account_account a
              WHERE a.code = '7900' AND a.company_id = c.id
         );
    )"});

    // ── 902 — per-document precision override + rate snapshot ─
    runner.registerMigration({902, "money_document_columns", R"(
        -- NULL = use decimal_precision['Account'] (docs/048 §2.2)
        ALTER TABLE account_move
            ADD COLUMN IF NOT EXISTS line_precision INTEGER NULL
                CHECK (line_precision IS NULL OR line_precision BETWEEN 0 AND 6);
        ALTER TABLE sale_order
            ADD COLUMN IF NOT EXISTS line_precision INTEGER NULL
                CHECK (line_precision IS NULL OR line_precision BETWEEN 0 AND 6);
        ALTER TABLE purchase_order
            ADD COLUMN IF NOT EXISTS line_precision INTEGER NULL
                CHECK (line_precision IS NULL OR line_precision BETWEEN 0 AND 6);

        -- The rate a document was booked at. Without this, historical
        -- documents silently change value when today's rate is edited
        -- (docs/048 §4.4). Default 1.0 = base currency.
        ALTER TABLE account_move
            ADD COLUMN IF NOT EXISTS currency_rate BIGINT NOT NULL DEFAULT 1000000;
        ALTER TABLE sale_order
            ADD COLUMN IF NOT EXISTS currency_rate BIGINT NOT NULL DEFAULT 1000000;
        ALTER TABLE purchase_order
            ADD COLUMN IF NOT EXISTS currency_rate BIGINT NOT NULL DEFAULT 1000000;

        -- Base-currency mirrors, for the ledger and dashboards (docs/048 §4.5)
        ALTER TABLE account_move
            ADD COLUMN IF NOT EXISTS amount_total_base    BIGINT NOT NULL DEFAULT 0,
            ADD COLUMN IF NOT EXISTS amount_residual_base BIGINT NOT NULL DEFAULT 0;
    )"});

    // ── 910 — account_move_line ───────────────────────────────
    // `balance` is GENERATED ALWAYS AS (debit - credit) STORED, so it must be
    // dropped before its inputs change type, then recreated.
    runner.registerMigration({910, "money_account_move_line", R"(
        ALTER TABLE account_move_line DROP COLUMN IF EXISTS balance;

        ALTER TABLE account_move_line
            ALTER COLUMN debit           TYPE BIGINT USING ROUND(COALESCE(debit,0)           * 1000000)::BIGINT,
            ALTER COLUMN credit          TYPE BIGINT USING ROUND(COALESCE(credit,0)          * 1000000)::BIGINT,
            ALTER COLUMN amount_currency TYPE BIGINT USING ROUND(COALESCE(amount_currency,0) * 1000000)::BIGINT,
            ALTER COLUMN price_unit      TYPE BIGINT USING ROUND(COALESCE(price_unit,0)      * 1000000)::BIGINT;

        ALTER TABLE account_move_line
            ADD COLUMN balance BIGINT GENERATED ALWAYS AS (debit - credit) STORED;
    )"});

    // ── 911 — account_move ────────────────────────────────────
    runner.registerMigration({911, "money_account_move",
        alterCols("account_move", {"amount_untaxed", "amount_tax",
                                   "amount_total", "amount_residual"})});

    // ── 912 — account_payment ─────────────────────────────────
    runner.registerMigration({912, "money_account_payment", R"(
        ALTER TABLE account_payment
            ALTER COLUMN amount TYPE BIGINT USING ROUND(COALESCE(amount,0) * 1000000)::BIGINT;

        -- Settlement in base currency + the rate actually obtained
        -- (docs/048 §4.6: the user enters MYR received; the rate is derived).
        ALTER TABLE account_payment
            ADD COLUMN IF NOT EXISTS amount_base    BIGINT NOT NULL DEFAULT 0,
            ADD COLUMN IF NOT EXISTS currency_rate  BIGINT NOT NULL DEFAULT 1000000;
        UPDATE account_payment SET amount_base = amount WHERE amount_base = 0;
    )"});

    // ── 920 — sale ────────────────────────────────────────────
    runner.registerMigration({920, "money_sale_order",
        alterCols("sale_order", {"amount_untaxed", "amount_tax", "amount_total"})});

    runner.registerMigration({921, "money_sale_order_line",
        alterCols("sale_order_line", {"price_unit", "product_uom_qty", "discount",
                                      "price_subtotal", "price_tax", "price_total",
                                      "qty_delivered", "qty_invoiced"})});

    // ── 930 — purchase ────────────────────────────────────────
    runner.registerMigration({930, "money_purchase_order",
        alterCols("purchase_order", {"amount_untaxed", "amount_tax", "amount_total"})});

    runner.registerMigration({931, "money_purchase_order_line",
        alterCols("purchase_order_line", {"price_unit", "product_qty", "discount",
                                          "price_subtotal", "price_tax", "price_total",
                                          "qty_received", "qty_invoiced"})});

    // ── 940 — stock ───────────────────────────────────────────
    runner.registerMigration({940, "money_stock_move",
        alterCols("stock_move", {"product_uom_qty", "quantity"})});

    // ── 950 — product ─────────────────────────────────────────
    runner.registerMigration({950, "money_product",
        alterCols("product_product", {"list_price", "standard_price"})});

    // ── 960 — mrp ─────────────────────────────────────────────
    runner.registerMigration({960, "money_mrp_bom_line",
        alterCols("mrp_bom_line", {"product_qty"})});

    // ── 970 — portal ──────────────────────────────────────────
    // Customer-specific pricing, created by PortalModule::ensureSchema_.
    // Missed in the first pass because it is not declared through a
    // FieldRegistry — only reachable by grepping raw SQL.
    runner.registerMigration({970, "money_partner_rental_price", R"(
        ALTER TABLE partner_rental_price
            ALTER COLUMN price_unit TYPE BIGINT
            USING ROUND(COALESCE(price_unit, 0) * 1000000)::BIGINT;
    )"});

    // ── 971–972 — columns missed by the first pass ────────────
    // Found by scripts/verify_ledger_integrity.sql check 6, which lists any
    // money column still NUMERIC. Both were marked scaled in their
    // FieldRegistry but never migrated — so the write path would have
    // multiplied by 1e6 into a NUMERIC column, and the
    // `price_unit * quantity / 1000000` SQL in portal/report assumed a
    // micro-unit quantity that was not there. New versions rather than
    // amendments to 910/960 because those have already been applied.
    runner.registerMigration({971, "money_account_move_line_quantity",
        alterCols("account_move_line", {"quantity"})});

    // mrp_bom carries its own product_qty alongside mrp_bom_line's. Migrating
    // both keeps header and line consistent — leaving one NUMERIC and the
    // other BIGINT is the kind of asymmetry that produces a silent
    // millionfold error later.
    runner.registerMigration({972, "money_mrp_bom_qty",
        alterCols("mrp_bom", {"product_qty"})});

    // ── 980 — ir.sequence (P4) ────────────────────────────────
    // Replaces the raw PG sequences created inline in ensureSchema_().
    // Seeded number_next is taken from the CURRENT value of each old
    // sequence, so numbering continues rather than restarting at 1 and
    // colliding with existing documents.
    runner.registerMigration({980, "create_ir_sequence", R"(
        CREATE TABLE IF NOT EXISTS ir_sequence (
            id                SERIAL  PRIMARY KEY,
            code              VARCHAR NOT NULL,
            name              VARCHAR NOT NULL,
            prefix            VARCHAR NOT NULL DEFAULT '',
            suffix            VARCHAR NOT NULL DEFAULT '',
            padding           INTEGER NOT NULL DEFAULT 5 CHECK (padding BETWEEN 0 AND 12),
            number_next       BIGINT  NOT NULL DEFAULT 1 CHECK (number_next > 0),
            number_increment  INTEGER NOT NULL DEFAULT 1 CHECK (number_increment > 0),
            -- never | yearly | monthly. Rollover is detected by comparing
            -- last_reset_period at allocation time, so a restart cannot be
            -- missed because the server was down at midnight.
            reset_policy      VARCHAR NOT NULL DEFAULT 'never'
                              CHECK (reset_policy IN ('never','yearly','monthly')),
            last_reset_period VARCHAR NOT NULL DEFAULT '',
            company_id        INTEGER REFERENCES res_company(id) ON DELETE CASCADE,
            active            BOOLEAN NOT NULL DEFAULT TRUE,
            create_date       TIMESTAMP DEFAULT now(),
            write_date        TIMESTAMP DEFAULT now()
        );
        -- One sequence per (code, company). A partial index because NULL
        -- company_id means "all companies" and NULLs do not compare equal
        -- in a plain UNIQUE constraint.
        CREATE UNIQUE INDEX IF NOT EXISTS ir_sequence_code_company_idx
            ON ir_sequence (code, company_id) WHERE company_id IS NOT NULL;
        CREATE UNIQUE INDEX IF NOT EXISTS ir_sequence_code_global_idx
            ON ir_sequence (code) WHERE company_id IS NULL;

        INSERT INTO ir_sequence (code, name, prefix, padding, reset_policy, number_next)
        SELECT v.code, v.name, v.prefix, v.padding, v.reset_policy, v.start
          FROM (VALUES
                ('sale.order',     'Sales Order',     'SO/%(year)s/',     4, 'yearly',
                 COALESCE((SELECT last_value + 1 FROM sale_order_seq), 1)),
                ('purchase.order', 'Purchase Order',  'PO/%(year)s/',     4, 'yearly',
                 COALESCE((SELECT last_value + 1 FROM purchase_order_seq), 1)),
                ('stock.picking.in',  'Receipts',     'WH/IN/%(year)s/',  4, 'yearly',
                 COALESCE((SELECT last_value + 1 FROM stock_in_seq), 1)),
                ('stock.picking.out', 'Deliveries',   'WH/OUT/%(year)s/', 4, 'yearly',
                 COALESCE((SELECT last_value + 1 FROM stock_out_seq), 1)),
                ('stock.picking.int', 'Internal',     'WH/INT/%(year)s/', 4, 'yearly',
                 COALESCE((SELECT last_value + 1 FROM stock_int_seq), 1))
               ) AS v(code, name, prefix, padding, reset_policy, start)
         WHERE NOT EXISTS (SELECT 1 FROM ir_sequence s WHERE s.code = v.code);

        -- Invoice numbering is per journal, so the code carries the journal
        -- code (e.g. 'account.move.INV'). Seeded lazily on first use by
        -- AccountModule rather than guessed here, because the set of journals
        -- is user-defined.
    )"});

    // ── 990 — ir.cron (P5) ────────────────────────────────────
    runner.registerMigration({990, "create_ir_cron", R"(
        CREATE TABLE IF NOT EXISTS ir_cron (
            id               SERIAL   PRIMARY KEY,
            code             VARCHAR  NOT NULL UNIQUE,
            name             VARCHAR  NOT NULL,
            interval_minutes INTEGER  NOT NULL DEFAULT 60 CHECK (interval_minutes > 0),
            -- Persisted so a job that came due while the server was down runs
            -- at startup instead of being silently skipped.
            next_run         TIMESTAMP NOT NULL DEFAULT now(),
            last_run         TIMESTAMP,
            active           BOOLEAN  NOT NULL DEFAULT TRUE,
            -- Failures back off exponentially but never stop being retried; a
            -- silently disabled billing job is worse than a noisy one.
            failure_count    INTEGER  NOT NULL DEFAULT 0,
            last_error       TEXT,
            create_date      TIMESTAMP DEFAULT now(),
            write_date       TIMESTAMP DEFAULT now()
        );
        CREATE INDEX IF NOT EXISTS ir_cron_due_idx
            ON ir_cron (next_run) WHERE active;

        -- Seeded inactive where the handler does not exist yet, so the
        -- scheduler does not log "no handler" every tick until the rental
        -- module lands.
        INSERT INTO ir_cron (code, name, interval_minutes, active)
        SELECT v.code, v.name, v.mins, v.active
          FROM (VALUES
                ('session.gc',      'Session cleanup',            60,  TRUE),
                ('rental.billing',  'Generate rental invoices',   1440, FALSE),
                ('rental.expenses', 'Generate recurring expenses',1440, FALSE)
               ) AS v(code, name, mins, active)
         WHERE NOT EXISTS (SELECT 1 FROM ir_cron c WHERE c.code = v.code);
    )"});

    // ── 1000 — invoice tax (P3) ───────────────────────────────
    // Invoice lines had no tax association at all: account_move_line carried
    // tax_line_id (for a line that IS a tax) but nothing recorded which taxes
    // a PRODUCT line is subject to, so amount_tax on the header could only
    // ever be copied from a sale order or typed by hand.
    runner.registerMigration({1000, "invoice_tax_support", R"(
        ALTER TABLE account_move_line
            ADD COLUMN IF NOT EXISTS tax_ids_json TEXT NOT NULL DEFAULT '[]';

        -- Where a tax posts. Without this the generated tax line has no
        -- account and the journal entry cannot balance.
        ALTER TABLE account_tax
            ADD COLUMN IF NOT EXISTS account_id INTEGER REFERENCES account_account(id);

        -- Tax payable (a liability: tax collected on behalf of the authority).
        -- Per company, and constraint-agnostic for the same reason as 7900.
        INSERT INTO account_account (code, name, account_type, internal_group, company_id)
        SELECT '2200', 'Tax Payable', 'liability_current', 'liability', c.id
          FROM res_company c
         WHERE NOT EXISTS (
             SELECT 1 FROM account_account a
              WHERE a.code = '2200' AND a.company_id = c.id);

        UPDATE account_tax t
           SET account_id = (SELECT a.id FROM account_account a
                              WHERE a.code = '2200' AND a.company_id = t.company_id
                              LIMIT 1)
         WHERE t.account_id IS NULL;
    )"});

    // ── 1010 — payment allocation (P1) ────────────────────────
    // What existed: one payment settled one invoice by decrementing a scalar
    // amount_residual. That cannot express a payment covering several
    // invoices (one transfer for three lockers — the normal rental case), an
    // unallocated credit (paying two months up front, before an invoice
    // exists), or reversing a misapplied payment.
    runner.registerMigration({1010, "payment_allocation", R"(
        CREATE TABLE IF NOT EXISTS account_partial_reconcile (
            id           SERIAL   PRIMARY KEY,
            payment_id   INTEGER  NOT NULL REFERENCES account_payment(id) ON DELETE CASCADE,
            move_id      INTEGER  NOT NULL REFERENCES account_move(id)    ON DELETE CASCADE,
            -- Amount applied, in the INVOICE's currency (micro-units).
            amount       BIGINT   NOT NULL CHECK (amount <> 0),
            -- Same amount in base currency, at the settlement rate. The
            -- difference against the invoice's own booked base value is the
            -- realised FX result (docs/048 §4.6).
            amount_base  BIGINT   NOT NULL DEFAULT 0,
            fx_diff      BIGINT   NOT NULL DEFAULT 0,
            date         DATE     NOT NULL DEFAULT CURRENT_DATE,
            company_id   INTEGER  REFERENCES res_company(id),
            create_date  TIMESTAMP DEFAULT now(),
            write_date   TIMESTAMP DEFAULT now()
        );
        CREATE INDEX IF NOT EXISTS apr_payment_idx ON account_partial_reconcile (payment_id);
        CREATE INDEX IF NOT EXISTS apr_move_idx    ON account_partial_reconcile (move_id);

        -- A payment's unallocated remainder is a customer credit. Derived
        -- rather than stored so it cannot drift from the allocations.
        CREATE OR REPLACE VIEW account_payment_unallocated AS
            SELECT p.id                AS payment_id,
                   p.partner_id,
                   p.company_id,
                   p.amount            AS amount_total,
                   COALESCE(SUM(r.amount), 0) AS amount_allocated,
                   p.amount - COALESCE(SUM(r.amount), 0) AS amount_unallocated
              FROM account_payment p
              LEFT JOIN account_partial_reconcile r ON r.payment_id = p.id
             WHERE p.state = 'posted'
             GROUP BY p.id, p.partner_id, p.company_id, p.amount;
    )"});
}

} // namespace odoo::core

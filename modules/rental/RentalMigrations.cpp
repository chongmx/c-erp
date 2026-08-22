// ============================================================
// modules/rental/RentalMigrations.cpp
// ============================================================
#include "RentalMigrations.hpp"
#include "MigrationRunner.hpp"

namespace odoo::modules::rental {

using odoo::infrastructure::MigrationRunner;

void registerRentalMigrations(MigrationRunner& runner) {

    // --------------------------------------------------------
    // 800 — rental.unit.type
    // --------------------------------------------------------
    runner.registerMigration({800, "rental_unit_type", R"SQL(
        CREATE TABLE IF NOT EXISTS rental_unit_type (
            id              SERIAL PRIMARY KEY,
            name            TEXT    NOT NULL,
            code            TEXT    NOT NULL,
            -- P2: money as BIGINT micro-units, never NUMERIC.
            default_rate    BIGINT  NOT NULL DEFAULT 0,
            default_period  TEXT    NOT NULL DEFAULT 'monthly',
            tax_ids_json    TEXT    NOT NULL DEFAULT '[]',
            area_sqm        NUMERIC(12,4) DEFAULT 0,   -- physical, not money
            volume_m3       NUMERIC(12,4) DEFAULT 0,   -- physical, not money
            company_id      INTEGER NOT NULL DEFAULT 1,
            active          BOOLEAN NOT NULL DEFAULT TRUE,
            create_date     TIMESTAMP NOT NULL DEFAULT now(),
            write_date      TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT rental_unit_type_code_uniq UNIQUE (code, company_id),
            CONSTRAINT rental_unit_type_period_chk
                CHECK (default_period IN ('monthly','quarterly','yearly'))
        );
    )SQL"});

    // --------------------------------------------------------
    // 801 — rental.unit
    //
    // `state` is DERIVED from active contract lines and recomputed, never
    // hand-edited — except maintenance/retired, which are operator facts
    // rather than consequences. A hand-editable state that can disagree
    // with the contract lines is the classic double-let bug.
    // --------------------------------------------------------
    runner.registerMigration({801, "rental_unit", R"SQL(
        CREATE TABLE IF NOT EXISTS rental_unit (
            id              SERIAL PRIMARY KEY,
            code            TEXT    NOT NULL,
            name            TEXT    NOT NULL DEFAULT '',
            type_id         INTEGER REFERENCES rental_unit_type(id),
            site            TEXT    NOT NULL DEFAULT '',
            zone            TEXT    NOT NULL DEFAULT '',
            floor           TEXT    NOT NULL DEFAULT '',
            area_sqm        NUMERIC(12,4) DEFAULT 0,
            volume_m3       NUMERIC(12,4) DEFAULT 0,
            state           TEXT    NOT NULL DEFAULT 'available',
            location_id     INTEGER,
            notes           TEXT    NOT NULL DEFAULT '',
            company_id      INTEGER NOT NULL DEFAULT 1,
            active          BOOLEAN NOT NULL DEFAULT TRUE,
            create_date     TIMESTAMP NOT NULL DEFAULT now(),
            write_date      TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT rental_unit_code_uniq UNIQUE (code, company_id),
            CONSTRAINT rental_unit_state_chk
                CHECK (state IN ('available','reserved','occupied','maintenance','retired'))
        );
        CREATE INDEX IF NOT EXISTS rental_unit_state_idx ON rental_unit(state);
        CREATE INDEX IF NOT EXISTS rental_unit_type_idx  ON rental_unit(type_id);
    )SQL"});

    // --------------------------------------------------------
    // 802 — rental.contract
    //
    // No contract-level billing date. Billing dates live on the LINES,
    // because a customer renting three units from three different dates
    // has three different due dates — see 803.
    //
    // billing_lead_days: invoices are raised IN ADVANCE of the period
    // (docs/054 §7). Without a lead time, "bill in advance" quietly
    // becomes "bill on day one of the period", which gives the tenant no
    // time to pay before occupying.
    // --------------------------------------------------------
    runner.registerMigration({802, "rental_contract", R"SQL(
        CREATE TABLE IF NOT EXISTS rental_contract (
            id                SERIAL PRIMARY KEY,
            name              TEXT    NOT NULL,
            partner_id        INTEGER NOT NULL,
            state             TEXT    NOT NULL DEFAULT 'draft',
            date_start        DATE,
            date_cancelled    DATE,
            billing_period    TEXT    NOT NULL DEFAULT 'monthly',
            billing_lead_days INTEGER NOT NULL DEFAULT 7,
            payment_term_id   INTEGER,
            deposit_amount    BIGINT  NOT NULL DEFAULT 0,
            deposit_state     TEXT    NOT NULL DEFAULT 'none',
            currency_id       INTEGER,
            journal_id        INTEGER,
            notes             TEXT    NOT NULL DEFAULT '',
            company_id        INTEGER NOT NULL DEFAULT 1,
            active            BOOLEAN NOT NULL DEFAULT TRUE,
            create_date       TIMESTAMP NOT NULL DEFAULT now(),
            write_date        TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT rental_contract_name_uniq UNIQUE (name, company_id),
            CONSTRAINT rental_contract_state_chk
                CHECK (state IN ('draft','active','cancelled','closed')),
            CONSTRAINT rental_contract_period_chk
                CHECK (billing_period IN ('monthly','quarterly','yearly')),
            -- Deposits are never auto-applied to rent and the refund is a
            -- choice, not a default (docs/054 §7).
            CONSTRAINT rental_contract_deposit_chk
                CHECK (deposit_state IN ('none','held','refunded','forfeited'))
        );
        CREATE INDEX IF NOT EXISTS rental_contract_partner_idx ON rental_contract(partner_id);
        CREATE INDEX IF NOT EXISTS rental_contract_state_idx   ON rental_contract(state);
    )SQL"});

    // --------------------------------------------------------
    // 803 — rental.contract.line
    //
    // THIS is where the per-unit dates live. Each line carries its own
    // date_start and its own next_invoice_date, so lines that start on
    // different days bill on different days with no configuration — it
    // falls out of the data.
    //
    // next_invoice_date is the date the INVOICE is raised; period_start
    // is the date the tenant's period begins. Billing in advance means
    // next_invoice_date = period_start - billing_lead_days.
    // --------------------------------------------------------
    runner.registerMigration({803, "rental_contract_line", R"SQL(
        CREATE TABLE IF NOT EXISTS rental_contract_line (
            id                 SERIAL PRIMARY KEY,
            contract_id        INTEGER NOT NULL REFERENCES rental_contract(id) ON DELETE CASCADE,
            unit_id            INTEGER REFERENCES rental_unit(id),
            date_start         DATE    NOT NULL,
            date_end           DATE,                   -- NULL = open-ended
            unit_price         BIGINT  NOT NULL DEFAULT 0,
            discount_pct       BIGINT  NOT NULL DEFAULT 0,
            tax_ids_json       TEXT    NOT NULL DEFAULT '[]',
            billing_anchor_day INTEGER NOT NULL DEFAULT 1,
            next_period_start  DATE,                   -- start of the period to bill next
            invoiced_through   DATE,                   -- last period_end already invoiced
            proration_policy   TEXT    NOT NULL DEFAULT 'full_period',
            state              TEXT    NOT NULL DEFAULT 'pending',
            company_id         INTEGER NOT NULL DEFAULT 1,
            create_date        TIMESTAMP NOT NULL DEFAULT now(),
            write_date         TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT rental_contract_line_state_chk
                CHECK (state IN ('pending','active','ended','cancelled')),
            CONSTRAINT rental_contract_line_anchor_chk
                CHECK (billing_anchor_day BETWEEN 1 AND 31),
            CONSTRAINT rental_contract_line_proration_chk
                CHECK (proration_policy IN ('full_period','prorate_days','start_next_cycle'))
        );
        CREATE INDEX IF NOT EXISTS rental_cl_contract_idx ON rental_contract_line(contract_id);
        CREATE INDEX IF NOT EXISTS rental_cl_unit_idx     ON rental_contract_line(unit_id);
        -- The billing run's driving query: active lines due to be billed.
        CREATE INDEX IF NOT EXISTS rental_cl_due_idx
            ON rental_contract_line(state, next_period_start);

        -- A unit can be held by at most ONE live line at a time. This is the
        -- double-let guard at the database level: deriving rental_unit.state
        -- is what keeps the UI honest, but only a constraint makes the race
        -- impossible when two operators let the same locker concurrently.
        CREATE UNIQUE INDEX IF NOT EXISTS rental_cl_unit_live_uniq
            ON rental_contract_line(unit_id)
            WHERE state IN ('pending','active') AND unit_id IS NOT NULL;
    )SQL"});

    // --------------------------------------------------------
    // 804 — rental.invoice.link
    //
    // The most important constraint in the module.
    //
    // UNIQUE (contract_line_id, period_start) makes double-billing
    // impossible even if the cron fires twice, the process restarts
    // mid-run, or someone clicks "Generate invoices now" twice. ir.cron is
    // at-least-once by design, so the SCHEDULER is not the guard — this is.
    //
    // It ships with the table rather than as a follow-up: added before any
    // data exists it is free, added afterwards it is a migration that can
    // fail on real rows.
    // --------------------------------------------------------
    runner.registerMigration({804, "rental_invoice_link", R"SQL(
        CREATE TABLE IF NOT EXISTS rental_invoice_link (
            id               SERIAL PRIMARY KEY,
            move_id          INTEGER NOT NULL,
            contract_id      INTEGER REFERENCES rental_contract(id) ON DELETE CASCADE,
            contract_line_id INTEGER NOT NULL
                             REFERENCES rental_contract_line(id) ON DELETE CASCADE,
            period_start     DATE    NOT NULL,
            period_end       DATE    NOT NULL,
            amount           BIGINT  NOT NULL DEFAULT 0,
            company_id       INTEGER NOT NULL DEFAULT 1,
            create_date      TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT rental_invoice_link_uniq UNIQUE (contract_line_id, period_start)
        );
        CREATE INDEX IF NOT EXISTS rental_il_move_idx     ON rental_invoice_link(move_id);
        CREATE INDEX IF NOT EXISTS rental_il_contract_idx ON rental_invoice_link(contract_id);
    )SQL"});

    // --------------------------------------------------------
    // 805 — rental.expense.category
    // --------------------------------------------------------
    runner.registerMigration({805, "rental_expense_category", R"SQL(
        CREATE TABLE IF NOT EXISTS rental_expense_category (
            id           SERIAL PRIMARY KEY,
            name         TEXT    NOT NULL,
            account_id   INTEGER,
            is_operating BOOLEAN NOT NULL DEFAULT TRUE,
            company_id   INTEGER NOT NULL DEFAULT 1,
            active       BOOLEAN NOT NULL DEFAULT TRUE,
            create_date  TIMESTAMP NOT NULL DEFAULT now(),
            write_date   TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT rental_expense_category_uniq UNIQUE (name, company_id)
        );
    )SQL"});

    // --------------------------------------------------------
    // 806 — rental.expense
    //
    // A recurring expense is a TEMPLATE row (is_recurring = TRUE) that the
    // cron clones into dated children (recurrence_parent_id set). Same
    // idempotency discipline as billing, for the same reason and with the
    // same shape of constraint.
    //
    // attachment_id was carried here as a placeholder for receipt upload. That
    // was based on a wrong premise — ir.attachment did exist, and it is
    // polymorphic — so migration 814 drops the column again. Receipts attach
    // through (res_model, res_id) like everything else (docs/092).
    // --------------------------------------------------------
    runner.registerMigration({806, "rental_expense", R"SQL(
        CREATE TABLE IF NOT EXISTS rental_expense (
            id                    SERIAL PRIMARY KEY,
            date                  DATE    NOT NULL,
            name                  TEXT    NOT NULL,
            category_id           INTEGER REFERENCES rental_expense_category(id),
            amount                BIGINT  NOT NULL DEFAULT 0,
            partner_id            INTEGER,
            unit_id               INTEGER REFERENCES rental_unit(id),
            contract_id           INTEGER REFERENCES rental_contract(id),
            account_id            INTEGER,
            state                 TEXT    NOT NULL DEFAULT 'draft',
            move_id               INTEGER,
            attachment_id         INTEGER,
            is_recurring          BOOLEAN NOT NULL DEFAULT FALSE,
            recurrence_interval   TEXT,
            recurrence_next_date  DATE,
            recurrence_end_date   DATE,
            recurrence_parent_id  INTEGER REFERENCES rental_expense(id) ON DELETE CASCADE,
            company_id            INTEGER NOT NULL DEFAULT 1,
            create_date           TIMESTAMP NOT NULL DEFAULT now(),
            write_date            TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT rental_expense_state_chk
                CHECK (state IN ('draft','posted','cancelled')),
            CONSTRAINT rental_expense_interval_chk
                CHECK (recurrence_interval IS NULL
                       OR recurrence_interval IN ('monthly','quarterly','yearly'))
        );
        CREATE INDEX IF NOT EXISTS rental_exp_date_idx     ON rental_expense(date);
        CREATE INDEX IF NOT EXISTS rental_exp_category_idx ON rental_expense(category_id);
        CREATE INDEX IF NOT EXISTS rental_exp_unit_idx     ON rental_expense(unit_id);
        CREATE INDEX IF NOT EXISTS rental_exp_due_idx
            ON rental_expense(is_recurring, recurrence_next_date);

        -- The recurring-expense equivalent of rental_invoice_link's guard:
        -- one child per template per date, so a double cron run cannot
        -- double-post the rent or the electricity bill.
        CREATE UNIQUE INDEX IF NOT EXISTS rental_expense_recurrence_uniq
            ON rental_expense(recurrence_parent_id, date)
            WHERE recurrence_parent_id IS NOT NULL;
    )SQL"});

    // --------------------------------------------------------
    // 807 — rental.event
    //
    // The domain event log, deliberately SEPARATE from audit_log.
    // audit_log is CRUD forensics (who wrote which row); this is business
    // narrative (contract activated, unit released, invoice generated).
    // Conflating them produces a log that is bad at both.
    //
    // Built in phase 2 rather than last, because every phase after this
    // one emits into it. Retrofitting emit() calls into finished code is
    // where events get missed.
    // --------------------------------------------------------
    runner.registerMigration({807, "rental_event", R"SQL(
        CREATE TABLE IF NOT EXISTS rental_event (
            id          SERIAL PRIMARY KEY,
            occurred_at TIMESTAMP NOT NULL DEFAULT now(),
            event_type  TEXT    NOT NULL,
            contract_id INTEGER,
            line_id     INTEGER,
            unit_id     INTEGER,
            partner_id  INTEGER,
            user_id     INTEGER,
            summary     TEXT    NOT NULL DEFAULT '',
            detail      JSONB,
            ref_model   TEXT,
            ref_id      INTEGER,
            company_id  INTEGER NOT NULL DEFAULT 1,
            create_date TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE INDEX IF NOT EXISTS rental_event_time_idx     ON rental_event(occurred_at DESC);
        CREATE INDEX IF NOT EXISTS rental_event_contract_idx ON rental_event(contract_id);
        CREATE INDEX IF NOT EXISTS rental_event_type_idx     ON rental_event(event_type);
    )SQL"});

    // --------------------------------------------------------
    // 808 — seed data
    //
    // Rates are micro-units: 120.00 -> 120000000.
    // --------------------------------------------------------
    runner.registerMigration({808, "rental_seed_reference_data", R"SQL(
        INSERT INTO rental_unit_type (name, code, default_rate, default_period, company_id)
        VALUES ('Small Locker',  'SL',  120000000, 'monthly', 1),
               ('Medium Locker', 'ML',  220000000, 'monthly', 1),
               ('Large Locker',  'LL',  350000000, 'monthly', 1),
               ('Storage Room',  'RM',  450000000, 'monthly', 1),
               ('Pallet Space',  'PS',   80000000, 'monthly', 1)
        ON CONFLICT (code, company_id) DO NOTHING;

        INSERT INTO rental_expense_category (name, is_operating, company_id)
        VALUES ('Utilities',    TRUE,  1),
               ('Maintenance',  TRUE,  1),
               ('Security',     TRUE,  1),
               ('Insurance',    TRUE,  1),
               ('Cleaning',     TRUE,  1),
               ('Rent / Lease', TRUE,  1),
               ('Capital',      FALSE, 1)
        ON CONFLICT (name, company_id) DO NOTHING;
    )SQL"});

    // --------------------------------------------------------
    // 809 — contract numbering
    //
    // ir.sequence, never COUNT(*)+1. P4 removed that race from invoice
    // numbering and P1 removed it from payment entry naming; it is not
    // being reintroduced here.
    // --------------------------------------------------------
    runner.registerMigration({809, "rental_ir_sequence", R"SQL(
        INSERT INTO ir_sequence (code, name, prefix, padding, reset_policy)
        VALUES ('rental.contract', 'Rental Contract', 'RENT/%(year)s/', 4, 'yearly')
        ON CONFLICT (code) WHERE company_id IS NULL DO NOTHING;
    )SQL"});

    // --------------------------------------------------------
    // 810 — scheduled jobs
    //
    // The rows already exist: migration 990 (P5) seeded 'rental.billing'
    // and 'rental.expenses' as INACTIVE placeholders, specifically so the
    // scheduler would not log "no handler" every tick until this module
    // landed. This migration therefore reconciles rather than inserts.
    //
    // They stay INACTIVE. The handlers do not exist yet — billing arrives
    // in phase 5, recurring expenses in phase 7 — and activating a job
    // with no handler reintroduces exactly the noise 990 was avoiding.
    // Each phase flips its own job on when it can actually service it.
    //
    // Two mistakes were made here and caught by verify_rental_schema.sh:
    //
    //   1. A THIRD code ('rental.expense.recurring') was invented rather
    //      than reusing 990's 'rental.expenses', producing two jobs for
    //      one purpose.
    //   2. `ON CONFLICT DO NOTHING` with `active TRUE` silently kept the
    //      existing inactive row — so had the handler existed, billing
    //      still would never have run, with nothing anywhere reporting a
    //      problem. DO NOTHING hides exactly the case worth knowing about.
    //
    // ir.cron is at-least-once by design, which is why 804 and 806 carry
    // UNIQUE guards rather than trusting the schedule.
    // --------------------------------------------------------
    runner.registerMigration({810, "rental_ir_cron", R"SQL(
        -- Idempotent and explicit: create only what is genuinely missing,
        -- and never flip `active` from under a running deployment.
        INSERT INTO ir_cron (code, name, interval_minutes, active)
        SELECT v.code, v.name, v.mins, FALSE
          FROM (VALUES
                ('rental.billing',  'Generate rental invoices',    1440),
                ('rental.expenses', 'Generate recurring expenses', 1440)
               ) AS v(code, name, mins)
         WHERE NOT EXISTS (SELECT 1 FROM ir_cron c WHERE c.code = v.code);

        -- Remove the duplicate introduced by the first cut of this
        -- migration on any database that ran it.
        DELETE FROM ir_cron WHERE code = 'rental.expense.recurring';
    )SQL"});

    // --------------------------------------------------------
    // 811 — unit state derivation (docs/054 phase 3)
    //
    // rental_unit.state is DERIVED from the contract lines. The question
    // is where to derive it, and this is deliberately a TRIGGER rather
    // than C++.
    //
    // The reason: derived state must not depend on which code path wrote
    // the line. Lines are written by the contract ViewModel, by the
    // billing engine, by cancellation, and by repair SQL — and "remember
    // to call deriveState() from every one of those" is exactly the class
    // of bug this project keeps finding. The tax_ids_json field that was
    // never registered, and the manual log() calls left behind in
    // GenericViewModel, are both the same shape: a step that had to be
    // remembered, and wasn't. A trigger cannot be forgotten.
    //
    // Split of responsibilities:
    //   trigger (here)  -> state, because it must always be right
    //   C++ (RentalUnits) -> events, because they need the acting user,
    //                        which the database does not have
    //
    // maintenance and retired are OPERATOR facts, not consequences of a
    // contract, so the trigger never overwrites them. Everything else is
    // computed. The partial unique index from 803 guarantees at most one
    // live line per unit, so there is no ambiguity to resolve here.
    // --------------------------------------------------------
    runner.registerMigration({811, "rental_unit_state_derivation", R"SQL(
        CREATE OR REPLACE FUNCTION rental_unit_derive_state(p_unit_id INTEGER)
        RETURNS VOID AS $$
        DECLARE
            v_current TEXT;
            v_new     TEXT;
        BEGIN
            IF p_unit_id IS NULL THEN RETURN; END IF;

            SELECT state INTO v_current FROM rental_unit WHERE id = p_unit_id;
            IF v_current IS NULL THEN RETURN; END IF;

            -- Operator facts win. A unit taken out of service stays out of
            -- service even if a contract line still points at it.
            IF v_current IN ('maintenance', 'retired') THEN RETURN; END IF;

            SELECT CASE
                     WHEN bool_or(state = 'active')  THEN 'occupied'
                     WHEN bool_or(state = 'pending') THEN 'reserved'
                     ELSE 'available'
                   END
              INTO v_new
              FROM rental_contract_line
             WHERE unit_id = p_unit_id
               AND state IN ('pending', 'active');

            v_new := COALESCE(v_new, 'available');

            -- Only write on an actual change, so this does not churn
            -- write_date on every unrelated line edit.
            IF v_new IS DISTINCT FROM v_current THEN
                UPDATE rental_unit
                   SET state = v_new, write_date = now()
                 WHERE id = p_unit_id;
            END IF;
        END;
        $$ LANGUAGE plpgsql;

        CREATE OR REPLACE FUNCTION rental_contract_line_state_trg()
        RETURNS TRIGGER AS $$
        BEGIN
            -- Both sides: moving a line to a different unit must release
            -- the old one as well as claim the new one.
            IF TG_OP IN ('UPDATE', 'DELETE') THEN
                PERFORM rental_unit_derive_state(OLD.unit_id);
            END IF;
            IF TG_OP IN ('INSERT', 'UPDATE') THEN
                PERFORM rental_unit_derive_state(NEW.unit_id);
            END IF;
            RETURN NULL;   -- AFTER trigger; return value is ignored
        END;
        $$ LANGUAGE plpgsql;

        DROP TRIGGER IF EXISTS rental_contract_line_state ON rental_contract_line;
        CREATE TRIGGER rental_contract_line_state
            AFTER INSERT OR UPDATE OR DELETE ON rental_contract_line
            FOR EACH ROW EXECUTE FUNCTION rental_contract_line_state_trg();

        -- Reconcile anything that predates the trigger.
        SELECT rental_unit_derive_state(id) FROM rental_unit;
    )SQL"});

    // --------------------------------------------------------
    // 812 — recurring billing (docs/057 §1, revised)
    //
    // The business rents pay-to-use monthly with no contract. A contract
    // is OPTIONAL and its purpose is to switch on recurring billing:
    //
    //   walk-in      invoice raised by hand when they pay
    //   on contract  the cron bills every period until cancellation
    //
    // So contract_id becomes nullable, the customer moves onto the line
    // (a walk-in has no contract to carry it), and billing_mode says
    // explicitly which kind this is.
    //
    // billing_mode is a column rather than "recurring if
    // next_period_start IS NOT NULL". The implicit version works and
    // produces exactly one bug — "why is this customer not being
    // invoiced?" — answerable only by reading the cron's SQL.
    // --------------------------------------------------------
    runner.registerMigration({812, "rental_recurring_billing", R"SQL(
        ALTER TABLE rental_contract_line
            ADD COLUMN IF NOT EXISTS partner_id     INTEGER,
            ADD COLUMN IF NOT EXISTS billing_mode   TEXT    NOT NULL DEFAULT 'manual',
            ADD COLUMN IF NOT EXISTS billing_months INTEGER NOT NULL DEFAULT 1;

        -- Backfill from the contract before the NOT NULL goes on, so an
        -- existing row cannot block the migration.
        UPDATE rental_contract_line l
           SET partner_id = c.partner_id
          FROM rental_contract c
         WHERE c.id = l.contract_id AND l.partner_id IS NULL;

        DELETE FROM rental_contract_line WHERE partner_id IS NULL;

        ALTER TABLE rental_contract_line
            ALTER COLUMN partner_id  SET NOT NULL,
            ALTER COLUMN contract_id DROP NOT NULL;

        ALTER TABLE rental_contract_line
            DROP CONSTRAINT IF EXISTS rental_cl_billing_mode_chk;
        ALTER TABLE rental_contract_line
            ADD CONSTRAINT rental_cl_billing_mode_chk
                CHECK (billing_mode IN ('manual','recurring'));

        ALTER TABLE rental_contract_line
            DROP CONSTRAINT IF EXISTS rental_cl_billing_months_chk;
        ALTER TABLE rental_contract_line
            ADD CONSTRAINT rental_cl_billing_months_chk
                CHECK (billing_months BETWEEN 1 AND 12);

        -- billing_lead_days lives on the contract, but a walk-in has no
        -- contract. Carrying it on the line makes the row self-sufficient
        -- for billing, which is what the engine's driving query needs.
        ALTER TABLE rental_contract_line
            ADD COLUMN IF NOT EXISTS billing_lead_days INTEGER NOT NULL DEFAULT 7;

        -- The driving query for the billing run.
        DROP INDEX IF EXISTS rental_cl_due_idx;
        CREATE INDEX IF NOT EXISTS rental_cl_due_idx
            ON rental_contract_line(billing_mode, state, next_period_start);
        CREATE INDEX IF NOT EXISTS rental_cl_partner_idx
            ON rental_contract_line(partner_id);

        -- A contracted line must belong to the contract's customer.
        -- Without this a contract could quietly invoice the wrong person.
        CREATE OR REPLACE FUNCTION rental_cl_partner_matches_trg()
        RETURNS TRIGGER AS $$
        DECLARE v_owner INTEGER;
        BEGIN
            IF NEW.contract_id IS NOT NULL THEN
                SELECT partner_id INTO v_owner
                  FROM rental_contract WHERE id = NEW.contract_id;
                IF v_owner IS NOT NULL AND v_owner <> NEW.partner_id THEN
                    RAISE EXCEPTION
                        'rental: line partner (%) does not match contract partner (%)',
                        NEW.partner_id, v_owner;
                END IF;
            END IF;
            RETURN NEW;
        END;
        $$ LANGUAGE plpgsql;

        DROP TRIGGER IF EXISTS rental_cl_partner_matches ON rental_contract_line;
        CREATE TRIGGER rental_cl_partner_matches
            BEFORE INSERT OR UPDATE ON rental_contract_line
            FOR EACH ROW EXECUTE FUNCTION rental_cl_partner_matches_trg();

        -- ----------------------------------------------------------
        -- Period arithmetic, as a function so the engine, the forecast
        -- and the tests all use ONE implementation.
        --
        -- The bug this exists to avoid: `date + interval '1 month'`
        -- DRIFTS. Jan 31 + 1 month is Feb 28, and Feb 28 + 1 month is
        -- Mar 28 — so a tenancy anchored on the 31st silently walks
        -- backwards to the 28th and never returns. Anchoring to the day
        -- of month and clamping to the month length fixes it:
        -- Jan 31 -> Feb 28 -> Mar 31.
        -- ----------------------------------------------------------
        CREATE OR REPLACE FUNCTION rental_next_period(
            p_from   DATE,
            p_anchor INTEGER,
            p_months INTEGER
        ) RETURNS DATE AS $$
        DECLARE
            v_month_start DATE;
            v_last_day    DATE;
            v_anchor      INTEGER;
        BEGIN
            v_anchor := GREATEST(1, LEAST(31, COALESCE(p_anchor, 1)));
            v_month_start := (date_trunc('month', p_from)
                              + (COALESCE(p_months,1) || ' months')::interval)::date;
            v_last_day := (v_month_start + interval '1 month' - interval '1 day')::date;
            RETURN LEAST(v_month_start + (v_anchor - 1), v_last_day);
        END;
        $$ LANGUAGE plpgsql IMMUTABLE;
    )SQL"});

    // --------------------------------------------------------
    // 813 — tie the invoice to its rental origin
    //
    // Odoo links an invoice to the sale order it came from, and this
    // codebase already follows that: account_move carries `invoice_origin`
    // (a registered Char field, so it flows to the API and the portal for
    // free) and `sale_id` (a raw column plus an FK, added by SaleModule
    // itself — SaleModule.cpp:1375).
    //
    // Rental mirrors it exactly rather than inventing a parallel scheme:
    // the module that owns the referenced table adds the column and the
    // FK. Nullable because a WALK-IN has no contract, and the invoice is
    // no less real for that.
    //
    // The line-level link already exists — rental_invoice_link records
    // which tenancy and which period each invoice line covers, which is
    // the "what does this invoice actually cover" answer. This is the
    // header-level convenience that mirrors sale_id.
    // --------------------------------------------------------
    runner.registerMigration({813, "rental_invoice_origin_link", R"SQL(
        ALTER TABLE account_move
            ADD COLUMN IF NOT EXISTS rental_contract_id INTEGER
                REFERENCES rental_contract(id);

        CREATE INDEX IF NOT EXISTS account_move_rental_idx
            ON account_move(rental_contract_id)
            WHERE rental_contract_id IS NOT NULL;
    )SQL"});

    // --------------------------------------------------------
    // 814 — drop rental_expense.attachment_id
    //
    // The column was added in 806 as a placeholder "so the capability can be
    // added later without a schema change", on the belief that there was no
    // ir.attachment model. There was — and it is polymorphic: attachments
    // find their owner through (res_model, res_id), so a dedicated column on
    // one table is not how a receipt gets attached here (docs/091, docs/092).
    //
    // Nothing ever wrote it and nothing reads it: a single-valued column would
    // also have capped a receipt at one file per expense, which is not how
    // expenses arrive. Dropping it removes a field that would otherwise keep
    // inviting exactly the wrong implementation.
    // --------------------------------------------------------
    runner.registerMigration({814, "rental_expense_drop_attachment_id", R"SQL(
        ALTER TABLE rental_expense DROP COLUMN IF EXISTS attachment_id;
    )SQL"});
}

} // namespace odoo::modules::rental

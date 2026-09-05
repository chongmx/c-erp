// ============================================================
// modules/rental/RentalMigrations.cpp
// ============================================================
#include "RentalMigrations.hpp"
#include "MigrationRunner.hpp"

namespace cerp::modules::rental {

using cerp::infrastructure::MigrationRunner;

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
    // the reference ERP links an invoice to the sale order it came from, and this
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
    // --------------------------------------------------------
    // 815 — a billing period that can express what people actually bill
    //
    // The schedule was months-only: billing_mode 'manual'|'recurring' plus
    // billing_months, and rental_next_period() advanced by whole months. So
    // weekly storage, a 10-day locker or a one-off cleaning fee had nowhere to
    // live — you either lied with months=1 or billed by hand forever.
    //
    // Generalised to (interval, unit), which covers every period in one shape:
    //
    //     daily      1 day        quarterly  3 month
    //     weekly     1 week       biannual   6 month
    //     monthly    1 month      yearly     1 year
    //     custom     X <unit>     <- every X days/weeks/months/years
    //
    // and two modes that do not recur at all:
    //
    //     oneoff     bill once, then stop
    //     ondemand   never scheduled; someone raises it when it happens
    //
    // billing_months is KEPT and kept in sync for month-based periods so the
    // existing billing run and any report reading it stay correct.
    // --------------------------------------------------------
    runner.registerMigration({815, "rental_billing_period_units", R"SQL(
        ALTER TABLE rental_contract_line
            ADD COLUMN IF NOT EXISTS billing_interval INTEGER NOT NULL DEFAULT 1;
        ALTER TABLE rental_contract_line
            ADD COLUMN IF NOT EXISTS billing_unit VARCHAR NOT NULL DEFAULT 'month';

        DO $mig$ BEGIN
            IF NOT EXISTS (SELECT 1 FROM pg_constraint
                            WHERE conname = 'rental_line_billing_unit_chk') THEN
                ALTER TABLE rental_contract_line ADD CONSTRAINT rental_line_billing_unit_chk
                    CHECK (billing_unit IN ('day','week','month','year'));
            END IF;
            IF NOT EXISTS (SELECT 1 FROM pg_constraint
                            WHERE conname = 'rental_line_billing_interval_chk') THEN
                ALTER TABLE rental_contract_line ADD CONSTRAINT rental_line_billing_interval_chk
                    CHECK (billing_interval >= 1 AND billing_interval <= 366);
            END IF;
        END $mig$;

        -- billing_mode gains the two non-recurring modes.
        ALTER TABLE rental_contract_line DROP CONSTRAINT IF EXISTS rental_line_billing_mode_chk;
        ALTER TABLE rental_contract_line
            ADD CONSTRAINT rental_line_billing_mode_chk
            CHECK (billing_mode IN ('manual','recurring','oneoff','ondemand'));

        -- Existing rows were months-only; carry them over exactly.
        UPDATE rental_contract_line
           SET billing_unit     = 'month',
               billing_interval = GREATEST(1, COALESCE(billing_months, 1))
         WHERE billing_unit IS NULL OR billing_unit = 'month';

        -- Unit-aware advance. The old 3-argument form is kept so anything still
        -- calling it keeps working; it now delegates.
        CREATE OR REPLACE FUNCTION rental_next_period(
            p_from     DATE,
            p_anchor   INTEGER,
            p_interval INTEGER,
            p_unit     VARCHAR
        ) RETURNS DATE AS $fn$
        DECLARE
            v_n      INTEGER := GREATEST(1, COALESCE(p_interval, 1));
            v_anchor INTEGER;
            v_start  DATE;
            v_last   DATE;
        BEGIN
            IF p_unit = 'day'  THEN RETURN p_from + (v_n || ' days')::interval;  END IF;
            IF p_unit = 'week' THEN RETURN p_from + (v_n * 7 || ' days')::interval; END IF;

            -- Month and year keep the anchor-day behaviour: bill on the 31st in a
            -- 30-day month and you get the 30th, not a skipped period.
            IF p_unit = 'year' THEN v_n := v_n * 12; END IF;
            v_anchor := GREATEST(1, LEAST(31, COALESCE(p_anchor, 1)));
            v_start  := (date_trunc('month', p_from) + (v_n || ' months')::interval)::date;
            v_last   := (v_start + interval '1 month' - interval '1 day')::date;
            RETURN LEAST(v_start + (v_anchor - 1), v_last);
        END $fn$ LANGUAGE plpgsql IMMUTABLE;

        CREATE OR REPLACE FUNCTION rental_next_period(
            p_from DATE, p_anchor INTEGER, p_months INTEGER
        ) RETURNS DATE AS $fn$
            SELECT rental_next_period($1, $2, $3, 'month');
        $fn$ LANGUAGE sql IMMUTABLE;
    )SQL"});

    // --------------------------------------------------------
    // 816 — the CONTRACT carries the billing period
    // (docs/architecture/modules.md, "The billing period")
    //
    // Until now rental_contract.billing_period was decorative: a TEXT column
    // limited to monthly/quarterly/yearly that nothing read. The schedule that
    // actually billed lived on the line. So a user who set a contract to
    // "quarterly" still got monthly invoices, and there was no way at all to
    // ask for weekly, daily, six-monthly, one-off or on-demand.
    //
    // After this migration:
    //
    //   contract.billing_period   the preset the user picks; the nine values
    //                             below cover every cadence asked for
    //   contract.billing_interval \  derived from the preset by trigger, except
    //   contract.billing_unit     /  for 'custom' where the user supplies them
    //
    //   line.billing_interval     NULL now MEANS "inherit from the contract".
    //   line.billing_unit         A line that wants its own cadence still sets
    //                             them; one that says nothing follows the
    //                             contract. That is why the NOT NULL and the
    //                             defaults from 815 are dropped here — with a
    //                             default of 1/'month' there was no way to
    //                             distinguish "monthly, deliberately" from
    //                             "nobody said", and the contract's setting
    //                             could never win.
    //
    // oneoff / ondemand have no interval at all, so they store NULL and the
    // billing run skips them — a period of "never" is not 1 of anything.
    // --------------------------------------------------------
    runner.registerMigration({816, "rental_contract_billing_period", R"SQL(
        ALTER TABLE rental_contract
            ADD COLUMN IF NOT EXISTS billing_interval INTEGER;
        ALTER TABLE rental_contract
            ADD COLUMN IF NOT EXISTS billing_unit VARCHAR;

        -- The preset list. 'custom' is what makes "every X <unit>" reachable:
        -- the preset names the shape, the interval/unit pair carries the X.
        --
        -- rental_contract_period_chk is the ORIGINAL constraint from migration
        -- 802 and only allows monthly/quarterly/yearly. Dropping it by the name
        -- one would guess (…_billing_period_chk) silently drops nothing, and
        -- then every new preset fails on a constraint that is still there.
        ALTER TABLE rental_contract DROP CONSTRAINT IF EXISTS rental_contract_period_chk;
        ALTER TABLE rental_contract DROP CONSTRAINT IF EXISTS rental_contract_billing_period_check;
        ALTER TABLE rental_contract DROP CONSTRAINT IF EXISTS rental_contract_billing_period_chk;
        UPDATE rental_contract
           SET billing_period = 'monthly'
         WHERE billing_period IS NULL
            OR billing_period NOT IN ('daily','weekly','monthly','quarterly',
                                      'biannual','yearly','custom','oneoff','ondemand');
        ALTER TABLE rental_contract
            ADD CONSTRAINT rental_contract_billing_period_chk
            CHECK (billing_period IN ('daily','weekly','monthly','quarterly',
                                      'biannual','yearly','custom','oneoff','ondemand'));

        DO $mig$ BEGIN
            IF NOT EXISTS (SELECT 1 FROM pg_constraint
                            WHERE conname = 'rental_contract_billing_unit_chk') THEN
                ALTER TABLE rental_contract ADD CONSTRAINT rental_contract_billing_unit_chk
                    CHECK (billing_unit IS NULL OR billing_unit IN ('day','week','month','year'));
            END IF;
            IF NOT EXISTS (SELECT 1 FROM pg_constraint
                            WHERE conname = 'rental_contract_billing_interval_chk') THEN
                ALTER TABLE rental_contract ADD CONSTRAINT rental_contract_billing_interval_chk
                    CHECK (billing_interval IS NULL OR
                           (billing_interval >= 1 AND billing_interval <= 366));
            END IF;
        END $mig$;

        -- One place decides what a preset means, so the UI, an import and a
        -- direct SQL insert cannot disagree about how often "quarterly" bills.
        CREATE OR REPLACE FUNCTION rental_contract_derive_period()
        RETURNS TRIGGER AS $fn$
        BEGIN
            CASE NEW.billing_period
                WHEN 'daily'     THEN NEW.billing_interval := 1; NEW.billing_unit := 'day';
                WHEN 'weekly'    THEN NEW.billing_interval := 1; NEW.billing_unit := 'week';
                WHEN 'monthly'   THEN NEW.billing_interval := 1; NEW.billing_unit := 'month';
                WHEN 'quarterly' THEN NEW.billing_interval := 3; NEW.billing_unit := 'month';
                WHEN 'biannual'  THEN NEW.billing_interval := 6; NEW.billing_unit := 'month';
                WHEN 'yearly'    THEN NEW.billing_interval := 1; NEW.billing_unit := 'year';
                WHEN 'custom'    THEN
                    -- The only preset where the user's numbers survive.
                    NEW.billing_interval := GREATEST(1, LEAST(366, COALESCE(NEW.billing_interval, 1)));
                    NEW.billing_unit     := COALESCE(NEW.billing_unit, 'month');
                ELSE
                    -- oneoff / ondemand: never scheduled.
                    NEW.billing_interval := NULL;
                    NEW.billing_unit     := NULL;
            END CASE;
            RETURN NEW;
        END $fn$ LANGUAGE plpgsql;

        DROP TRIGGER IF EXISTS rental_contract_period_trg ON rental_contract;
        CREATE TRIGGER rental_contract_period_trg
            BEFORE INSERT OR UPDATE ON rental_contract
            FOR EACH ROW EXECUTE FUNCTION rental_contract_derive_period();

        -- Backfill: re-state every row so the trigger fills in the pair.
        UPDATE rental_contract SET billing_period = billing_period;

        -- NULL on a line now means "inherit". Existing lines keep the values
        -- they have, so nothing that was billing at a set cadence changes.
        ALTER TABLE rental_contract_line ALTER COLUMN billing_interval DROP NOT NULL;
        ALTER TABLE rental_contract_line ALTER COLUMN billing_interval DROP DEFAULT;
        ALTER TABLE rental_contract_line ALTER COLUMN billing_unit     DROP NOT NULL;
        ALTER TABLE rental_contract_line ALTER COLUMN billing_unit     DROP DEFAULT;
    )SQL"});

    // --------------------------------------------------------
    // 817 — retire the line's OLD billing_mode constraint
    //
    // 815 added rental_line_billing_mode_chk allowing oneoff and ondemand, and
    // dropped "rental_line_billing_mode_chk" first — a name that did not exist
    // yet. The original constraint is called rental_cl_billing_mode_chk, so it
    // survived, and a row must satisfy EVERY check: the effective set stayed
    // ('manual','recurring') and a one-off line was rejected by a constraint
    // nobody had noticed was still there.
    //
    // This is the second time a rename-by-guess has left a stale CHECK in
    // place (see 816 and rental_contract_period_chk). Drop by the name the
    // database reports, never by the name the new constraint will have.
    // --------------------------------------------------------
    runner.registerMigration({817, "rental_line_drop_legacy_billing_mode_chk", R"SQL(
        ALTER TABLE rental_contract_line DROP CONSTRAINT IF EXISTS rental_cl_billing_mode_chk;

        -- 815's interval check predates NULL meaning "inherit the contract's
        -- period" (816). NULL passes a CHECK, so nothing was rejected, but the
        -- constraint should say what it means.
        ALTER TABLE rental_contract_line DROP CONSTRAINT IF EXISTS rental_line_billing_interval_chk;
        ALTER TABLE rental_contract_line
            ADD CONSTRAINT rental_line_billing_interval_chk
            CHECK (billing_interval IS NULL OR
                   (billing_interval >= 1 AND billing_interval <= 366));
    )SQL"});

    // --------------------------------------------------------
    // 818 — a line under a contract inherits that contract's customer
    //
    // The contract form now edits its lines directly, and the line grid does
    // not show a Customer column: the contract already says who the tenant is,
    // and asking again invites the two to disagree. But partner_id is required
    // on the line — migration 812 moved the customer there so a walk-in can
    // rent with no contract at all — so a line added from the contract form
    // would arrive with no customer and be rejected.
    //
    // Filled here rather than in the client, so an import and a hand-written
    // INSERT get it too. Only when the line does not carry one: a walk-in line
    // has no contract_id and keeps the customer it was given.
    // --------------------------------------------------------
    runner.registerMigration({818, "rental_line_inherit_contract_partner", R"SQL(
        CREATE OR REPLACE FUNCTION rental_line_inherit_partner()
        RETURNS TRIGGER AS $fn$
        BEGIN
            IF NEW.partner_id IS NULL AND NEW.contract_id IS NOT NULL THEN
                SELECT partner_id INTO NEW.partner_id
                  FROM rental_contract WHERE id = NEW.contract_id;
            END IF;
            RETURN NEW;
        END $fn$ LANGUAGE plpgsql;

        DROP TRIGGER IF EXISTS rental_line_partner_trg ON rental_contract_line;
        CREATE TRIGGER rental_line_partner_trg
            BEFORE INSERT OR UPDATE ON rental_contract_line
            FOR EACH ROW EXECUTE FUNCTION rental_line_inherit_partner();

        -- Existing rows that somehow lack one.
        UPDATE rental_contract_line l
           SET partner_id = c.partner_id
          FROM rental_contract c
         WHERE l.contract_id = c.id AND l.partner_id IS NULL;
    )SQL"});

    // --------------------------------------------------------
    // 819 — a recurring line starts billing from its own start date
    //
    // The billing run selects lines with next_period_start IS NOT NULL. Nothing
    // set it: the tests that existed wrote it by hand, so it was always
    // populated in a test and never populated in real use. A line added on the
    // contract form was therefore active, recurring, priced -- and silently
    // never invoiced. Found by driving the screen; an API test that sets the
    // column itself cannot see it.
    // --------------------------------------------------------
    runner.registerMigration({819, "rental_line_default_next_period", R"SQL(
        CREATE OR REPLACE FUNCTION rental_line_default_next_period()
        RETURNS TRIGGER AS $fn$
        BEGIN
            -- Only for a schedule that HAS periods. manual, oneoff and ondemand
            -- are billed when someone decides, so a next period would be a lie.
            IF NEW.next_period_start IS NULL
               AND NEW.billing_mode = 'recurring'
               AND NEW.date_start IS NOT NULL THEN
                NEW.next_period_start := NEW.date_start;
            END IF;
            RETURN NEW;
        END $fn$ LANGUAGE plpgsql;

        DROP TRIGGER IF EXISTS rental_line_next_period_trg ON rental_contract_line;
        CREATE TRIGGER rental_line_next_period_trg
            BEFORE INSERT ON rental_contract_line
            FOR EACH ROW EXECUTE FUNCTION rental_line_default_next_period();

        UPDATE rental_contract_line
           SET next_period_start = date_start
         WHERE next_period_start IS NULL
           AND billing_mode = 'recurring'
           AND date_start IS NOT NULL
           AND invoiced_through IS NULL;
    )SQL"});

    // --------------------------------------------------------
    // 820 — sequential bookings on one unit (the booking calendar)
    //
    // Migration 803 put a partial UNIQUE index on rental_contract_line
    // (unit_id) WHERE state IN ('pending','active'). That is the double-let
    // guard, and it is the right instinct: deriving rental_unit.state keeps
    // the UI honest, but only a CONSTRAINT makes the race impossible when two
    // operators let the same locker at the same moment.
    //
    // It is also too strong for a booking calendar. "At most one live line per
    // unit" forbids Alice in A-101 for 3-7 December AND Bob for 12-20
    // December — two lets that never touch. A calendar you cannot book twice
    // on is not a calendar.
    //
    // So the guard is not removed, it is SHARPENED: the rule was never "one
    // line per unit", it was "never two tenants in one unit at the same time".
    // An exclusion constraint says exactly that and nothing more.
    //
    // Ranges are INCLUSIVE at both ends ('[]'). date_end is the last day of
    // the let — the billing run already treats it that way
    // (next_period_start <= date_end) — so a unit IS occupied on date_end and
    // the next booking starts the day after. An open-ended line runs to
    // 'infinity', which is what makes "rent until termination" block every
    // later booking, correctly.
    //
    // TWO guards, deliberately:
    //
    //   * the EXCLUDE constraint is race-proof and enforced whatever writes
    //     the row — but needs btree_gist, and its message is unreadable;
    //   * the BEFORE trigger names the dates that clash, which is what an
    //     operator needs, and still enforces the rule on a database where the
    //     extension cannot be installed.
    //
    // Existing data cannot violate this: the old index already allowed at most
    // one live line per unit, so there is nothing to overlap with.
    // --------------------------------------------------------
    runner.registerMigration({820, "rental_line_booking_overlap", R"SQL(
        DO $mig$
        BEGIN
            -- Best effort. A role without CREATE privilege on the database
            -- falls through to the trigger, which needs no extension.
            BEGIN
                CREATE EXTENSION IF NOT EXISTS btree_gist;
            EXCEPTION WHEN OTHERS THEN
                RAISE NOTICE 'btree_gist unavailable; the overlap guard will be trigger-only';
            END;

            IF EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'btree_gist')
               AND NOT EXISTS (SELECT 1 FROM pg_constraint
                                WHERE conname = 'rental_cl_unit_no_overlap') THEN
                EXECUTE $ddl$
                    ALTER TABLE rental_contract_line
                      ADD CONSTRAINT rental_cl_unit_no_overlap
                      EXCLUDE USING gist (
                          unit_id WITH =,
                          daterange(date_start,
                                    COALESCE(date_end, 'infinity'::date), '[]') WITH &&)
                      WHERE (state IN ('pending','active') AND unit_id IS NOT NULL)
                $ddl$;
            END IF;
        END $mig$;

        -- The readable half of the guard. Fires first, so an operator sees
        -- which dates clash rather than a constraint name.
        CREATE OR REPLACE FUNCTION rental_line_check_overlap() RETURNS trigger AS $fn$
        DECLARE clash RECORD;
        BEGIN
            IF NEW.unit_id IS NULL OR NEW.state NOT IN ('pending','active') THEN
                RETURN NEW;
            END IF;
            SELECT l.date_start, l.date_end INTO clash
              FROM rental_contract_line l
             WHERE l.unit_id = NEW.unit_id
               AND l.id IS DISTINCT FROM NEW.id
               AND l.state IN ('pending','active')
               AND daterange(l.date_start,
                             COALESCE(l.date_end, 'infinity'::date), '[]')
                && daterange(NEW.date_start,
                             COALESCE(NEW.date_end, 'infinity'::date), '[]')
             LIMIT 1;
            IF FOUND THEN
                RAISE EXCEPTION
                    'This unit is already let from % to %',
                    clash.date_start,
                    COALESCE(clash.date_end::text, 'open-ended')
                    USING ERRCODE = 'exclusion_violation';
            END IF;
            RETURN NEW;
        END $fn$ LANGUAGE plpgsql;

        DROP TRIGGER IF EXISTS rental_line_overlap_trg ON rental_contract_line;
        CREATE TRIGGER rental_line_overlap_trg
            BEFORE INSERT OR UPDATE OF unit_id, date_start, date_end, state
            ON rental_contract_line
            FOR EACH ROW EXECUTE FUNCTION rental_line_check_overlap();

        -- Only now is the old index redundant. Dropped by the name the
        -- database reports, not a guessed one (docs/development/conventions).
        DROP INDEX IF EXISTS rental_cl_unit_live_uniq;

        -- The calendar's driving query: live lines for a unit over a window.
        CREATE INDEX IF NOT EXISTS rental_cl_unit_period_idx
            ON rental_contract_line (unit_id, date_start, date_end)
            WHERE state IN ('pending','active');
    )SQL"});

}

} // namespace cerp::modules::rental

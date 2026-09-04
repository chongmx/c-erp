// ============================================================
// modules/base/PartnerMigrations.cpp — the partner hierarchy (docs/130)
//
// A customer is usually an organisation with people inside it. res.partner
// models that with parent_id (the customer's company) — which is a DIFFERENT
// thing from company_id (the tenant that owns the row). Confusing the two is
// what made "add a contact to a customer" fail with "You cannot create records
// for another company"; see docs/130 §1.
//
// These are real migrations rather than ensureSchema_ ALTERs because they
// BACKFILL data. ensureSchema_ runs every boot with no ordering and no
// once-only guarantee, which is fine for "ADD COLUMN IF NOT EXISTS" and wrong
// for "walk the tree and populate a column".
//
// Version range 1-99 is reserved for core/base (MigrationRunner.hpp).
// ============================================================
#include "PartnerMigrations.hpp"
#include "MigrationRunner.hpp"

namespace cerp::modules::base {

using cerp::infrastructure::MigrationRunner;

void registerPartnerMigrations(MigrationRunner& runner) {

    // --------------------------------------------------------
    // 10 — commercial_partner_id (docs/130 §4)
    //
    // "Everything for this customer" used to need an OR across two columns:
    //
    //     WHERE id = :acme OR parent_id = :acme
    //
    // which does not index well and is WRONG at depth — a contact under a
    // branch under Acme is missed entirely. Odoo 14 stores the answer instead
    // (res_partner.py:289-297) and so do we.
    //
    // Maintained by trigger, not by the model, so it stays true for SQL written
    // by hand, by a migration, or by a module that never goes through
    // ResPartner — the same reasoning as the cycle guard.
    // --------------------------------------------------------
    runner.registerMigration({10, "res_partner_commercial_partner_id", R"SQL(
        ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS commercial_partner_id INTEGER;

        DO $mig$ BEGIN
            IF NOT EXISTS (SELECT 1 FROM pg_constraint
                            WHERE conname = 'res_partner_commercial_fk') THEN
                ALTER TABLE res_partner ADD CONSTRAINT res_partner_commercial_fk
                    FOREIGN KEY (commercial_partner_id)
                    REFERENCES res_partner(id) ON DELETE SET NULL;
            END IF;
        END $mig$;

        CREATE INDEX IF NOT EXISTS res_partner_commercial_idx
            ON res_partner(commercial_partner_id);

        -- Resolve one row: a company, or a partner with no parent, is its own
        -- commercial entity; anyone else inherits their parent's.
        CREATE OR REPLACE FUNCTION res_partner_commercial_of(p_id INTEGER)
        RETURNS INTEGER AS $fn$
        DECLARE cur INTEGER := p_id; par INTEGER; iscomp BOOLEAN; depth INTEGER := 0;
        BEGIN
            LOOP
                SELECT parent_id, is_company INTO par, iscomp
                  FROM res_partner WHERE id = cur;
                IF NOT FOUND THEN RETURN NULL; END IF;
                IF iscomp OR par IS NULL THEN RETURN cur; END IF;
                cur := par; depth := depth + 1;
                -- The cycle trigger makes a loop impossible, but a bounded walk
                -- means a corrupt row degrades to a wrong answer, not a hang.
                IF depth > 64 THEN RETURN cur; END IF;
            END LOOP;
        END $fn$ LANGUAGE plpgsql;

        CREATE OR REPLACE FUNCTION res_partner_set_commercial() RETURNS trigger AS $fn$
        BEGIN
            IF NEW.is_company OR NEW.parent_id IS NULL THEN
                NEW.commercial_partner_id := NEW.id;
            ELSE
                NEW.commercial_partner_id := res_partner_commercial_of(NEW.parent_id);
            END IF;
            RETURN NEW;
        END $fn$ LANGUAGE plpgsql;

        DROP TRIGGER IF EXISTS res_partner_commercial_trg ON res_partner;
        CREATE TRIGGER res_partner_commercial_trg
            BEFORE INSERT OR UPDATE OF parent_id, is_company ON res_partner
            FOR EACH ROW EXECUTE FUNCTION res_partner_set_commercial();

        -- Re-parenting a company must move everyone beneath it, at any depth.
        CREATE OR REPLACE FUNCTION res_partner_cascade_commercial() RETURNS trigger AS $fn$
        BEGIN
            IF NEW.commercial_partner_id IS DISTINCT FROM OLD.commercial_partner_id THEN
                WITH RECURSIVE kids AS (
                    SELECT id FROM res_partner WHERE parent_id = NEW.id
                    UNION ALL
                    SELECT c.id FROM res_partner c JOIN kids k ON c.parent_id = k.id
                )
                UPDATE res_partner p
                   SET commercial_partner_id = res_partner_commercial_of(p.id)
                  FROM kids WHERE p.id = kids.id AND NOT p.is_company;
            END IF;
            RETURN NULL;
        END $fn$ LANGUAGE plpgsql;

        DROP TRIGGER IF EXISTS res_partner_cascade_commercial_trg ON res_partner;
        CREATE TRIGGER res_partner_cascade_commercial_trg
            AFTER UPDATE OF commercial_partner_id ON res_partner
            FOR EACH ROW EXECUTE FUNCTION res_partner_cascade_commercial();

        -- Backfill. Companies and roots first, then everyone else resolves
        -- against them.
        UPDATE res_partner SET commercial_partner_id = id
         WHERE is_company OR parent_id IS NULL;
        UPDATE res_partner SET commercial_partner_id = res_partner_commercial_of(id)
         WHERE commercial_partner_id IS NULL;
    )SQL"});

    // --------------------------------------------------------
    // 11 — the tenant descends the hierarchy (docs/130 §5)
    //
    // Odoo sets a child's company_id from its parent (res_partner.py:369) and
    // cascades on write (:539). We stamped the CURRENT USER's company instead,
    // which is usually the same value and occasionally not — and the failure
    // mode is a contact sitting in a different tenant from its own company,
    // invisible to the people who own it.
    // --------------------------------------------------------
    runner.registerMigration({11, "res_partner_tenant_descends", R"SQL(
        CREATE OR REPLACE FUNCTION res_partner_inherit_tenant() RETURNS trigger AS $fn$
        DECLARE parent_co INTEGER;
        BEGIN
            IF NEW.parent_id IS NOT NULL THEN
                SELECT company_id INTO parent_co FROM res_partner WHERE id = NEW.parent_id;
                IF parent_co IS NOT NULL THEN
                    NEW.company_id := parent_co;
                END IF;
            END IF;
            RETURN NEW;
        END $fn$ LANGUAGE plpgsql;

        DROP TRIGGER IF EXISTS res_partner_inherit_tenant_trg ON res_partner;
        CREATE TRIGGER res_partner_inherit_tenant_trg
            BEFORE INSERT OR UPDATE OF parent_id, company_id ON res_partner
            FOR EACH ROW WHEN (NEW.parent_id IS NOT NULL)
            EXECUTE FUNCTION res_partner_inherit_tenant();

        CREATE OR REPLACE FUNCTION res_partner_cascade_tenant() RETURNS trigger AS $fn$
        BEGIN
            IF NEW.company_id IS DISTINCT FROM OLD.company_id THEN
                WITH RECURSIVE kids AS (
                    SELECT id FROM res_partner WHERE parent_id = NEW.id
                    UNION ALL
                    SELECT c.id FROM res_partner c JOIN kids k ON c.parent_id = k.id
                )
                UPDATE res_partner p SET company_id = NEW.company_id
                  FROM kids WHERE p.id = kids.id;
            END IF;
            RETURN NULL;
        END $fn$ LANGUAGE plpgsql;

        DROP TRIGGER IF EXISTS res_partner_cascade_tenant_trg ON res_partner;
        CREATE TRIGGER res_partner_cascade_tenant_trg
            AFTER UPDATE OF company_id ON res_partner
            FOR EACH ROW EXECUTE FUNCTION res_partner_cascade_tenant();

        UPDATE res_partner c SET company_id = p.company_id
          FROM res_partner p
         WHERE c.parent_id = p.id
           AND p.company_id IS NOT NULL
           AND c.company_id IS DISTINCT FROM p.company_id;
    )SQL"});

    // --------------------------------------------------------
    // 12 — address types (docs/130 §6)
    //
    // sale_order already carries partner_invoice_id and partner_shipping_id
    // (SaleModule.cpp:98-99), but nothing could CREATE a partner that IS an
    // invoice address, so those fields could only ever point at whole contacts.
    // Odoo's `type` (res_partner.py:185) is what they were waiting for.
    // --------------------------------------------------------
    runner.registerMigration({12, "res_partner_address_type", R"SQL(
        ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS type VARCHAR NOT NULL DEFAULT 'contact';
        ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS street2 VARCHAR;

        DO $mig$ BEGIN
            IF NOT EXISTS (SELECT 1 FROM pg_constraint
                            WHERE conname = 'res_partner_type_chk') THEN
                ALTER TABLE res_partner ADD CONSTRAINT res_partner_type_chk
                    CHECK (type IN ('contact','invoice','delivery','other'));
            END IF;
        END $mig$;

        CREATE INDEX IF NOT EXISTS res_partner_type_idx ON res_partner(parent_id, type);
    )SQL"});
    // --------------------------------------------------------
    // 13 — the cascade triggers must actually fire (fixes 10 and 11)
    //
    // `AFTER UPDATE OF <column>` fires on the columns NAMED IN THE UPDATE
    // STATEMENT, not on the columns that actually changed. Re-parenting runs
    //
    //     UPDATE res_partner SET parent_id = ...
    //
    // and the BEFORE trigger then rewrites commercial_partner_id — but because
    // the statement never named that column, `AFTER UPDATE OF
    // commercial_partner_id` stayed silent and descendants kept pointing at the
    // old company. Caught by tests/integration/core/partner-hierarchy §1: a
    // contact two levels down still resolved to the previous parent.
    //
    // Fire on ANY update and keep the IS DISTINCT FROM guard inside, which is
    // what makes the trigger cheap in the common case.
    // --------------------------------------------------------
    runner.registerMigration({13, "res_partner_cascade_fires", R"SQL(
        DROP TRIGGER IF EXISTS res_partner_cascade_commercial_trg ON res_partner;
        CREATE TRIGGER res_partner_cascade_commercial_trg
            AFTER UPDATE ON res_partner
            FOR EACH ROW EXECUTE FUNCTION res_partner_cascade_commercial();

        DROP TRIGGER IF EXISTS res_partner_cascade_tenant_trg ON res_partner;
        CREATE TRIGGER res_partner_cascade_tenant_trg
            AFTER UPDATE ON res_partner
            FOR EACH ROW EXECUTE FUNCTION res_partner_cascade_tenant();

        -- Repair anything the silent triggers left behind.
        UPDATE res_partner p SET commercial_partner_id = res_partner_commercial_of(p.id)
         WHERE p.commercial_partner_id IS DISTINCT FROM res_partner_commercial_of(p.id);
        UPDATE res_partner c SET company_id = p.company_id
          FROM res_partner p
         WHERE c.parent_id = p.id AND p.company_id IS NOT NULL
           AND c.company_id IS DISTINCT FROM p.company_id;
    )SQL"});

    // --------------------------------------------------------
    // 14 — commercial_company_name (docs/130 §4, Odoo res_partner.py:228,300-303)
    //
    // Migration 13 left a hole. Phase 5 clears company_name once parent_id is
    // set, because the relation is then the source of truth — but the contact
    // LIST renders company_name, so every contact under a company showed a
    // blank Company column. The source of truth moved and the display did not.
    //
    // Odoo's answer is a second derived value:
    //
    //     commercial_company_name = commercial_partner.is_company
    //                             ? commercial_partner.name
    //                             : own company_name
    //
    // so a linked contact shows its company's real name and an imported one
    // still shows its free text. Stored, not computed at read, so the list can
    // sort and filter on it.
    //
    // Renaming the company must reach every contact beneath it, which is why
    // the trigger also fires on `name` and cascades.
    // --------------------------------------------------------
    runner.registerMigration({14, "res_partner_commercial_company_name", R"SQL(
        ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS commercial_company_name VARCHAR;

        CREATE OR REPLACE FUNCTION res_partner_commercial_name_of(p_id INTEGER)
        RETURNS VARCHAR AS $fn$
        DECLARE cp INTEGER; nm VARCHAR; isc BOOLEAN; own VARCHAR;
        BEGIN
            SELECT commercial_partner_id, company_name INTO cp, own
              FROM res_partner WHERE id = p_id;
            IF cp IS NULL THEN RETURN own; END IF;
            SELECT name, is_company INTO nm, isc FROM res_partner WHERE id = cp;
            IF isc THEN RETURN nm; END IF;
            RETURN own;
        END $fn$ LANGUAGE plpgsql;

        CREATE OR REPLACE FUNCTION res_partner_set_commercial_name() RETURNS trigger AS $fn$
        DECLARE cp INTEGER; nm VARCHAR; isc BOOLEAN;
        BEGIN
            -- Do NOT read NEW.commercial_partner_id here. Same-timing triggers
            -- fire in NAME order, and res_partner_commercial_NAME_trg sorts
            -- BEFORE res_partner_commercial_trg ('n' < 't'), so that column is
            -- still NULL at this point on INSERT. Relying on it produced a blank
            -- Company cell for every contact — the exact bug this migration is
            -- meant to fix. Resolve the chain independently instead, mirroring
            -- the logic in res_partner_set_commercial().
            IF NEW.is_company OR NEW.parent_id IS NULL THEN
                NEW.commercial_company_name :=
                    CASE WHEN NEW.is_company THEN NEW.name ELSE NEW.company_name END;
            ELSE
                cp := res_partner_commercial_of(NEW.parent_id);
                SELECT name, is_company INTO nm, isc FROM res_partner WHERE id = cp;
                NEW.commercial_company_name := CASE WHEN isc THEN nm ELSE NEW.company_name END;
            END IF;
            RETURN NEW;
        END $fn$ LANGUAGE plpgsql;

        DROP TRIGGER IF EXISTS res_partner_commercial_name_trg ON res_partner;
        CREATE TRIGGER res_partner_commercial_name_trg
            BEFORE INSERT OR UPDATE ON res_partner
            FOR EACH ROW EXECUTE FUNCTION res_partner_set_commercial_name();

        -- Renaming a company must reach every contact beneath it.
        CREATE OR REPLACE FUNCTION res_partner_cascade_commercial_name() RETURNS trigger AS $fn$
        BEGIN
            IF NEW.name IS DISTINCT FROM OLD.name
               OR NEW.is_company IS DISTINCT FROM OLD.is_company
               OR NEW.commercial_partner_id IS DISTINCT FROM OLD.commercial_partner_id THEN
                UPDATE res_partner c
                   SET commercial_company_name = res_partner_commercial_name_of(c.id)
                 WHERE c.commercial_partner_id = NEW.id
                   AND c.id <> NEW.id
                   AND c.commercial_company_name
                       IS DISTINCT FROM res_partner_commercial_name_of(c.id);
            END IF;
            RETURN NULL;
        END $fn$ LANGUAGE plpgsql;

        DROP TRIGGER IF EXISTS res_partner_cascade_commercial_name_trg ON res_partner;
        CREATE TRIGGER res_partner_cascade_commercial_name_trg
            AFTER UPDATE ON res_partner
            FOR EACH ROW EXECUTE FUNCTION res_partner_cascade_commercial_name();

        UPDATE res_partner SET commercial_company_name = res_partner_commercial_name_of(id);
    )SQL"});

}

} // namespace cerp::modules::base

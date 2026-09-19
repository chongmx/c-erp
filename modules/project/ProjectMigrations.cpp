// ============================================================
// modules/project/ProjectMigrations.cpp
// ============================================================
#include "ProjectMigrations.hpp"
#include "MigrationRunner.hpp"

namespace cerp::modules::project {

using cerp::infrastructure::MigrationRunner;

void registerProjectMigrations(MigrationRunner& runner) {

    // --------------------------------------------------------
    // 1100 — a task becomes a ticket
    //
    // KEYS. Every project has a prefix ("CERP") and a counter; a task takes
    // the next number when it is created, so the first task of c-erp is
    // CERP-1 for ever. The trigger is the ONLY writer of number, key and
    // display_name: a client that sends them has its values overwritten, so
    // two tickets can never share a key and a key can never be edited into
    // pointing at something else.
    //
    //   * number comes from UPDATE ... RETURNING on the project row, which
    //     row-locks it — two tasks created at the same instant cannot draw
    //     the same number.
    //   * moving a task to another project gives it that project's next
    //     number (the Jira rule): its old key named the old project.
    //   * renaming a prefix re-keys the project's tasks, via the AFTER
    //     trigger on project_project re-running the task trigger.
    //
    // display_name ("CERP-12 Fix the save button") is stored so a picker can
    // search by key as well as by title — the parent-task picker, most
    // obviously. Same reasoning as res_partner.display_name (migration 15).
    //
    // PRIORITY stays an integer so existing rows keep their meaning (the old
    // star was priority 1 = High): -1 Low, 0 Normal, 1 High, 2 Urgent. Sorting
    // by priority DESC is then simply the order of urgency.
    // --------------------------------------------------------
    runner.registerMigration({1100, "project_issue_keys", R"SQL(
        ALTER TABLE project_project ADD COLUMN IF NOT EXISTS task_prefix VARCHAR;
        ALTER TABLE project_project ADD COLUMN IF NOT EXISTS task_seq    INTEGER NOT NULL DEFAULT 0;

        ALTER TABLE project_task ADD COLUMN IF NOT EXISTS number       INTEGER;
        ALTER TABLE project_task ADD COLUMN IF NOT EXISTS key          VARCHAR;
        ALTER TABLE project_task ADD COLUMN IF NOT EXISTS display_name VARCHAR;
        ALTER TABLE project_task ADD COLUMN IF NOT EXISTS issue_type   VARCHAR NOT NULL DEFAULT 'task';
        ALTER TABLE project_task ADD COLUMN IF NOT EXISTS reporter_id  INTEGER
            REFERENCES res_users(id) ON DELETE SET NULL;

        -- A prefix from the project: "c-erp" -> CERP, "Easy Locker Space" -> ELS.
        -- One word: its letters and digits, first four. Several: their initials.
        -- Always starts with a letter, 2-10 characters, unique (a number is
        -- appended to break a tie).
        CREATE OR REPLACE FUNCTION project_derive_prefix(p_name TEXT, p_id INTEGER)
        RETURNS TEXT LANGUAGE plpgsql AS $fn$
        DECLARE
            words TEXT[];
            base  TEXT;
            cand  TEXT;
            i     INTEGER := 1;
        BEGIN
            words := regexp_split_to_array(trim(COALESCE(p_name, '')), '\s+');
            IF array_length(words, 1) > 1 THEN
                base := '';
                FOR i IN 1 .. LEAST(array_length(words, 1), 4) LOOP
                    base := base || left(regexp_replace(words[i], '[^A-Za-z0-9]', '', 'g'), 1);
                END LOOP;
            ELSE
                base := left(regexp_replace(COALESCE(p_name, ''), '[^A-Za-z0-9]', '', 'g'), 4);
            END IF;
            base := upper(base);
            IF base !~ '^[A-Z]' THEN base := 'P' || base; END IF;
            base := left(base, 8);
            IF length(base) < 2 THEN base := 'TASK'; END IF;

            cand := base;
            i := 1;
            WHILE EXISTS (SELECT 1 FROM project_project
                           WHERE upper(task_prefix) = cand AND id IS DISTINCT FROM p_id) LOOP
                i := i + 1;
                cand := base || i;
            END LOOP;
            RETURN cand;
        END
        $fn$;

        -- Existing projects, oldest first, so the first one keeps the plain prefix.
        DO $do$
        DECLARE r RECORD;
        BEGIN
            FOR r IN SELECT id, name FROM project_project
                      WHERE COALESCE(task_prefix, '') = '' ORDER BY id LOOP
                UPDATE project_project SET task_prefix = project_derive_prefix(r.name, r.id)
                 WHERE id = r.id;
            END LOOP;
        END
        $do$;
        UPDATE project_project SET task_prefix = upper(task_prefix);

        -- Existing tasks, numbered in creation order within their project.
        WITH n AS (
            SELECT id, row_number() OVER (PARTITION BY project_id ORDER BY id) AS num
              FROM project_task
        )
        UPDATE project_task t SET number = n.num FROM n WHERE t.id = n.id;
        UPDATE project_task t
           SET key = p.task_prefix || '-' || t.number,
               display_name = p.task_prefix || '-' || t.number || ' ' || t.name
          FROM project_project p WHERE p.id = t.project_id;
        UPDATE project_project p
           SET task_seq = COALESCE((SELECT max(number) FROM project_task t
                                     WHERE t.project_id = p.id), 0);

        -- Priority: the old star (1) keeps meaning High; anything outside the
        -- four levels is clamped rather than rejected.
        UPDATE project_task SET priority = LEAST(GREATEST(priority, -1), 2)
         WHERE priority NOT BETWEEN -1 AND 2;

        ALTER TABLE project_task DROP CONSTRAINT IF EXISTS project_task_issue_type_chk;
        ALTER TABLE project_task ADD CONSTRAINT project_task_issue_type_chk
            CHECK (issue_type IN ('task', 'bug', 'feature', 'chore'));
        ALTER TABLE project_task DROP CONSTRAINT IF EXISTS project_task_priority_chk;
        ALTER TABLE project_task ADD CONSTRAINT project_task_priority_chk
            CHECK (priority BETWEEN -1 AND 2);
        ALTER TABLE project_project DROP CONSTRAINT IF EXISTS project_project_task_prefix_chk;
        ALTER TABLE project_project ADD CONSTRAINT project_project_task_prefix_chk
            CHECK (task_prefix ~ '^[A-Z][A-Z0-9]{1,9}$');

        CREATE UNIQUE INDEX IF NOT EXISTS project_project_task_prefix_uniq
            ON project_project (upper(task_prefix));
        CREATE UNIQUE INDEX IF NOT EXISTS project_task_number_uniq
            ON project_task (project_id, number);
        CREATE INDEX IF NOT EXISTS project_task_key_idx
            ON project_task (upper(key));
        CREATE INDEX IF NOT EXISTS project_task_display_name_idx
            ON project_task (lower(display_name));

        -- A project created without a prefix gets one; a prefix typed in lower
        -- case is stored in upper case.
        CREATE OR REPLACE FUNCTION project_prefix_fn() RETURNS trigger
        LANGUAGE plpgsql AS $fn$
        BEGIN
            IF COALESCE(trim(NEW.task_prefix), '') = '' THEN
                NEW.task_prefix := project_derive_prefix(NEW.name, NEW.id);
            ELSE
                NEW.task_prefix := upper(trim(NEW.task_prefix));
            END IF;
            RETURN NEW;
        END
        $fn$;
        DROP TRIGGER IF EXISTS project_prefix_trg ON project_project;
        CREATE TRIGGER project_prefix_trg
            BEFORE INSERT OR UPDATE OF task_prefix, name ON project_project
            FOR EACH ROW EXECUTE FUNCTION project_prefix_fn();

        -- The only writer of number, key and display_name.
        CREATE OR REPLACE FUNCTION project_task_key_fn() RETURNS trigger
        LANGUAGE plpgsql AS $fn$
        DECLARE
            pfx TEXT;
            n   INTEGER;
        BEGIN
            IF TG_OP = 'INSERT' OR NEW.project_id IS DISTINCT FROM OLD.project_id THEN
                UPDATE project_project SET task_seq = task_seq + 1
                 WHERE id = NEW.project_id
                RETURNING task_seq, task_prefix INTO n, pfx;
                NEW.number := n;
            ELSE
                NEW.number := OLD.number;
                SELECT task_prefix INTO pfx FROM project_project WHERE id = NEW.project_id;
            END IF;
            NEW.key          := COALESCE(NULLIF(pfx, ''), 'TASK') || '-' || NEW.number;
            NEW.display_name := NEW.key || ' ' || NEW.name;
            RETURN NEW;
        END
        $fn$;
        DROP TRIGGER IF EXISTS project_task_key_trg ON project_task;
        CREATE TRIGGER project_task_key_trg
            BEFORE INSERT OR UPDATE ON project_task
            FOR EACH ROW EXECUTE FUNCTION project_task_key_fn();

        -- A renamed prefix re-keys the project's tasks: touching each row makes
        -- the task trigger recompute its key from the new prefix.
        CREATE OR REPLACE FUNCTION project_rekey_fn() RETURNS trigger
        LANGUAGE plpgsql AS $fn$
        BEGIN
            IF NEW.task_prefix IS DISTINCT FROM OLD.task_prefix THEN
                UPDATE project_task SET number = number WHERE project_id = NEW.id;
            END IF;
            RETURN NULL;
        END
        $fn$;
        DROP TRIGGER IF EXISTS project_rekey_trg ON project_project;
        CREATE TRIGGER project_rekey_trg
            AFTER UPDATE OF task_prefix ON project_project
            FOR EACH ROW EXECUTE FUNCTION project_rekey_fn();
    )SQL"});

    // --------------------------------------------------------
    // 1101 — labels and watchers
    //
    // Link tables rather than array columns: "every ticket labelled ui" and
    // "everything I watch" are the questions asked of them, and both are an
    // index lookup this way. Labels are shared across projects, like Jira's.
    // --------------------------------------------------------
    runner.registerMigration({1101, "project_labels_watchers", R"SQL(
        CREATE TABLE IF NOT EXISTS project_tag (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            color       INTEGER NOT NULL DEFAULT 0,
            active      BOOLEAN NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        );
        CREATE UNIQUE INDEX IF NOT EXISTS project_tag_name_uniq
            ON project_tag (lower(name));

        CREATE TABLE IF NOT EXISTS project_task_tag_rel (
            task_id INTEGER NOT NULL REFERENCES project_task(id) ON DELETE CASCADE,
            tag_id  INTEGER NOT NULL REFERENCES project_tag(id)  ON DELETE CASCADE,
            PRIMARY KEY (task_id, tag_id)
        );
        CREATE INDEX IF NOT EXISTS project_task_tag_rel_tag_idx
            ON project_task_tag_rel (tag_id);

        CREATE TABLE IF NOT EXISTS project_task_watcher_rel (
            task_id INTEGER NOT NULL REFERENCES project_task(id) ON DELETE CASCADE,
            user_id INTEGER NOT NULL REFERENCES res_users(id)    ON DELETE CASCADE,
            PRIMARY KEY (task_id, user_id)
        );
        CREATE INDEX IF NOT EXISTS project_task_watcher_rel_user_idx
            ON project_task_watcher_rel (user_id);
    )SQL"});
}

} // namespace cerp::modules::project

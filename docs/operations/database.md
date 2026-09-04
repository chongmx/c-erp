# Database operations

PostgreSQL. The schema is created and evolved from C++ at boot — there is no
`.sql` file to apply and no external migration directory. See
[../reference/database-schema.md](../reference/database-schema.md).

---

## What "a clean database" means

**One thing: `db/snapshots/baseline.dump`.**

It holds the schema plus the reference data every module seeds — chart of
accounts, journals, units, footprints, task stages, menus, help articles — one
company and the admin user. No transactions, no products, no orders, no test
debris.

It is **not** a dump of whatever the working database happens to contain. It is
built from an empty PostgreSQL database that the application provisions itself,
on a scratch database on a spare port, so the working database is never touched
and the result cannot inherit anything lying around in it.

```bash
./scripts/make_baseline.sh              # rebuild db/snapshots/baseline.dump
./scripts/make_baseline.sh out.dump     # build it somewhere else
```

`scripts/derive_baseline.sh` is the fallback when the database role lacks
`CREATEDB`.

## Snapshots

```bash
./scripts/db_snapshot.sh take    [file]   # default log/pretest.dump
./scripts/db_snapshot.sh restore [file]
./scripts/db_snapshot.sh verify  [file]   # is this a usable dump?
```

A custom-format `pg_dump`, not a `TRUNCATE` list, for three reasons:

- it restores *state* instead of approximating "clean" with a table list;
- it survives a test that changed a **configuration** row — a decimal
  precision, a sequence, a setting — which no reset scope would put back;
- a failed restore leaves the dump on disk to retry from.

> **Stop the server before a restore.** `--clean` drops objects, and open
> connections hold locks that make that fail halfway. `restore` does
> `DROP SCHEMA … CASCADE`: `pg_restore --clean` alone silently skipped tables
> that had dependents.

## The test workflow

`tests/run.sh` does this on every run:

1. snapshots the working database to `log/pretest.dump`;
2. loads `baseline.dump`, so the run starts from identical data;
3. runs the suite;
4. **restores `log/pretest.dump`** — your data comes back exactly as it was.

Flags: `--no-baseline` (run against the working database), `--keep-db` (skip
step 4; the snapshot is still taken), `--baseline <file>`.

See [testing.md](testing.md).

## Backups

In-app, under Settings → Database: backup, restore, download, upload, delete.
`core/DbBackup` runs `pg_dump` / `pg_restore` via **argv arrays**, never a shell
string. It is admin-gated, requires password re-confirmation, and is handed only
the caller's own tenant `DbConfig` — so one company can never dump or restore
another's database.

## Database Tools

Settings → Technical → Database Tools: a table browser, a SQL console and a map
of the schema.

Every query runs inside a `pqxx::read_transaction`, so a crafted
`WITH … INSERT` data-modifying CTE is refused by **PostgreSQL itself** rather
than by a string check. There is deliberately no write path. Table and column
names arriving from the request are allowlisted against the real schema (S-49).

## Tenants

```bash
tools/provision_tenant.sh <name>      # create and provision one tenant database
tools/migrate_all_tenants.sh          # migrate every tenant
./build/c-erp --provision             # boot, provision + migrate all, exit
tools/db_preflight.sh                 # check the database is ready
```

See [../architecture/multi-company.md](../architecture/multi-company.md).

## Admin password

```bash
./scripts/set_admin_password.sh
```

Writes a PBKDF2 hash straight into `res_users`. For recovering access, not for
routine use.

## Housekeeping

```bash
./tests/tools/audit_test_leaks.sh          # row deltas per test: who leaves rows behind
./tests/tools/verify_ledger_integrity.sql  # exact ledger equalities, no epsilon
```

Attachment bytes are content-addressed under `data/filestore/<h[:2]>/<h>` and
reclaimed by a garbage-collection pass — the same content stored twice occupies
one file.

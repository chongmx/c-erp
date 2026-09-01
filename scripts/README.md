# scripts/

Operational scripts: build it, deploy it, run it, reset its database, seed it.

Nothing here is part of the test suite. Tests live in `tests/` and run from
`./tests/run.sh` — see [tests/README.md](../tests/README.md).

## Layout

```
scripts/
  build.sh              build ./build/c-erp and ./build/erp-admin
  deploy.sh             build in Docker, ship to GCP, restart the service
  server.sh             the systemd unit: install / start / stop / logs
  run_tests.sh          forwarder — the suite moved to ./tests/run.sh

  db_snapshot.sh        take / restore / verify a whole-database dump
  make_baseline.sh      build db/snapshots/baseline.dump from an empty database
  derive_baseline.sh    fallback for make_baseline.sh when the role lacks CREATEDB
  set_admin_password.sh write a password hash straight into res_users

  seed.sh               one entry point for every dataset  ->  seed/
  deps/                 everything a fresh machine needs installed
  deprecated/           kept for the record, not for use
```

## First run on a new machine

```bash
./scripts/deps/install.sh          # toolchain, wkhtmltopdf, OWL, PostgreSQL
cmake -B ./build && cmake --build ./build
./scripts/server.sh --install --start
```

Each step is also a script of its own under `deps/`, so a machine that already
has PostgreSQL can skip it with `--no-db`.

## Seeding

```bash
./scripts/seed.sh                  # what exists, and what each dataset is
./scripts/seed.sh parts            # electronics catalogue (docs/098)
./scripts/seed.sh rental           # demo storage facility
./scripts/seed.sh website          # Easy Locker Space CMS pages
./scripts/seed.sh all
```

Every dataset is idempotent and removable (`--clean` / `--clear`), and none of
it is needed to run the suite: `tests/run.sh` restores a clean baseline first
and seeds its own fixtures. Seed when you want to *look* at the application.

## Database state

`db/snapshots/baseline.dump` is what "a clean database" means — schema plus
reference data, one company, the admin user, no transactions.

```bash
./scripts/make_baseline.sh                                   # rebuild it
./scripts/db_snapshot.sh restore db/snapshots/baseline.dump  # reset to it
./scripts/db_snapshot.sh take my.dump                        # capture any state
```

`db_snapshot.sh` is also what `tests/run.sh` uses to put your working database
back after a run, so it is load-bearing for the suite even though it lives here.

## Where the rest went

| was | is now |
|---|---|
| `scripts/install_dep.sh` | `scripts/deps/install_deps.sh` |
| `scripts/install_wkhtml.sh` `setup_frontend.sh` `setup_db.sh` | `scripts/deps/` |
| `scripts/seed_demo_parts.sh` | `scripts/seed/parts.sh` (`./scripts/seed.sh parts`) |
| `scripts/seed_rental_demo.sh` | `scripts/seed/rental.sh` (`./scripts/seed.sh rental`) |
| `scripts/seed_easylocker_site.py` | `scripts/seed/website.py` (`./scripts/seed.sh website`) |
| `scripts/seed_test_fixtures.sh` | `tests/lib/sale_fixture.sh` |
| `scripts/audit_test_leaks.sh` | `tests/tools/audit_test_leaks.sh` |
| `scripts/gen_menu_doc.py` | `tests/tools/gen_menu_doc.py` |
| `scripts/verify_ledger_integrity.sql` | `tests/tools/verify_ledger_integrity.sql` |
| `scripts/test_nginx_proxy.sh` | `tests/security/hardening/nginx-proxy/` |
| `scripts/test_nginx_s40_forge.sh` | `tests/security/hardening/nginx-forge/` |
| twelve one-off scripts | `scripts/deprecated/` — see its README |

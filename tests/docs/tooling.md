# Test tooling — what exists and how to run it

A reference. If you are wondering "is there already a tool for this?", the
answer is probably here.

---

## The one command

```bash
./tests/run.sh                      # everything. This is what CI runs.
```

Snapshots your database → loads `baseline.dump` → runs the suite → **restores
your database**. Exit 0 means green.

| Flag | Does |
|---|---|
| `--unit` | C++ only. No server, no database, milliseconds |
| `--no-unit` | skip the C++ tier, run the rest |
| `--group <g>` | one group: `setup` `integration` `functional` `security` `teardown` (repeatable) |
| `--only <substr>` | every test whose **path** matches, e.g. `--only bank`, `--only product/` |
| `--filter <name>` | passed to `erp_tests` (unit tier only) |
| `--list` | print what would run, in order, with each test's scenario. Runs nothing |
| `--keep-db` | don't restore afterwards — for inspecting a failure |
| `--no-baseline` | run against your working database instead of the baseline |
| `--baseline <f>` / `--scenario <n>` | use a different starting state |

Logs land in `log/tests/<group>_<area>_<name>.log` — one per test, always.

**Run a single test directly** (works from any directory):

```bash
bash tests/integration/account/bank-recon/test.sh
```

---

## Writing a test

A test is a **folder**: `test.sh` + `meta`. Full template in
[../README.md](../README.md). The four lines every test starts with:

```bash
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
```

### `tests/lib/` — what the harness gives you

**assert.sh** — the verdict protocol. The runner scores on OUTPUT, not exit
code, and a **missing** verdict is a failure.

| | |
|---|---|
| `ok <label>` / `no <label>` | pass / fail a check |
| `sec <label>` | section heading |
| `t_eq <expected> <actual> <label>` | prints what it actually got on failure |
| `t_ne`, `t_ge <actual> <min>`, `t_nonempty` | comparisons |
| `t_contains` / `t_lacks <haystack> <needle>` | `t_lacks` is for disclosure tests |
| `verdict` | **last line of every test.** Prints `All checks passed.` or `*** FAILURES ***` |

> Prefer `t_ge`/`t_eq` over `t_ne "0" "$x"`: an **empty** string is also "not
> zero", so a query that errored sails through. That cost three false passes.

**api.sh** — talking to the server.

| | |
|---|---|
| `auth_or_die` | log in, or fail loudly and stop. Standard opening move |
| `call <model> <method> <args-json>` | args is the **array**: `'[{...}]'`, not `'{...}'` |
| `call_k <model> <method> <args> <extra-kwargs>` | for methods reading kwargs |
| `call_as <session> <model> <method> <args>` | as somebody else, or nobody (security tests) |
| `rid` | filter: pull the integer id out of a `create` response |
| `has_error <json>` | matches the error **object**, not the word — payloads legitimately contain `"error":0` |
| `http_get <path>` / `http_code <path>` | non-RPC routes, with the session cookie |

**db.sh** — PostgreSQL.

| | |
|---|---|
| `pg "<sql>"` | query, **strips spaces** — `CT Renamed` comes back `CTRenamed` |
| | `boolean::text` gives `true`/`false`, **not** `t`/`f`. Use `::int` and compare `0`/`1` |
| `pgv "<sql>"` | same but shows errors and keeps formatting |
| `pgid "<sql>"` | for `INSERT … RETURNING id` — takes `head -1`, because psql prints the row **and** the command tag |
| `scenario_load <name>` / `scenario_save <name>` | named database states |

> An empty id from `pgid` almost always means the INSERT **failed** — `pg`
> swallows stderr. Re-run it with `pgv` before suspecting anything else.

**fixtures.sh** — the canonical `FX-` data set (`fx_create`, `fx_drop`,
`fx_report`). Declare `needs=fixtures` in `meta`; the runner seeds it.

**render.mjs** — see below.

---

## Browser rendering

```bash
node tests/lib/render.mjs Products Configuration Categories .ct-shell
```

Arguments: the **menu path to click**, then the selector proving the screen
arrived. Prints a JSON report (rows, sidebar width, detail panel, console
errors), writes a screenshot to `$SHOT` (default `/tmp/render.png`), and
**exits non-zero if the selector never appeared or the console complained**.

Needs `puppeteer-core` (installed) and Chrome at `/usr/bin/google-chrome`.
Worked example: `tests/integration/product/category-tree/test.sh` §9.

**This is the only check that catches an OWL template error** — those are
silent server-side. Read [browser-render-checks.md](browser-render-checks.md)
before writing one; it documents the traps (no hash router, static files
registered at boot, cookie-only sessions).

---

## Databases and snapshots

```bash
bash scripts/db_snapshot.sh take    log/mine.dump     # capture
bash scripts/db_snapshot.sh restore log/mine.dump     # put it back (restarts the server)
bash scripts/db_snapshot.sh verify  log/mine.dump     # is the archive readable?
```

`restore` does `DROP SCHEMA public CASCADE` first — **not** `pg_restore
--clean`, which silently skips tables it cannot drop (docs/109 §9).

| File | Is |
|---|---|
| `db/snapshots/baseline.dump` | what the suite runs against: schema, reference data, one company, admin. **No transactions and no products** — `product_product` is empty, so a test that reads "the first product" must seed it (`needs=fixtures`, or its own rows) |
| `backups/odoo/default-clean.dump` | for the **Database & Backup** screen: runnable ERP, zero products/orders/entries |
| `backups/odoo/default-demo.dump` | clean + the demo catalogue |
| `log/pretest.dump` | your database, as the last suite run found it. **This is the file to restore if something goes wrong** |

Rebuilding data:

```bash
bash scripts/make_baseline.sh        # rebuild db/snapshots/baseline.dump
bash scripts/seed.sh                 # what each dataset is, and what exists now
bash scripts/seed.sh parts           # 163-part catalogue (--clean to remove)
bash scripts/seed.sh rental          # rental demo data (--clear to remove)
```

> **Seed, restart, verify, then dump.** Startup normalisation fills in
> `value_base` and missing product templates; a dump taken straight after
> seeding captures a half-built database.

---

## Unit tests (C++)

```bash
./tests/run.sh --unit                # build + run
./build/erp_tests                    # run directly
./build/erp_tests Money              # filter by name (argv[1], substring)
```

Sources live in `tests/unit/<subject>/`. The CMake source list is a **glob
evaluated at configure time** — a new file needs `cmake -B ./build` re-run once
before it compiles. Rule: **no database, ever.**

---

## Finding problems

```bash
bash tests/tools/audit_test_leaks.sh          # which tests leave rows behind
bash tests/tools/audit_test_leaks.sh bank     # just the matching ones
bash tests/integration/core/menu-ids/test.sh  # menu/action id collisions, + next free id
psql … -f tests/tools/verify_ledger_integrity.sql
```

`audit_test_leaks.sh` measures row deltas per test rather than reading cleanup
blocks — every script *had* a cleanup when an invoice leak ran for eleven days.

---

## The server

```bash
pkill -x c-erp; sleep 2
(setsid ./build/c-erp > /tmp/cerp_run.log 2>&1 < /dev/null &)
```

`pkill -x` matches the executable name **exactly**. Never `pkill -f` on a test
path — it matches your own command line and reads as a mysterious exit 143.

Logs: `log/system.log` (rotates on restart, so grep the newest
`log/system.*.log` after a crash). Errors are masked in HTTP responses by
SEC-28; **the real message is only in the log.**

---

## Recipes

| I want to… | Run |
|---|---|
| See everything is fine | `./tests/run.sh` |
| Iterate on one test | `bash tests/<path>/test.sh` |
| See what a failure did to the database | `./tests/run.sh --only x --keep-db`, then inspect |
| Check a screen renders | `node tests/lib/render.mjs <menus…> <selector>` |
| Reset to a clean ERP | restore `default-clean.dump` from Database & Backup |
| Get my data back after a bad run | `bash scripts/db_snapshot.sh restore log/pretest.dump` |
| Know what runs and in what order | `./tests/run.sh --list` |

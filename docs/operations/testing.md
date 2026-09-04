# Testing

The suite documents itself, inside `tests/`. This page orients you and points
at the right file — it deliberately does not restate them, because a copy here
would rot the first time a flag changed.

| Read this | For |
|---|---|
| [`tests/README.md`](../../tests/README.md) | the layout, the `meta` file, how to write a test |
| [`tests/docs/tooling.md`](../../tests/docs/tooling.md) | **every tool, flag and helper — and the trap each one hides** |
| [`tests/docs/browser-render-checks.md`](../../tests/docs/browser-render-checks.md) | driving a real browser; the only way to catch an OWL template error |
| [`tests/docs/test-plan.md`](../../tests/docs/test-plan.md) | what a real test of a page has to do |
| [`tests/docs/menu-coverage.md`](../../tests/docs/menu-coverage.md) | every page, its model, and the tests that touch it (generated) |

**Read `tests/docs/tooling.md` before writing or debugging a test.** `pg()`
strips spaces, `has_error` matches the error *object*, an empty id means the
INSERT failed — each of those has cost someone an afternoon.

---

## Running it

```bash
./tests/run.sh                              # everything — this is what CI runs
./tests/run.sh --unit                       # C++ only: no database, no server, milliseconds
./tests/run.sh --group security             # one group
./tests/run.sh --only bank                  # every test whose path matches
./tests/run.sh --list                       # what would run, in order, and what state each demands
./tests/run.sh --keep-db                    # don't restore afterwards (debugging)

bash tests/integration/account/bank-recon/test.sh    # one test, from anywhere
./build/erp_tests Tax                                # one unit suite
```

`scripts/run_tests.sh` still works — it forwards to `tests/run.sh`.

Exit 0 means green. **All tests are expected to pass; any failure is a
regression**, not a known-bad.

## The two tiers, and why both

| | Where | Rule |
|---|---|---|
| **unit** | `tests/unit/<subject>/*.cpp`, registered with `ERP_TEST` | pure functions only. **Never let these acquire a database dependency** — running in milliseconds with nothing else alive is the whole value |
| **integration / functional / security** | `tests/**/test.sh` | drive the real HTTP API against real PostgreSQL |

The second tier catches what the first structurally cannot: migrations, field
registration, SQL, and whether the wiring between them is connected at all.

Groups run in a fixed order — `setup`, `integration`, `functional`, `security`,
`teardown` — and within a group by the declared `order`, then path.

## A test is a folder

Holding a `test.sh` and a `meta` that declares its group, order, database
`scenario` and whether it `needs=fixtures`. Order and database state are
**declared**, not implied by filename, so a rename cannot silently reshuffle the
suite.

## Database state

Every run snapshots your working database, loads `baseline.dump`, runs, and
restores your snapshot. Your data comes back exactly as it was. See
[database.md](database.md).

The baseline holds schema, reference data, the demo parts catalogue (**163
products**), 13 partners, one company and admin — and **no transactions at
all**: zero orders, invoices, pickings, contracts. So a script that assumes an
*order* exists is not testing what it claims to; either declare
`needs=fixtures` or seed its own. Running against the baseline is how you find
out.

Do not infer those counts from `pg_restore --data-only -t <table>`: it reports
0 rows for a custom-format dump whatever the dump contains. The numbers above
were measured by restoring it and counting. `tests/docs/tooling.md` carries the
same note next to each snapshot file.

## Three things that bite

- **Every test must end with `All checks passed.` or `*** FAILURES ***`.** The
  runner scores on that line rather than the exit code, and treats a **missing**
  verdict as a failure — so a test that dies early can never be scored as a pass.
- **`erp_tests` is not in the default build target.** `cmake --build ./build`
  stays the fast path; `tests/run.sh` builds it explicitly. Its source list is a
  glob evaluated at **configure** time, so a newly added unit file needs
  `cmake -B ./build` re-run once.
- **Never report a screen as working on API tests alone.** An OWL template error
  is silent server-side: every RPC returns 200 while the panel renders nothing.
  Three harnesses exist so this is cheap; `tests/docs/browser-render-checks.md`
  covers the traps they work around.

  ```bash
  node tests/lib/render.mjs Products Configuration Categories .ct-shell
  node tests/lib/render_forms.mjs --all   # open every FORM, report console errors
  node tests/lib/render_pick.mjs          # type into a picker, choose, clear, page
  ```

  `render.mjs` reaches a screen by clicking menus, which lands on a **list** —
  a template error in a *form* walks straight past it, which is why
  `render_forms.mjs` exists. And rendering is not working: a dropdown that
  opens behind the card renders perfectly, so `render_pick.mjs` drives one.

## By-hand instruments

`tests/tools/` — run by you, not by the runner.

```bash
./tests/tools/audit_test_leaks.sh            # row deltas per test: who leaves rows behind
./tests/tools/audit_schema_doc.sh            # is reference/database-schema.md still true?
./tests/tools/audit_doc_links.sh             # do the docs point at files that exist?
python3 tests/tools/gen_menu_doc.py          # regenerate tests/docs/menu-coverage.md
psql -f tests/tools/verify_ledger_integrity.sql
```

Nothing test-related belongs in `scripts/` — that folder is operational only.

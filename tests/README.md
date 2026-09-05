# tests/

One entry point, one layout, and a `meta` file that says out loud what each
test needs.

```bash
./tests/run.sh                      # everything — this is what CI runs
./tests/run.sh --unit               # C++ only: no server, no database
./tests/run.sh --group security     # one group
./tests/run.sh --only bank          # every test whose path matches 'bank'
./tests/run.sh --list               # what would run, in order, and why
./tests/run.sh --keep-db            # don't restore afterwards (for debugging)
```

A single test also runs on its own, from anywhere:

```bash
bash tests/integration/account/bank-recon/test.sh
```

`scripts/run_tests.sh` still works — it forwards here.

**Every tool, every flag, every helper: [docs/tooling.md](docs/tooling.md).**

## Layout

```
tests/
  run.sh          the runner: discovery, ordering, scenarios, verdicts
  docs/           environment knowledge that is not derivable from the code
  lib/            the shared harness, sourced by every shell test
    assert.sh       ok / no / t_eq / verdict — and the verdict protocol
    api.sh          auth, call, call_as, http_get
    db.sh           pg / pgid, scenario_load, scenario_save
    fixtures.sh     fx_create / fx_drop / fx_report — the canonical set
    sale_fixture.sh ensure_sale_fixture — one sale order, for tests that need
                    only that and do not want the whole canonical set
    render.mjs      the browser driver (see docs/browser-render-checks.md)
    render_*.mjs    one click-driven journey each, called by a functional test
    testlib/        vendored python helpers (segno, qrcheck) for QR checks
  tools/          run by hand, not by the runner — they measure the suite
    audit_test_leaks.sh         row deltas per test: who leaves rows behind
    gen_menu_doc.py             regenerates docs/menu-coverage.md
    verify_ledger_integrity.sql exact ledger equalities, no epsilon
  unit/           C++ only. No database, ever. Milliseconds.
    money/ tax/ session/
  setup/          creates the canonical fixtures, and asserts the creation
  integration/    per-area technical checks
    core/ money/ account/ sale/ purchase/ stock/ product/ mrp/ project/ rental/
  functional/     whole journeys, driven by CLICKING a real browser
    account/ base/ core/ mrp/ portal/ product/ project/ purchase/
    rental/ sale/ stock/
  security/       penetration tests — these assert that things FAIL
    auth/ access/ injection/ hardening/
  teardown/       deletes the fixtures, and asserts the deletion
```

**A test is a folder, not a file.** That is what lets it carry whatever it
needs: `test.sh`, a `seed.sql`, a `helper.cpp`, an `expected/` directory of
golden output. Nest one level deeper if a test grows enough parts to need it.

**Both tiers are grouped by module, and neither encodes order in a name.**
Functional tests were once `01-sell`, `02-buy`, … `17-partner-display-name`,
which stopped scaling the moment there were several per area: the number said
where a test ran but not what it covered, and five different concerns sat in
one flat list. Run order is `order=` in the `meta`, and it is unchanged by the
move — so a folder can be renamed or re-filed without touching the sequence.

### Functional tests are click-driven

A functional test drives a real browser and asserts what is **on the screen**.
It does not create its records over the API — a record planted by one `create`
call is not the record a user has, and the difference is where the bugs live: a
picker that never offered the row, a dialog that could only be cancelled, a
dropdown that was a text box. Where a large starting state is needed, load a
prefabricated scenario once through the database restore *page*, and restore at
the end.

The shell test owns the fixtures, the cleanup and the verdict, and re-checks
the database with `pg` afterwards — so a driver that stopped early cannot pass
by saying nothing.

## The `meta` file

```
group=integration     setup | integration | functional | security | teardown
order=20              position within the group (ties break on path)
scenario=baseline     the database state this test demands
needs=fixtures        the canonical set, or `none`
provides=fixtures     it creates them itself (the setup group only)
timeout=300           seconds before the runner kills it as wedged
skip=yes             temporarily exclude it — and say why in a comment
```

Groups run in a fixed order — setup, integration, functional, security,
teardown — and within a group by `order`, then path. Order used to be implied
by filename; declaring it means a rename cannot silently reshuffle the suite.

### Scenarios

`scenario=` names a database state from `db/snapshots/`:

| value | meaning |
|---|---|
| `baseline` | `db/snapshots/baseline.dump` — schema, reference data, one company, no transactions |
| `current` | whatever is in the database; no restore |
| `<name>` | `db/snapshots/<name>.dump` |
| `path/to.dump` | an explicit file |

The runner restores **only when the name changes**, so twenty tests that all
want `baseline` cost one restore, not twenty.

Note what that promises: the same *starting* state for a run, not isolation
between tests within it. Tests still see each other's rows — which is why the
canonical fixtures are prefixed `FX-` and why a test that needs rows of its own
seeds them itself.

To test a corner case against a large or awkward database, build the state
once, capture it, and name it:

```bash
bash scripts/db_snapshot.sh take db/snapshots/large-catalogue.dump
# then in the test's meta:  scenario=large-catalogue
```

## Writing a test

```bash
mkdir -p tests/integration/stock/reservation
cat > tests/integration/stock/reservation/meta <<'EOF'
group=integration
order=35
scenario=baseline
needs=fixtures
timeout=300
EOF
```

```bash
#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
auth_or_die

sec "reservation"
ID=$(call stock.picking create '[{"name":"QA-PICK"}]' | rid)
t_nonempty "$ID" "picking created"
t_eq "1" "$(pg "SELECT count(*) FROM stock_picking WHERE id=$ID")" "row present"

verdict
```

That preamble walks up for `CMakeLists.txt` instead of counting `../`, so the
test behaves identically whether the runner invoked it or you ran it directly,
and it can be nested a folder deeper without an edit. Every relative path in a
test therefore resolves from the repository root.

### The rules that are not negotiable

- **End with a verdict.** `verdict` prints `All checks passed.` or
  `*** FAILURES ***`. The runner scores on that line, not on the exit code —
  and a **missing** verdict is a failure, so a test that dies at line 40 is
  reported as broken instead of quietly vanishing from the suite.
- **Seed what you read.** The baseline has zero products and zero orders. A
  test that looks up "the first product" and assumes one exists is not testing
  what it claims to; either declare `needs=fixtures` or create your own.
- **Clean up what you create.** Use a distinctive prefix and delete it on the
  way out (`trap cleanup EXIT`). `tests/tools/audit_test_leaks.sh` reports who
  leaves rows behind.
- **Never read another test's debris.** The restore stops it accumulating, but
  a test that depends on it is broken regardless.
- **Unit tests never touch the database.** That tier's whole value is that it
  runs in milliseconds with nothing else alive.
- **Never report a screen as working on API tests alone.** An OWL template
  error is silent server-side: every RPC returns 200 while the panel renders
  nothing. See [docs/browser-render-checks.md](docs/browser-render-checks.md)
  before writing or trusting any test that covers a screen.

## Adding a unit test

Drop a `.cpp` under `tests/unit/<subject>/`, register cases with `ERP_TEST`,
then re-run `cmake -B ./build` — the source list is a glob evaluated at
configure time, so a new file needs one re-configure before it compiles.

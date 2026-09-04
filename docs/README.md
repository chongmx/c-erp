# c-erp documentation

These pages describe **how the system is now**. They are not a change log —
nothing here says "what was wrong and what I did about it". If a page and the
code disagree, the code is right and the page is a bug.

The historical record moved to [`deprecated/`](deprecated/) and is frozen.
Work in progress — a plan being executed, an investigation still open — goes in
[`temp/`](temp/) and is deleted once it lands in the pages below.

---

## Start here

| | |
|---|---|
| [architecture/overview.md](architecture/overview.md) | the layers, the request path, boot order, the engines in `core/` |
| [development/conventions.md](development/conventions.md) | the rules every new file follows, and the mistake each prevents |
| [security/README.md](security/README.md) | what is enforced and where |
| [operations/build-and-run.md](operations/build-and-run.md) | build it, run it, seed it |

Also `CLAUDE.md` at the repository root — the mandatory subset of the
conventions plus the build, test and database commands.

## Architecture

| | |
|---|---|
| [overview.md](architecture/overview.md) | layers, request path, boot, schema evolution |
| [modules.md](architecture/modules.md) | the 20 modules: what each one owns |
| [frontend.md](architecture/frontend.md) | the OWL app, the portal, the kiosk, the public site |
| [multi-company.md](architecture/multi-company.md) | record rules, tenants, the control plane |

## Reference

| | |
|---|---|
| [database-schema.md](reference/database-schema.md) | 128 tenant tables (plus 2 in the control plane), by module, with columns |
| [http-api.md](reference/http-api.md) | JSON-RPC, the access model, every route |
| [id-registry.md](reference/id-registry.md) | menu, action and group ids — **and how to pick the next one** |
| [part-lookup-api.md](reference/part-lookup-api.md) | the contract with a part-lookup agent |

## Development

| | |
|---|---|
| [conventions.md](development/conventions.md) | module split, models, ViewModels, SQL, OWL, naming |
| [document-layout-editor.md](development/document-layout-editor.md) | the DLE internals, and how to add a block property |

## Security

| | |
|---|---|
| [README.md](security/README.md) | injection, authorization, sessions, rate limiting, uploads |
| [error-handling.md](security/error-handling.md) | SEC-28 — never leak `ex.what()` to a response |

## Operations

| | |
|---|---|
| [build-and-run.md](operations/build-and-run.md) | build, run, service, seed data |
| [configuration.md](operations/configuration.md) | every key in `config/system.cfg` |
| [database.md](operations/database.md) | the baseline, snapshots, backups, tenants |
| [testing.md](operations/testing.md) | how to run the suite, and where its own docs are |
| [deployment.md](operations/deployment.md) | topology, cross-build, verification |

## Guides

| | |
|---|---|
| [document-templates.md](guides/document-templates.md) | customising invoice / order / delivery PDFs |

## Working notes

| | |
|---|---|
| [temp/](temp/) | work in progress, with a shelf life — empty is the healthy state |

## Documentation that lives elsewhere

Some documentation is deliberately kept beside the thing it describes, so it
cannot drift:

| | |
|---|---|
| [`tests/README.md`](../tests/README.md) | the test layout and the `meta` file |
| [`tests/docs/tooling.md`](../tests/docs/tooling.md) | every test tool, flag and helper — **read before writing a test** |
| [`tests/docs/browser-render-checks.md`](../tests/docs/browser-render-checks.md) | driving a real browser |
| [`tests/docs/menu-coverage.md`](../tests/docs/menu-coverage.md) | every page and its tests (generated) |
| [`scripts/README.md`](../scripts/README.md) | every operational script |
| [`deploy/README.md`](../deploy/README.md) | the production runbook |

## Keeping these honest

Several pages are derived from the source and will drift if the source moves:

- `reference/database-schema.md` — the table and column lists come from the
  `CREATE TABLE` / `ALTER TABLE` statements in `core/` and `modules/`.
- `reference/id-registry.md` — the id ranges come from the seeds. **Never pick
  an id from that page**; run `bash tests/integration/core/menu-ids/test.sh`,
  which prints the next free one and fails on a collision.
- `architecture/modules.md` — model and table counts come from
  `registerCreator(...)` and `CREATE TABLE`.

When you add a module, a table or a menu, update the page in the same change.
Two of those pages can be checked rather than trusted:

```bash
./tests/tools/audit_schema_doc.sh    # database-schema.md vs a live database
./tests/tools/audit_doc_links.sh     # every link and backticked path resolves
```

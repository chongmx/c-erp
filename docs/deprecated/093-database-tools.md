# 093 — Database Tools: a browser, a SQL console, and a map of the schema

Settings ▸ **Database Tools** — look inside the company's own database without
leaving the app or opening a psql session.

Suite: **65 passed, 0 failed** (`verify_db_tools.sh`, 51 checks).

Three tabs, answering three different questions:

| Tab | Question |
|---|---|
| **Browse** | *What is actually in this table?* — every table, its rows, columns, keys, indexes and both directions of every foreign key. Filter, sort, page, and click a foreign key to follow it. |
| **SQL** | *Let me just ask.* — a read-only console with the PostgreSQL error text intact. |
| **Schema** | *What shape is this database?* — size and row counts, storage by module, and a foreign-key map. |

---

## 1. Why this is safe to put in the product

An admin screen that runs SQL against the live company database earns its
scrutiny. The defence is layered, and the layers are ordered by how much work
they actually do.

### The read-only transaction does the real work

Every call runs inside `pqxx::read_transaction` — a genuine `BEGIN READ ONLY` —
and the transaction is **never committed**. PostgreSQL itself refuses every
write, including the ones a keyword filter would wave through:

```
WITH d AS (DELETE FROM ir_attachment RETURNING id) SELECT count(*) FROM d
```

That statement starts with `WITH`. Any check that reads the first token lets it
past. The transaction does not.

The test asserts this the only way that can't pass by accident:

```bash
q "SELECT nextval('res_users_id_seq')"
# -> ERROR: cannot execute nextval() in a read-only transaction
```

`nextval()` is ordinary read-shaped SQL. Nothing blocks it except a real READ
ONLY transaction, so the check fails the moment that guarantee is lost — rather
than staying green because some *other* layer happened to catch the DELETE.

### Identifiers are resolved, never merely filtered (S-49)

A table or column name from the browser is looked up in `pg_catalog` first. What
gets quoted into SQL is the name **PostgreSQL handed back**, not the name the
client sent. A charset check would stop injection and still let a caller name
any relation in the cluster; resolution against the `public` schema is what stops
that. Comparison values are always bound parameters.

### And then the rest

- `SET LOCAL statement_timeout = '15s'` — a cartesian join cannot pin a pool
  connection. The timeout returns advice, not a stack trace.
- One statement per box. The scanner tracks string literals, `--` and `/* */`
  comments and `$tag$` quoting, so `SELECT 'a;b'` is fine and
  `SELECT 1; DROP TABLE res_users` is not.
- Leading keyword must be SELECT / WITH / TABLE / VALUES / EXPLAIN / SHOW.
- `pg_authid` and `pg_shadow` (role password hashes) are refused by name, as are
  the server-side file functions — each already unreachable, each now producing a
  sentence instead of a permission error.
- Results are capped: 500 rows per page when browsing, 1000 from the console,
  and the console says when it truncated.
- Columns named `password`, `token`, `api_key`, `secret` and friends are replaced
  with `••••••••` **server-side**. The test asserts the real bcrypt prefix never
  appears in the response body.
- Admin-only, and scoped to the caller's own tenant via `TenantScope` — the same
  envelope as `/web/db/*` in docs/075. One company can never read another's rows.
- Console queries are written to the audit log.

### One deliberate SEC-28 exception

SEC-28 says mask `ex.what()`. The SQL console is the one place where the
PostgreSQL error *is* the product — `column "x" does not exist` is what the user
came for, and an authenticated admin can already read the whole schema through
this very screen. So `pqxx::sql_error` is passed through **for `op == "query"`
only**; every other op still masks it behind `devMode`. `ValidationError` carries
the deliberate, user-facing refusals and is always passed through, as it is
everywhere else.

---

## 2. What the screen does

**Browse.** Sidebar groups tables by module with row counts; views are
italicised. The grid marks the primary key, prints each column's type under its
name, dims NULLs, and renders foreign keys as links — clicking one opens the
target table filtered to that row. The footer shows the exact SQL that produced
what you are looking at.

`count(*)` is exact up to ~200k rows and falls back to the planner's estimate
above that, flagged with `~`, rather than stalling the screen on a table scan.

**Columns** doubles as a profiler: pick a column and get null share, distinct
count, min/max/avg for numerics, and the most common values as bars.

**SQL.** Ctrl/Cmd+Enter runs. Six starter snippets (biggest tables, unbalanced
journal entries, tables with no primary key, unused indexes…), last ten queries
remembered, copy-as-CSV.

**Schema.** Stat tiles, storage by module, largest tables, and the foreign-key
map in two modes:

- *By module* — an **arc diagram**: the 19 modules on one axis, arc thickness =
  number of foreign keys. The first attempt was a ring layout, and this schema
  breaks it: 19 boxes at 118px need ~2,200px of circumference, which a circle
  that fits on screen does not have, so the nodes overlapped. An arc diagram
  grows sideways instead of collapsing inward.
- *Around one table* — the chosen table centred, what it references on the right,
  what references it on the left, each edge labelled with the FK column.
  Direction is in the layout, not just the arrowheads. Click to re-centre.

### Colour

Eight module hues, and they are the same eight everywhere — the dot in the
sidebar, the bar in "storage by module", the node in the map. Colour follows the
**entity**, assigned from a fixed alphabetical order of module names, so
filtering the list never repaints anything.

They were computed, not eyeballed. Every hue is ≥3:1 against `--surface`
(`#16213e`), and every adjacent pair clears ΔE 8 under simulated protanopia,
deuteranopia and tritanopia (≥15 for normal vision). The first candidate set
failed two pairs at the tail; re-stepping to warm/cool alternation fixed both. A
ninth module folds into a neutral rather than inventing a hue that would fail the
same check.

---

## 3. Bugs found while building it

Three, all found by actually rendering the thing rather than assuming it worked.
The component was mounted headless in Chrome against fixtures captured from the
live API, and every tab screenshotted.

**Every bar chart was invisible.** `.dbs-bar-fill` is a `<span>`, and `width` /
`height` do nothing on an inline box. The tracks rendered; the fills did not.
One `display: block`. This is the kind of defect that ships when a component is
declared done because the JSON was right.

**The foreign-key map drew no arcs at all.** A stray **NUL byte** had landed in
the source: `const k = a + '\0' + b;` instead of a space. Keys were built with
NUL, `k.split(' ')` never split them, and every lookup missed — so the map
silently rendered zero of its 40 edges. `node --check` passed, Chrome parsed it,
and the page looked plausible. The fix removes the byte *and* the parsing: the
map value now carries both module names, so nothing is re-split and no separator
can go wrong again. All touched files are now NUL-scanned.

**The arcs bowed downward** off the bottom of the box, with their labels floating
in the empty space above. SVG's y-axis points down, so sweeping left→right
clockwise (flag 1) is what rises — I had the flag inverted.

A fourth came from the test suite rather than the screenshots: a query ending in
a `-- comment` swallowed the closing paren of the LIMIT wrapper. The wrapper's
newlines are load-bearing.

---

## 4. Files

| File | |
|---|---|
| `core/DbExplorer.{hpp,cpp}` | the seven read-only ops; identifier resolution; the statement gate |
| `core/infrastructure/JsonRpcDispatcher.hpp` | `/web/dbtool`, admin gate, read-only transaction, timeout, audit |
| `web/static/src/components/DbStudio.js` | the screen + `DbSchemaMap` |
| `web/static/src/components/dbstudio.css` | styles + the validated module palette |
| `web/static/src/services/rpc.js` | `RpcService.dbTool()` |
| `modules/report/ReportModule.cpp` | action **101**, menu **74** under Settings (seq 46) |
| `scripts/verify_db_tools.sh` | 51 checks |

Ids checked with `scripts/verify_menu_ids.sh` — no collisions; next free are now
menu 75 / action 102.

---

## 5. Not built

- **No write path.** Not "writes are blocked" — there is no code that writes.
  Editing a row from here would mean giving up the READ ONLY transaction, which
  is the entire safety argument. Records are edited through their real forms,
  where validation and audit live.
- **No cross-tenant browsing.** Deliberate, and the same rule as docs/075.
- **No saved/shared queries.** History is per-browser (localStorage). Saved
  queries would want a table, ownership and sharing rules — a real feature, not a
  side effect of this one.

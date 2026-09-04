# 123 — Migrating the name to c-erp

The product is c-erp. This records what was renamed, what was deliberately not,
and what is left.

---

## 1. Done

| What | Scale |
|---|---|
| **C++ namespace** `odoo::` → `cerp::` | **157 files**; `namespace odoo` → `namespace cerp`, every `odoo::` qualification, every `using namespace odoo` |
| **Product name in user-visible strings** | the login card, the home title, `app.css`, `rpc.js` — `odoo-cpp` → `c-erp` |
| **Login subtitle** | "C++ Backend · Odoo 19 Compatible" → "C++ ERP · fast by construction" |
| **JSON-RPC error names** | `odoo.exceptions.*` → `cerp.exceptions.*`, changed on **both** sides in the same pass — the frontend matches on these strings, so a one-sided rename would have broken error handling silently |
| **Prose** in our own comments, docs and scripts | benchmark references became "the reference ERP" rather than being deleted, which would have left dangling clauses |

The namespace rename is compiler-verified: it either builds or it does not, and
it builds.

---

## 2. Deliberately not renamed

**`zzref/`, `zzref2/odoo14`, `zzref3/`** — vendored third-party source, kept for
reference. It *is* Odoo; renaming it would make it a lie and would break every
citation that points into it. These are not part of the shipped product and
nothing in `core/`, `modules/` or `web/` depends on them.

---

## 3. Not yet done: the database name

The PostgreSQL database is still called `odoo`, and this is the one remaining
place the old name is user-visible — it is the default in the login card's
**Database** field.

It was left because it is a **data migration, not a rename**, and doing it
blind at the end of a session would have been reckless. It touches:

* the live database itself (`ALTER DATABASE odoo RENAME TO cerp`, which needs
  every connection closed);
* `config.json`;
* `db/snapshots/baseline.dump` and every other snapshot — a dump carries its
  own database name, so the baseline has to be rebuilt or restored under the
  new name;
* roughly **96 files** across `tests/` and `scripts/` that name the database,
  including `DBN` defaults in the browser drivers;
* the frontend defaults in `rpc.js` and `LoginPage.js`.

### The order it has to happen in

1. Stop the server.
2. `ALTER DATABASE odoo RENAME TO cerp` (no open connections).
3. `config.json` → the new name.
4. `rpc.js` and `LoginPage.js` defaults.
5. `tests/` and `scripts/` — the `DBN`/`db` defaults.
6. **Rebuild `baseline.dump`** with `scripts/make_baseline.sh`, because the
   existing dump restores under the old name.
7. Full suite. `tests/run.sh` snapshots the working database first, so a failed
   attempt is recoverable.

Worth doing as its own change with nothing else in flight, so that if the suite
goes red the cause is unambiguous.

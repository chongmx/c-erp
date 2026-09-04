# Conventions

The rules every new file follows, and the mistakes each one exists to prevent.
`CLAUDE.md` at the repository root carries the mandatory subset; this page is
the long form.

Everything is in namespace `cerp` — `cerp::core`, `cerp::infrastructure`,
`cerp::modules::<module>`.

---

## PERF-E — the module file split

**Mandatory.** Every module is a slim `.hpp` (declaration only) plus a `.cpp`
(implementation, and every inner class).

```cpp
// modules/xxx/XxxModule.hpp — no implementations, no heavy includes
#pragma once
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace cerp::modules::xxx {

class XxxModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "xxx"; }
    explicit XxxModule(core::ModelFactory&, core::ServiceFactory&,
                       core::ViewModelFactory&, core::ViewFactory&);
    std::string              moduleName()   const override;
    std::string              version()      const override;
    std::vector<std::string> dependencies() const override;
    void registerModels()     override;
    void registerServices()   override;
    void registerViews()      override;
    void registerViewModels() override;
    void registerRoutes()     override;
    void initialize()         override;
private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;
    void ensureSchema_();
    void seedMenus_();
};

} // namespace cerp::modules::xxx
```

```cpp
// modules/xxx/XxxModule.cpp — inner classes and every method body
#include "XxxModule.hpp"
#include "BaseModel.hpp"          // the heavy includes live here
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

namespace cerp::modules::xxx {
using namespace cerp::infrastructure;
using namespace cerp::core;

class XxxFoo : public BaseModel<XxxFoo> { /* ... */ };

XxxModule::XxxModule(/*...*/) : /*...*/ {}
void XxxModule::registerModels() { /* ... */ }

} // namespace cerp::modules::xxx
```

**Why:** incremental compilation. Changing a module's `.cpp` recompiles one
translation unit, not the codebase. `pqxx`, `BaseModel` and `nlohmann/json` stay
out of headers so `main.cpp` compiles fast.

`CMakeLists.txt` globs `*.cpp` from `main.cpp`, `core/`, `modules/` and
`factories/` at **configure** time — a new file needs `cmake -B ./build` re-run
once before it compiles.

## Models

### Never call `registerFields()` in a derived constructor

`BaseModel<TDerived>` calls it via `std::call_once`. The derived constructor
body must be **empty**.

```cpp
// CORRECT
explicit StockMove(std::shared_ptr<DbConnection> db)
    : BaseModel<StockMove>(std::move(db)) {}

// WRONG — throws "FieldRegistry: duplicate field" at runtime
explicit StockMove(std::shared_ptr<DbConnection> db)
    : BaseModel<StockMove>(std::move(db)) { registerFields(); }
```

### `FieldRegistry` is static per model type

`inline static FieldRegistry fieldRegistry_{}` plus `std::call_once` — once per
template specialisation, not once per request. No instance-level registrations.

Field names must be ASCII identifiers; `FieldRegistry::add()` enforces
`[a-zA-Z_][a-zA-Z0-9_]*` at boot. That is what makes the registry safe to use as
the SQL column allowlist (S-49).

### A constrained value set is a `Selection`, not a `Char`

If a CHECK constraint limits a column to a fixed list, register the field as
`FieldType::Selection` and give it that list. The generic form then draws a
combobox; registered as `Char` it draws a free text box and the user has to
guess the spelling, with a raw constraint violation as the only feedback.

```cpp
core::FieldDef bp{"billing_period", FieldType::Selection, "Billing Period"};
bp.selection = { {"monthly", "Monthly"}, {"quarterly", "Quarterly (3 months)"} };
fieldRegistry_.add(bp);
```

Keep `validate()` in step, so an invalid value is a 400 naming the choices
rather than a 500 quoting SQL. The choices, the CHECK and `validate()` are
three copies of one list — changing one alone is the bug.

### `normalizeForDb_` handles `false` → NULL and `[id,"Name"]` → id

`write()` and `create()` call it automatically. In `deserializeFields()`, use
`m2oToId_()` for Many2one fields:

```cpp
void deserializeFields(const nlohmann::json& j) override {
    if (const int v = m2oToId_(j, "partner_id")) partnerId = v;
}
```

### A new column on an existing table needs an ALTER

`CREATE TABLE IF NOT EXISTS` runs once and never again. Put the `ALTER` beside
the column definition in `ensureSchema_()`:

```cpp
txn.exec("ALTER TABLE my_table ADD COLUMN IF NOT EXISTS new_col INTEGER");
```

### Versioned migrations

For a data transformation rather than a shape change, register a numbered
migration instead. Versions are globally unique integers; the reserved ranges
are documented on `IModule::registerMigrations`:

```
1–99     core / base        400–499  stock
100–199  account            500–599  mrp
200–299  sale               600–699  portal / report
300–399  purchase           700–799  auth / auth_signup
```

In practice the registered blocks are: `1–14` (audit log, partner hierarchy),
`800–817` (rental), `901–972` (the Money scale-6 conversion), `980–1041`
(sequences, cron and later work). Four files register them —
`core/MoneyMigrations.cpp`, `modules/base/PartnerMigrations.cpp`,
`modules/ir/IrModule.cpp`, `modules/rental/RentalMigrations.cpp`.

`registerMigrations()` runs **after** every module's `register*()` sequence and
**before** any `initialize()`.

### Widening a CHECK: drop it by the name the database reports

A row must satisfy **every** CHECK on its table. Adding a wider constraint
beside an older narrow one does not widen anything — the effective rule is the
intersection, and the new values are still refused.

```sql
-- WRONG: drops a name that does not exist, so the old constraint survives
ALTER TABLE t DROP CONSTRAINT IF EXISTS t_new_mode_chk;
ALTER TABLE t ADD  CONSTRAINT t_new_mode_chk CHECK (mode IN ('a','b','c'));
```

`DROP CONSTRAINT IF EXISTS` is silent when the name is wrong, which is what
makes this survive review. Look the real name up first:

```sql
SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint
 WHERE conrelid = 't'::regclass AND contype = 'c';
```

This has now happened twice in `rental`: migration 816 guessed
`rental_contract_billing_period_chk` when the constraint was
`rental_contract_period_chk`, and 815 guessed `rental_line_billing_mode_chk`
when it was `rental_cl_billing_mode_chk` — so one-off and on-demand lines were
rejected by a constraint nobody knew was still there, and no test caught it
because the values were only ever exercised at contract level. Migration 817
cleans that up.

## ViewModels

### `REGISTER_METHOD` dispatch

```cpp
REGISTER_METHOD("search_read", handleSearchRead)
REGISTER_METHOD("create",      handleCreate)
```

Map `search_read` and `web_search_read` to the same handler; likewise `read` and
`web_read`. Use `REGISTER_MUTATOR` for methods that write.

### Handlers return plain records

`search_read` and `read` return `[{id, field, …}]` **directly**. Never wrap in
`{arch, fields, record}` — that shape belongs to `get_views` alone.

### `views_.fields()` must list everything the frontend needs

`BaseView` subclasses return a hardcoded JSON from `fields()`. It is **not**
generated from the model registry, so a One2many field such as `order_line` must
be listed explicitly or the frontend never sees it.

### A list view needs a registered View class

Every model shown in the sidebar needs both:

```cpp
views_.registerView<MyModelListView>("my.model.list");
views_.registerView<MyModelFormView>("my.model.form");
```

Without the list view, `get_views` returns `fields: {}` and `ListView` renders
zero columns.

### Resolve the session from `ServiceFactory`

```cpp
viewModels_.registerCreator("res.users", [&sf, db] {
    auto sessions = sf.sessions();          // the SHARED store
    return std::make_shared<AuthViewModel>(auth, sessions, db, db->config().name);
});
```

**Never** `std::make_shared<infrastructure::SessionManager>()` inside a factory
lambda — that builds an empty per-request store and every lookup returns
`nullopt`.

Then, in a handler:

```cpp
infrastructure::Session callerSession_(const core::CallKwArgs& call) const {
    const std::string sid = call.kwargs.contains("context")
        ? call.kwargs["context"].value("session_id", std::string{})
        : std::string{};
    if (!sid.empty()) if (auto s = sessions_->get(sid)) return *s;
    return infrastructure::Session{};
}
```

## Routes

### Pool exhaustion → 503

```cpp
} catch (const PoolExhaustedException& e) {
    LOG_ERROR << "[route] pool: " << e.what();
    cb(htmlError(503, "The server is temporarily overloaded. Please retry."));
}
```

Place it **above** `catch (const std::exception&)` in every route that acquires
a database connection.

### Every module route is registered directly with drogon

The `HttpServer` helpers (`addJsonPost`, `addJsonPostWithResponse`,
`addJsonGet`) are used by `JsonRpcDispatcher` alone — they carry the `/web/...`
JSON-RPC surface. **Every route a module registers goes straight to
`drogon::app().registerHandler()` and therefore inherits nothing.**

Nine files do it today: `portal` (26 routes), `website` (14), `report` (7),
`rental` (7), `ir` (4), `product` (3), `auth_signup` (3), `hr/HrKiosk` (2).

Each such lambda must:

1. capture `devMode` and `secureCookies` by value before the lambda;
2. gate every catch on `devMode` and `LOG_ERROR` the real message;
3. set `Secure` on any cookie when `secureCookies`;
4. apply the security headers itself.

Full detail: [../security/error-handling.md](../security/error-handling.md).

## SQL

### Qualify columns in a JOIN

```sql
-- BAD: ambiguous once res_partner is joined
WHERE company_id = $1
-- GOOD
WHERE rp.company_id = $1
```

This is not style. `RecordRuleSql` emits its filter as an id-membership
subquery precisely because a bare `company_id` in an outer query that joins
`res_partner` would be ambiguous and fail.

### `exec()`, not `exec_params()`

```cpp
txn.exec(query, pqxx::params{...});     // exec_params is deprecated
```

### Keep a `json` result alive

```cpp
// WRONG — the temporary is destroyed before the loop runs
for (auto& [k,v] : view->fields().items()) ...

// CORRECT
auto flds = view->fields();
for (auto& [k,v] : flds.items()) ...
```

### Seeds use `ON CONFLICT`, never a COUNT guard

```sql
INSERT INTO ir_ui_menu (id, name, ...) VALUES (...) ON CONFLICT (id) DO NOTHING;
```

A `IF (COUNT(*) > 0) RETURN` early-return blocks *new* seeds from being added
whenever an older partial seed exists.

## Money and quantities

Amounts and quantities are **int64 micro-units, scale 6** (`core/Money.hpp`).
Never `double`, never `NUMERIC` for an amount.

`double` above a `NUMERIC(16,2..4)` column was measurably wrong at the precision
that matters most: `0.00042` is not representable and stored as
`0.00042000000000000002`, and accumulating a price in a loop gave a different
answer from multiplying it.

**Display** precision is a separate concern (`core/DecimalPrecision.hpp`), is
user-configurable, and never changes what is stored.

## Sessions

`SessionManager::get()` uses a shared lock on the fast path and upgrades to
exclusive only when `accessedAt` needs refreshing (older than the 60 s touch
interval). `PortalSessionManager` follows the same pattern; any future session
store must too.

After `authenticate()`, the session is enriched with `isAdmin` and `groupIds`
from `res_groups_users_rel` and stored via `update()`. Later requests read them
back through `callerSession_()`.

## Performance

### Throttle rate-limiter pruning

```cpp
// WRONG — O(n) on every allow()
prune_(now);

// CORRECT — at most once per window
if ((now - lastPrune_) >= std::chrono::seconds(kWindowSeconds)) {
    prune_(now); lastPrune_ = now;
}
```

### Hex with a lookup table, not `ostringstream`

```cpp
static constexpr char kHex[] = "0123456789abcdef";
std::string id; id.reserve(32);
for (unsigned char b : buf) { id += kHex[b >> 4]; id += kHex[b & 0x0f]; }
```

### `ServiceFactory` is the config bus

Add new configuration there — never read an environment variable or a file
inside a module constructor. It currently carries `db()`, `devMode()`,
`secureCookies()` and `sessions()`.

## OWL 2 IIFE build

There is no bundler, which constrains what the frontend can do.

### Event delegation inside `t-foreach` — mandatory

A named method in `t-on-*` inside a `t-foreach` cannot resolve in the IIFE
build.

```xml
<!-- WRONG — method not found at runtime -->
<t t-foreach="lines" t-as="line" t-key="line.id">
    <input t-on-change="onLineChange"/>
</t>

<!-- CORRECT — delegate on the parent, dispatch on data-* -->
<div t-on-change="onTableChange">
    <t t-foreach="lines" t-as="line" t-key="line.id">
        <input data-line-field="qty" data-key="line.id"/>
    </t>
</div>
```

### No raw `&&` in a template — write `and`

Templates are parsed as XML, so a bare `&&` is a parse error and has to be
escaped `&amp;&amp;`, which nobody enjoys reading. OWL's expression compiler
replaces the word operators, so write those instead:

| Write | Compiles to |
|---|---|
| `and` `or` | `&&` `||` |
| `gt` `gte` `lt` `lte` | `>` `>=` `<` `<=` |

```xml
<button t-if="state.selectedId and !props.readonly"/>
```

`not` is **not** in that table — use `!`, which needs no escaping. Where the
condition is long enough to be worth naming, a getter is still better than
either.

### Wrap each component file in an IIFE

Script tags share global scope.

```js
const MyComponent = (() => {
    const { Component, useState, xml } = owl;
    class MyComponent extends Component { /* ... */ }
    return MyComponent;
})();
```

Never write `const { Component, ... } = owl` at the top level of a second script
file.

### Define children before parents, and declare every one you render

`static components = { Foo }` evaluates `Foo` at class-definition time. Child
components must appear **before** the parent that names them — which is why the
script order in `index.html` is load-bearing.

The matching trap is rendering a component you did not declare. OWL resolves it
at **first render**, so

```
Cannot find the definition of component "M2OSelect",
missing static components key in parent
```

fires only when a user opens that one form — never at load, never in an API
test, and the screen is blank rather than broken-looking.
`tests/functional/13-form-pickers` checks this statically across every class
*and* by opening each form in a browser.

### `complete_name` is not on `product.product`

It exists on `stock_location`, not on products. Load product options with
`['id', 'name']`.

## Naming

| Thing | Form | Example |
|---|---|---|
| C++ class | PascalCase | `StockPicking` |
| Model name | dotted | `stock.picking` |
| SQL table | underscored | `stock_picking` |
| View key | `model.name.list` / `.form` | `stock.picking.list` |
| ViewModel key | the model name | `stock.picking` |

## Adding a module — the checklist

1. `modules/mymod/MyModule.hpp` + `.cpp`, split as above.
2. Implement `IModule`: `registerModels`, `registerServices`, `registerViews`,
   `registerViewModels`, `registerRoutes`, `initialize`, plus `ensureSchema_()`
   and `seed*()`.
3. Schema: `CREATE TABLE IF NOT EXISTS` + `ALTER TABLE … ADD COLUMN IF NOT
   EXISTS` for anything added later.
4. Seeds: `ON CONFLICT (id) DO NOTHING`.
5. Sensitive model → add it to `checkModelAccess_()` in `JsonRpcDispatcher`.
6. ViewModel needing a session → capture `services_.sessions()`.
7. Register a list **and** a form view.
8. Seed `ir_act_window` + `ir_ui_menu` — take the ids from
   `bash tests/integration/core/menu-ids/test.sh`, never by reading a table.
   See [../reference/id-registry.md](../reference/id-registry.md).
9. Add the module to `main.cpp` in dependency order.
10. Portal-style route → capture `devMode_` and `secureCookies_` and apply them
    in every catch and every cookie.
11. `cmake -B ./build` once, so the glob picks the new files up.
12. Add a test folder under `tests/` with a `meta` — see
    [../operations/testing.md](../operations/testing.md).

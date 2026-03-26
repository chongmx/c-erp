# Security: Error Handling — ex.what() Disclosure Rule

**Rule ID:** SEC-28
**Severity:** HIGH
**Status:** Fixed in all known locations (report module fixed 2026-03-26)

---

## The Rule

**Never pass `ex.what()` into any HTTP response body without gating it behind `devMode`.**

```cpp
// ❌ WRONG — leaks SQL errors, table names, query text to attackers
cb(htmlError(500, std::string("Internal error: ") + ex.what()));
return errorResponse_(id, 500, "Server Error", e.what());

// ✅ CORRECT — safe in production, full detail in dev
LOG_ERROR << "[module/route] " << ex.what();            // always log server-side
cb(htmlError(500, devMode ? ex.what() : "An internal error occurred"));
```

---

## Why this matters

`ex.what()` from a pqxx exception contains the **full PostgreSQL error text**, which
includes:

- The complete failed SQL statement
- Table names, column names, constraint names
- Data values that caused the failure (e.g. duplicate key values)
- Internal schema details

An attacker who can trigger a server error (bad input, edge case) and read the response
body learns the schema for free. This dramatically lowers the effort required for SQL
injection, privilege escalation, or targeted attacks.

**Example of what leaks without this fix:**

```
ERROR: duplicate key value violates unique constraint "product_product_barcode_key"
DETAIL: Key (barcode)=(12345678) already exists.
```

This tells an attacker the table is `product_product`, the column is `barcode`, there is
a unique constraint on it, and the value they tried already exists.

---

## Where `devMode` comes from

| Context | How to get devMode |
|---|---|
| JSON-RPC dispatcher | `devMode_` member (set in constructor from `ServiceFactory`) |
| HTTP route lambdas | Capture `bool devMode = services_.devMode()` before the lambda, then close over it |
| HttpServer `addJsonPost` wrapper | `devMode` is already captured from `cfg_.devMode` |

**In a module's `registerRoutes()`:**

```cpp
void registerRoutes() override {
    auto db      = db_;
    auto sessions = services_.sessions();
    bool devMode  = services_.devMode();   // ← capture once, close over in each lambda

    drogon::app().registerHandler("/my/route/{1}",
        [db, sessions, devMode](const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                 const std::string& param)
        {
            try {
                // ... business logic ...
            } catch (const std::runtime_error& ex) {
                // runtime_error = application-level error (record not found, validation)
                // Still gate it — even benign messages create an oracle.
                cb(htmlError(404, devMode ? ex.what() : "Record not found"));
            } catch (const std::exception& ex) {
                // std::exception = could be pqxx SQL error, JSON parse error, anything
                LOG_ERROR << "[my/route] " << ex.what();   // log for debugging
                cb(htmlError(500, devMode ? ex.what() : "An internal error occurred"));
            }
        },
        {drogon::Get}
    );
}
```

---

## Special exception: `AccessDeniedError`

`AccessDeniedError` (from `core/infrastructure/Errors.hpp`) is **always** passed through
unconditionally — the user must know why their request was denied (HTTP 403). This is by
design and is not a disclosure risk.

```cpp
} catch (const AccessDeniedError& e) {
    return errorResponse_(id, 403, "Access Denied", e.what());  // always shown — OK
} catch (const std::exception& e) {
    LOG_ERROR << "[rpc] " << e.what();
    return errorResponse_(id, 200, "Odoo Server Error",
                          devMode_ ? e.what() : "An internal error occurred");  // gated
}
```

---

## Checklist for every new route or catch block

Before merging any new HTTP route:

- [ ] Every `catch (const std::exception& ex)` body that writes to the response uses
      `devMode ? ex.what() : "An internal error occurred"`
- [ ] Every `catch (const std::runtime_error& ex)` body that writes to the response uses
      `devMode ? ex.what() : "<safe user-facing message>"`
- [ ] Actual error detail is logged with `LOG_ERROR` regardless of devMode
- [ ] `devMode` is captured from `services_.devMode()` at the top of `registerRoutes()`,
      not inline-called inside the lambda (to avoid a dangling reference)

---

## S-30: Record-Level Authorization (known gap, documented)

**Status:** Documented gap — not yet implemented.

The current access control is model-level only: a user either can or cannot access
`sale.order` as a whole. There is no row-level filtering (Odoo's `ir.rule` equivalent).

**Impact:** In a single-company, small-team deployment this is acceptable. In a
multi-company or multi-team deployment, a user who can access `sale.order` can read
every sales order in the system.

**Future fix:** Implement an `ir_rule` table and inject domain filters into
`BaseModel::search/searchRead` based on `(user_id, company_id, group_ids)` from the
session. This is a medium architectural change tracked for a future security phase.

Until then, deployments with strict data isolation requirements should use separate
database instances per company.

---

## S-29: Shell Injection via wkhtmltopdf Arguments (fixed 2026-03-26)

**Location:** `modules/report/ReportModule.hpp` — PDF generation route
**Fix:** `pdfPaperFormat` is now validated against allowlist `{"A3","A4","A5","Letter","Legal"}`
before interpolation into the `wkhtmltopdf` shell command. Any value not in the list
silently falls back to `"A4"`.

**Principle:** Any DB-sourced value that reaches a shell command or external process
**must** be either:
1. Validated against a fixed allowlist (preferred), OR
2. Passed as a direct argument via `execve()`/`fork()+exec()` (bypasses shell entirely)

Never use string concatenation to build shell commands with untrusted input.

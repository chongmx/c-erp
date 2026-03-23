# Security & Performance Audit v3 — Fixes (2026-03-23)

All findings from the third security/performance pass have been assessed and fixed.

---

## Assessment Summary

| ID | Severity | Verdict | Action |
|----|----------|---------|--------|
| SEC-23 | HIGH | **Real** — res.users write/create/delete endpoints had no authorization | Fixed |
| SEC-04 | HIGH | **Real** — no model-level access check in JSON-RPC dispatch path | Fixed |
| SEC-24 | MEDIUM | **Real** — PortalSessionManager still used plain std::mutex | Fixed |
| SEC-11 | MEDIUM | **Real** — WebSocket endpoint accepted unauthenticated connections | Fixed |
| PERF-07 | MEDIUM | **Real** — rate limiter prune ran on every allow() call | Fixed |
| PERF-08 | LOW | **Real** — session ID generation used ostringstream | Fixed |
| PERF-09 | MEDIUM | Duplicate of SEC-24 (portal session mutex) | Fixed under SEC-24 |
| PERF-03 | HIGH | **Valid** — single TU compile time | Deferred (architectural) |
| PERF-04 | MEDIUM | **Valid** — no caching for fields_get/get_views | Deferred |
| PERF-05 | LOW | **Valid** — JSON deep-copy in handleCallKw_ | Deferred |
| PERF-06 | MEDIUM | **Valid** — portal sequential SQL | Deferred |

---

## SEC-23 — User Management Endpoints Had No Authorization

**Root cause:** `AuthViewModel::handleCreate()`, `handleWrite()`, and `handleUnlink()` executed
as any authenticated user, allowing privilege escalation.

**Fix (`modules/auth/AuthViewModel.hpp`):**

Added `callerSession_()` helper (resolves session from `call.kwargs["context"]["session_id"]`
via `sessions_`) and `requireAdmin_()` helper (checks `isAdmin || hasGroup(SETTINGS_CONFIGURATION)`).

Rules applied:
- `handleCreate`: requires `requireAdmin_()`
- `handleSearchRead`: requires `requireAdmin_()` (prevents data harvesting of all users)
- `handleWrite`:
  - Non-admin: can only write their own record (`ids == [session.uid]`)
  - Non-admin: cannot modify `groups_id` (privilege escalation)
  - Non-admin: cannot modify `password` via write (must use `change_password` which requires the old password)
- `handleUnlink`:
  - Requires admin/settings group
  - Prevents self-deletion

---

## SEC-04 — No Model-Level Authorization in JSON-RPC Dispatch

**Root cause:** `JsonRpcDispatcher::handleCallKw_()` checked `isAuthenticated()` but never
verified that the authenticated user had rights to access the requested model.

**Fix (`core/infrastructure/JsonRpcDispatcher.hpp`):**

Added `checkModelAccess_(model, session)` called immediately after the auth check.
Uses a static `unordered_map<string,int>` mapping model names to minimum required group IDs.
Admins bypass all checks. Throws `std::runtime_error` on denial.

Models protected:
```
account.move / account.move.line  → ACCOUNT_BILLING  (5)
hr.employee                       → HR_EMPLOYEE      (15)
stock.move / stock.picking / ...  → INVENTORY_USER   (11)
sale.order / sale.order.line      → SALES_USER       (7)
purchase.order / ...              → PURCHASE_USER    (9)
mrp.production                    → MRP_USER         (13)
```

---

## SEC-24 + PERF-09 — PortalSessionManager Plain std::mutex

**Root cause:** Core `SessionManager` was upgraded to `shared_mutex` with two-phase get()
in PERF-01, but `PortalSessionManager` in `PortalModule.hpp` still used plain `std::mutex`,
serializing all session lookups under portal traffic.

**Fix (`modules/portal/PortalModule.hpp`):**
- Added `#include <shared_mutex>`
- Changed `mutable std::mutex mutex_` → `mutable std::shared_mutex mutex_`
- `create()`, `remove()`: `std::unique_lock` (exclusive, write)
- `get()`: two-phase locking — shared lock fast path; exclusive lock only when
  `accessedAt` needs refreshing (at most once per `kTouchInterval_` = 60s)

Mirrors the `SessionManager` pattern exactly.

---

## SEC-11 — Unauthenticated WebSocket Endpoint

**Root cause:** `BusController::handleNewConnection()` called `service_->onConnect()` without
validating the session cookie. Any client could connect and subscribe to notification channels.

**Fix (`core/infrastructure/WebSocketServer.hpp`, `core/Container.hpp`):**
- Added `SessionManager* sessionManager_` static to `BusController` (same pattern as `service_`)
- `WebSocketServer` constructor now accepts `shared_ptr<SessionManager>` (optional, defaults to
  nullptr for backward compat)
- `registerRoutes()` calls `BusController::setSessionManager(sessions_.get())`
- `Container.hpp`: reordered construction so `sessions` is created before `ws`; passes
  `sessions` to `WebSocketServer` constructor
- `handleNewConnection`: reads `session_id` cookie, looks up session, rejects
  (sends error JSON, calls `conn->shutdown()`) if not authenticated

---

## PERF-07 — Rate Limiter Prune on Every Call

**Root cause:** `LoginRateLimiter::allow()` (and `PortalLoginRateLimiter::allow()`) called
`prune_()` on every invocation. Under normal load this iterates the full IP table on every
request, even when no entries are expired.

**Fix (`core/infrastructure/JsonRpcDispatcher.hpp`, `modules/portal/PortalModule.hpp`):**
Added `lastPrune_` (`Clock::time_point`) member to both rate limiters.
`prune_()` is now called only when `now - lastPrune_ >= kWindowSeconds`.

---

## PERF-08 — ostringstream for Hex Session ID Generation

**Root cause:** Both `SessionManager::generateId_()` and `PortalSessionManager::generateId_()`
used `std::ostringstream` with `std::hex` / `std::setfill` / `std::setw` — allocates and
streams one character at a time with locale overhead.

**Fix (`core/infrastructure/SessionManager.hpp`, `modules/portal/PortalModule.hpp`):**
Replaced with a direct character lookup using `static constexpr char kHex[] = "0123456789abcdef"`.
The output string is pre-reserved to 32 characters. Removed `<sstream>` and `<iomanip>`
includes from `SessionManager.hpp`.

---

## Deferred Findings (Valid, Out of Scope)

| ID | Reason deferred |
|----|-----------------|
| PERF-03 | Splitting the single-TU build requires extracting all headers into proper .hpp/.cpp pairs — multi-day refactor, no correctness impact |
| PERF-04 | fields_get/get_views caching requires an invalidation strategy; data is static at runtime but a cache layer is non-trivial |
| PERF-05 | JSON deep-copy in handleCallKw_ is minor relative to DB round-trip cost |
| PERF-06 | Portal sequential SQL requires restructuring portal handler queries — medium scope, no security impact |

---

---

## SEC-25 — handleCallKw_ Leaked e.what() Regardless of devMode

**Root cause:** The `catch` blocks inside `handleCallKw_()` passed `e.what()` directly to
`errorResponse_()`. The outer `HttpServer` devMode gating was already bypassed because the
exception was caught internally first, so SQL errors, table names, and internal messages
were always sent to clients.

**Fix (`core/infrastructure/JsonRpcDispatcher.hpp`, `core/infrastructure/Errors.hpp`,
`core/Container.hpp`):**
- New `AccessDeniedError` class in `core/infrastructure/Errors.hpp` — extends
  `std::runtime_error`; always shown to client (user must know why they're denied)
- Added `devMode_` member to `JsonRpcDispatcher`; passed from `Container` via
  `cfg.http.devMode` (new 5th constructor argument)
- Catch order in `handleCallKw_()`:
  1. `AccessDeniedError` → always shown, HTTP 403
  2. `std::out_of_range` → `devMode_ ? e.what() : "An internal error occurred"` + `LOG_ERROR`
  3. `std::exception` → same devMode gate + `LOG_ERROR`
- `checkModelAccess_()` and all authorization guards in `AuthViewModel` now throw
  `AccessDeniedError` instead of `std::runtime_error`

---

## SEC-26 — Model Access Map Had Coverage Gaps (Allow-by-Default)

**Root cause:** `checkModelAccess_()` had 13 models in `kRequired`. Any model not in the
map was allowed for any authenticated user. Notable gaps: `account.account`,
`account.journal`, `account.tax`, `account.payment`, `account.payment.term`,
`res.company`, `res.groups`, `ir.config.parameter`, `hr.department`, `hr.job`, `mrp.bom`.

**Fix (`core/infrastructure/JsonRpcDispatcher.hpp`):**
Three-tier deny-by-default policy:
1. **kAllowed** (any authenticated BASE_INTERNAL user): `ir.ui.menu`, `ir.actions.act_window`,
   `res.currency`, `res.partner`, `res.users`, `uom.uom`, `product.product`,
   `product.category`, `product.supplierinfo`, `mail.message`, `portal.partner`
2. **kRequired** (specific group — expanded from 13 to 23 entries):
   - All account.* models → ACCOUNT_BILLING (5)
   - hr.department, hr.job added → HR_EMPLOYEE (15)
   - `res.company` → SETTINGS_CONFIGURATION (4)
   - `res.groups` → SETTINGS_CONFIGURATION (4)
   - `ir.config.parameter` → BASE_ADMIN (3) — may contain SMTP credentials / API keys
   - `mrp.bom` added → MRP_USER (13)
3. **Default** (model not in either set): require BASE_INTERNAL (2) — any future ViewModel
   registered without an explicit entry is still protected from unauthenticated access

---

## Updated Rules

### ViewModel authorization — any sensitive model
Any ViewModel handling create/write/unlink must check the caller's session:
1. Resolve session via `sessions_->get(call.kwargs["context"]["session_id"])`
2. Check `session.isAdmin || session.hasGroup(Groups::X)` as appropriate
3. For write: check that non-admin is writing their own record only
4. For password changes: never accept `password` in a generic `write()`; require `change_password` (which validates the old password first)

### Model access map must be kept in sync
When adding a new model that contains sensitive data, add it to the `kRequired` map in
`JsonRpcDispatcher::checkModelAccess_()` with the appropriate minimum group constant.

### PortalSessionManager is now the same pattern as SessionManager
Both use `shared_mutex` + two-phase `get()`. Any future session store should follow this pattern.

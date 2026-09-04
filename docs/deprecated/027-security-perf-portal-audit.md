# Portal Security & Performance Audit — Fixes (2026-03-23)

All findings from the portal security audit and performance audit have been assessed and fixed.

---

## Assessment Summary

| ID | Severity | Verdict | Action |
|----|----------|---------|--------|
| SEC-16 | HIGH | **Real** — path traversal via attacker-controlled filename | Fixed |
| SEC-17 | HIGH | **Real** — `e.what()` leaked to portal clients regardless of devMode | Fixed |
| SEC-18 | MEDIUM | **Real** — portal cookie missing Secure flag | Fixed |
| SEC-19 | MEDIUM | **Real** — no file size or extension validation on upload | Fixed |
| PERF-01 | HIGH | **Real** — single std::mutex serializes all session lookups | Fixed |
| PERF-02 | HIGH | **Real** — FieldRegistry rebuilt on every model instantiation | Fixed |

---

## SEC-16 — Path Traversal in File Upload

**Root cause:** `file.getFileName()` was used directly in the save path. An attacker could upload
`../../web/static/shell.js` and overwrite application files.

**Fix (`modules/portal/PortalModule.hpp`):**
- Extract only the basename (strip all `'/'` and `'\\'` components)
- Validate extension against allowlist: `.pdf`, `.jpg`, `.jpeg`, `.png`
- Reject empty or invalid filenames before calling `saveAs()`
- Save path uses only the sanitized basename: `data/payment_proofs/{id}_{ts}_{baseName}`
- Store `baseName` (not raw filename) in the `payment_proof` table

---

## SEC-17 — Verbose Error Messages in Portal

**Root cause:** Every catch block in `registerRoutes()` returned `e.what()` directly to the client.
The core `HttpServer` had this fixed via `devMode`, but portal routes bypass `HttpServer` entirely
by registering directly with Drogon, so the fix was never applied.

**Fix (`modules/portal/PortalModule.hpp` + `core/factories/Factories.hpp` + `core/Container.hpp`):**
- Added `devMode_` and `secureCookies_` to `ServiceFactory` (propagated from `AppConfig`)
- `Container` passes `cfg.http.devMode` and `cfg.http.secureCookies` to `ServiceFactory`
- `PortalModule` reads them in its constructor; captures them in `registerRoutes()` lambdas
- All 14 catch blocks now return `"An internal error occurred"` in production and `e.what()` in dev
- Errors are logged server-side via `LOG_ERROR << "[portal] " << e.what()` in all cases

**Rule:** Any module that registers routes directly with Drogon (bypassing `HttpServer::addJsonPost`)
**must** capture `devMode` from `ServiceFactory` and apply the same conditional pattern.

---

## SEC-18 — Portal Session Cookie Missing Secure Flag

**Root cause:** The login route set `HttpOnly` and `SameSite=Lax` but never called `setSecure(true)`.
The core `JsonRpcDispatcher` had this fixed via its `secureCookies_` member, but portal did not.

**Fix (`modules/portal/PortalModule.hpp`):**
```cpp
if (secureCookies) cookie.setSecure(true);
```
Added immediately after `cookie.setMaxAge(8 * 3600)` in the login handler.

---

## SEC-19 — No File Size or Type Validation on Upload

**Root cause:** No size or extension check before `file.saveAs()`.

**Fix (`modules/portal/PortalModule.hpp`):**
- Size check: `file.fileLength() > 10 MB` → 400 Bad Request before any disk write
- Extension check: lowercase extension must be `.pdf`, `.jpg`, `.jpeg`, or `.png`
- Combined with SEC-16 basename sanitization (same code block)

---

## PERF-01 — Single-Mutex Session Store

**Root cause:** `SessionManager` used `std::mutex` for all operations including `get()`.
Under concurrent load, every HTTP request that checks its session serializes on this lock.

**Fix (`core/infrastructure/SessionManager.hpp`):**
- Replaced `std::mutex` with `std::shared_mutex`
- `create()`, `update()`, `destroy()`, `evictExpired()`: `std::unique_lock` (exclusive)
- `size()`, `healthInfo()`: `std::shared_lock` (concurrent reads)
- `get()` uses **two-phase locking**:
  1. Shared lock first — returns cached session if `accessedAt` was updated within 60s
  2. Exclusive lock only when `accessedAt` needs refreshing (at most once per minute per session)
- Result: under read-heavy workloads, concurrent `get()` calls run in parallel instead of serializing

```cpp
// kTouchInterval_ = 60 seconds — reduces exclusive-lock frequency dramatically
static constexpr Duration kTouchInterval_ = std::chrono::seconds{60};
```

---

## PERF-02 — Per-Request FieldRegistry Rebuilding

**Root cause:** `BaseModel<TDerived>` constructor called `registerFields()` on every instantiation,
rebuilding the `FieldRegistry` (~15+ map insertions) for every JSON-RPC request.

**Fix (`modules/base/BaseModel.hpp`):**
- Changed `FieldRegistry fieldRegistry_` from an instance member to `inline static FieldRegistry fieldRegistry_`
- Wrapped `registerFields()` call in `std::call_once` using a local static `std::once_flag`
- Since `BaseModel<ResPartner>` and `BaseModel<ResUsers>` are different template instantiations,
  each has its own `once` flag and `fieldRegistry_` static — correct per-model-type isolation
- Subsequent instantiations of the same model type reuse the already-populated static registry

```cpp
static std::once_flag once;
std::call_once(once, [this]() {
    static_cast<TDerived*>(this)->TDerived::registerFields();
});
```

---

## Lessons Learned — Patterns to Follow

### Portal routes bypass core HTTP helpers
**Problem:** `PortalModule` registers routes directly with `drogon::app().registerHandler()`.
This bypasses `HttpServer::addJsonPost()` which already implements `devMode` error handling and
the `secureCookies` pattern. Any future portal route added this way inherits none of those protections.

**Rule:** When adding a new portal route:
1. Capture `devMode` and `secureCookies` in the lambda capture list
2. Wrap all catch blocks with `devMode ? e.what() : "An internal error occurred"`
3. Apply `if (secureCookies) cookie.setSecure(true)` to any cookie you set
4. Call `LOG_ERROR << "[portal] " << e.what()` in every catch block

### File upload checklist
Before accepting any file upload:
1. Check `file.fileLength() <= kMaxBytes` (10 MB)
2. Extract basename: strip all `/` and `\` path components
3. Validate extension against an allowlist (never content-type alone — it's attacker-controlled)
4. Use server-generated filename for the save path; store original name only in DB if needed

### ServiceFactory is the config bus for modules
`ServiceFactory` now exposes `devMode()` and `secureCookies()`. If future modules need other
config values (e.g., `smtpHost`, `uploadDir`), add them to `ServiceFactory` and pass them
from `Container` — do not read env vars or files directly inside module constructors.

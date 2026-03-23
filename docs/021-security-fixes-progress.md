# Security Fixes — Progress Log (2026-03-23)

All fixes implemented and build verified clean.

---

## Assessment Summary

| ID | Verdict | Action |
|----|---------|--------|
| SEC-02 (revised) | **Defense-in-depth gap** — not a live user-injection but `FieldRegistry` never validated names | Fixed in `FieldRegistry::add()` |
| SEC-03 | **Real** — non-CSPRNG session IDs | Fixed |
| SEC-04 | **Real architectural gap** — no RBAC | Documented, deferred to future phase |
| SEC-05 | **Largely mitigated** — SameSite=Lax | Documented; SEC-07 fix closes remainder |
| SEC-06 | **Real** — no login rate limiting | Fixed |
| SEC-07 | **Real** — wildcard CORS default | Fixed |
| SEC-08 | **Real** — `e.what()` returned to client | Fixed |
| SEC-09 | **Real** — no security headers | Fixed |
| SEC-10 | **Real** — missing Secure cookie flag | Fixed |

---

## SEC-02 — Field Names in write() / buildSelectCols_() (FALSE POSITIVE)

**Finding:** write() and buildSelectCols_() concatenate field names into SQL.

**Assessment:** Already safe. Both paths check `fieldRegistry_.has(key)` before touching the name:

```cpp
// write() — BaseModel.hpp:141
if (!fieldRegistry_.has(it.key())) continue;
setClause += it.key() + "=$" + std::to_string(idx++);  // only reached if whitelisted

// buildSelectCols_() — BaseModel.hpp:269
if (f != "id" && fieldRegistry_.has(f)) s += "," + f;  // same guard
```

The `FieldRegistry` is populated exclusively from C++ `registerFields()` code — never from user input. Any key not in the registry is silently skipped before it reaches SQL. **No change needed.**

---

## SEC-03 — mt19937_64 Session IDs → RAND_bytes() ✅ Fixed

**File:** `core/infrastructure/SessionManager.hpp`

**Problem:** `mt19937_64` seeded from `std::random_device` is a PRNG, not a CSPRNG. An attacker who observes enough session IDs can reconstruct the internal state and predict future tokens.

**Fix:** Replaced with `RAND_bytes()` from OpenSSL (already a dependency for PBKDF2).

```cpp
// Before
static thread_local std::mt19937_64 rng{std::random_device{}()};
std::uniform_int_distribution<uint64_t> dist;
const uint64_t hi = dist(rng);
const uint64_t lo = dist(rng);
// ... hex encode hi+lo

// After
unsigned char buf[16];
if (RAND_bytes(buf, sizeof(buf)) != 1)
    throw std::runtime_error("SessionManager: RAND_bytes failed");
// ... hex encode buf byte by byte
```

Removed `#include <random>`, added `#include <openssl/rand.h>`. Output is still a 32-character hex string (128 bits), same wire format, no other changes needed.

---

## SEC-04 — No Authorization Layer (DOCUMENTED — FUTURE PHASE)

**Finding:** Any authenticated user can call any ViewModel method on any record.

**Assessment:** Confirmed. The dispatcher checks only `session.isAuthenticated()` — there is no model-level access control (no equivalent of Odoo's `ir.model.access` or `ir.rule`).

**Decision:** This requires a dedicated implementation phase (role-based access control):
- `ir_model_access` table: per-model read/write/create/unlink flags per group
- `ir_rule`: per-record domain filters
- Session group membership resolution

This is deferred as a separate phase. See `docs/plan.md` for future prioritisation. In the interim, the system is appropriate for single-user or trusted-network deployments only.

---

## SEC-05 — CSRF (MITIGATED BY EXISTING CONTROLS)

**Finding:** No explicit CSRF token mechanism.

**Assessment:** Meaningfully mitigated by two existing controls:

1. **`SameSite=Lax` on session cookie** (`JsonRpcDispatcher.hpp:192`): The browser will not include this cookie on cross-site POST requests, which is the primary CSRF attack vector for JSON APIs.

2. **JSON content-type requirement**: All API endpoints parse `nlohmann::json::parse(req->body())`. A cross-site HTML form or `<img>` tag cannot send `Content-Type: application/json` — that requires JavaScript and triggers a CORS preflight.

Fixing SEC-07 (CORS wildcard → restricted origin) completes the CSRF mitigation: even if a malicious script sends a preflight, it will not receive `Access-Control-Allow-Origin` confirmation, so the browser blocks the actual request.

**No additional change beyond SEC-07 fix.** Traditional CSRF tokens (double-submit cookie) are not needed given these controls.

---

## SEC-06 — No Login Rate Limiting ✅ Fixed

**File:** `core/infrastructure/JsonRpcDispatcher.hpp`

**Problem:** The `authenticate` endpoint accepted unlimited requests from any IP, enabling brute-force password attacks.

**Fix:** Added `LoginRateLimiter` class (in `JsonRpcDispatcher.hpp`) and wired it around the `authenticate` method dispatch.

**Parameters:** 10 failed attempts per 5-minute sliding window per IP. Counter resets on successful login.

```cpp
// Before authenticate call:
if (call.method == "authenticate") {
    const std::string ip = req->getPeerAddr().toIp();
    if (!rateLimiter_.allow(ip))
        return errorResponse_(id, 429, "Too many requests", "...");
}

// After authenticate call:
if (call.method == "authenticate") {
    const std::string ip = req->getPeerAddr().toIp();
    if (ok) rateLimiter_.recordSuccess(ip);
    else    rateLimiter_.recordFailure(ip);
}
```

`LoginRateLimiter` is an in-memory store with a `std::mutex`. Expired entries are pruned on each access to prevent unbounded growth. Member `rateLimiter_` lives on the `JsonRpcDispatcher` instance (one per server lifetime).

**Limitation:** In-memory only — restarts or multi-process deployments reset counts. For production with multiple instances, move state to Redis or PostgreSQL.

---

## SEC-07 — Wildcard CORS Default ✅ Fixed

**File:** `core/infrastructure/HttpServer.hpp`

**Problem:** `HttpConfig::corsOrigin` defaulted to `"*"`, causing every API response to carry `Access-Control-Allow-Origin: *`. This allows any website to read API responses via cross-origin JavaScript.

**Fix:** Changed default to `""` (empty). When empty, **no CORS headers are emitted** — correct for same-origin deployments where the frontend is served by the same server. When set to a specific origin, only that origin is reflected.

```cpp
// Before
std::string corsOrigin = "*";

// After
std::string corsOrigin = "";   // empty = same-origin only (no CORS headers)
```

All four handler helpers (`addJsonPost`, `addJsonPostWithResponse`, `addJsonGet`, `addCorsOptions`) now guard the header addition:

```cpp
if (!origin.empty())
    res->addHeader("Access-Control-Allow-Origin", origin);
```

**For development** with a separate frontend (e.g. a hot-reload dev server on port 3000), set in `config/system.cfg`:
```ini
cors_origin = http://localhost:3000
```

**Never set to `*` in production.**

---

## SEC-02 (Revised) — FieldRegistry Identifier Validation ✅ Fixed

**File:** `modules/base/FieldRegistry.hpp`

**Finding (revised):** `write()` and `buildSelectCols_()` guard field name concatenation with `fieldRegistry_.has()`, which performs an exact hash-map key lookup. A user-supplied string can only pass this check if it is byte-for-byte identical to a registered name — so a live HTTP attacker cannot inject via field names. However, `FieldRegistry::add()` never validated that registered names are safe SQL identifiers. A developer mistake (e.g. registering `"write date"`) or supply chain compromise could introduce a dangerous name that every downstream caller would trust.

**Fix:** Added `isValidIdentifier()` inline function (checks `[a-zA-Z_][a-zA-Z0-9_]*`) and call it at the top of `FieldRegistry::add()`:

```cpp
inline bool isValidIdentifier(const std::string& name) {
    if (name.empty()) return false;
    if (!std::isalpha(name[0]) && name[0] != '_') return false;
    for (auto c : name.substr(1))
        if (!std::isalnum(c) && c != '_') return false;
    return true;
}

void add(FieldDef def) {
    if (!isValidIdentifier(def.name))
        throw std::invalid_argument("FieldRegistry: field name '" + def.name +
                                    "' is not a valid SQL identifier");
    ...
}
```

Bad names now crash at boot (during `registerFields()`), not silently at query time. No per-use re-check is needed downstream.

---

## SEC-08 — Verbose Error Messages ✅ Fixed

**Files:** `core/infrastructure/HttpServer.hpp`, `core/Container.hpp`

**Problem:** All three handler helpers (`addJsonPost`, `addJsonPostWithResponse`, `addJsonGet`) returned `e.what()` verbatim in `{"detail": ...}`, potentially exposing SQL error text, table names, column names, and library internals.

**Fix:** Added `bool devMode` to `HttpConfig` (default `false`). All error responses now return a generic message in production:

```cpp
// In production (devMode=false):
{"error": "Internal server error", "detail": "An internal error occurred"}

// In development (devMode=true):
{"error": "Internal server error", "detail": "<full e.what() text>"}
```

Full exception text is always logged server-side via `LOG_ERROR` regardless of mode.

**Config (`config/system.cfg`):**
```ini
dev_mode = true   ; local development only — never in production
```

---

## SEC-09 — Missing Security Headers ✅ Fixed

**File:** `core/infrastructure/HttpServer.hpp`

**Problem:** No security headers on any response.

**Fix:** Added `applySecurityHeaders_()` private helper called by all four handler types:

```cpp
void applySecurityHeaders_(const HttpResponsePtr& res, const std::string& origin) const {
    res->addHeader("X-Content-Type-Options", "nosniff");
    res->addHeader("X-Frame-Options",        "DENY");
    res->addHeader("Referrer-Policy",        "strict-origin-when-cross-origin");
    res->addHeader("Content-Security-Policy", "default-src 'none'");
    if (!origin.empty())
        res->addHeader("Access-Control-Allow-Origin", origin);
}
```

**Note on CSP:** `default-src 'none'` is correct for pure JSON API responses (no resources should execute). Static HTML/JS files served via Drogon's `setDocumentRoot()` bypass this — those need a separate response hook with a policy permissive enough for the OWL frontend (`script-src 'self' 'unsafe-eval' 'unsafe-inline'`). HSTS is intentionally omitted here: it should be set at the reverse proxy (nginx/caddy) level, not the application, since the app cannot know if it is behind TLS termination.

---

## SEC-10 — Missing Secure Cookie Flag ✅ Fixed

**Files:** `core/infrastructure/JsonRpcDispatcher.hpp`, `core/Container.hpp`

**Problem:** Session cookie had `HttpOnly=true, SameSite=Lax` but no `Secure` flag, meaning it was transmitted over plain HTTP, exposing it to network interception.

**Fix:** Added `bool secureCookies` to `HttpConfig` (default `false` for local HTTP dev). `JsonRpcDispatcher` receives it as a constructor parameter and applies it when setting the cookie:

```cpp
if (secureCookies_) c.setSecure(true);
```

**Config (`config/system.cfg`):**
```ini
secure_cookies = true   ; required whenever behind HTTPS (all production deployments)
```

**Important:** The `Secure` flag requires the server to be behind TLS. Set `secure_cookies = true` in any deployment using HTTPS (directly or via reverse proxy). Leaving it `false` on localhost is safe because localhost is loop-back only.

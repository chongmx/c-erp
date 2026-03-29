# Security & Performance Fixes — 2026-03-29

All four issues from this session have been addressed.

---

## Summary

| ID | Severity | Status | Description |
|----|----------|--------|-------------|
| S-32 | MEDIUM | **Fixed** | `handleActionLoad_` ungated `e.what()` |
| S-34 | MEDIUM | **Fixed** | Stored XSS in portal template renderer |
| PERF-A | MEDIUM | **Fixed** | `rowsToJson_` exception-cascade type inference |
| PERF-C | HIGH | **Fixed (prior session)** | Connection pool blocking under contention |

---

## S-32 — handleActionLoad_ Ungated e.what() (MEDIUM)

**File:** `core/infrastructure/JsonRpcDispatcher.hpp` line ~743

**Root cause:** The `catch (const std::exception& e)` block in `handleActionLoad_` returned
`e.what()` directly to the client, which may include action IDs, model names, and SQL fragments.

**Fix:**
```cpp
} catch (const std::exception& e) {
    LOG_ERROR << "[rpc/action_load] " << e.what();
    return errorResponse_(id, 200, "Odoo Server Error",
                          devMode_ ? e.what() : "An internal error occurred");
}
```

Follows the SEC-28 pattern: always log, gate detail behind `devMode_`.

---

## S-34 — Stored XSS in Portal Template Renderer (MEDIUM)

**File:** `modules/portal/PortalModule.cpp`

**Root cause:** `portalRenderTemplate()` substituted DB-sourced values (partner name, company name,
product names, addresses, etc.) directly into HTML without encoding. A partner or product name
containing `<script>` would execute in the customer's browser.

**Fix:** Added `htmlEscape()` static helper:
```cpp
static std::string htmlEscape(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;
        }
    }
    return out;
}
```

Applied at substitution time in `portalRenderTemplate` for both `vars` and `lines`:
```cpp
row = portalReplaceAll(row, "{{" + k + "}}", htmlEscape(v));   // lines
tmpl = portalReplaceAll(tmpl, "{{" + k + "}}", htmlEscape(v)); // vars
```

Escaping at the substitution point is the most comprehensive fix — no per-assignment changes
needed; any future fields added to `vars`/`lines` are automatically covered.

---

## PERF-A — rowsToJson_ Exception-Cascade Type Inference (MEDIUM)

**File:** `modules/base/BaseModel.hpp` — `rowsToJson_()`

**Root cause:** Used exception cascade to guess column types:
```cpp
try { obj[col] = field.as<int>();    continue; } catch (...) {}
try { obj[col] = field.as<double>(); continue; } catch (...) {}
obj[col] = s;
```
pqxx `as<T>()` throws `pqxx::conversion_error` on type mismatch. For every string column in
every row, two exceptions were thrown and caught, each involving stack unwinding. On queries
returning 50+ rows with many string columns this was measurable overhead.

**Fix:** Use `field.type()` (returns PostgreSQL OID) to dispatch without exceptions:
```cpp
static constexpr pqxx::oid OID_BOOL    = 16;
static constexpr pqxx::oid OID_INT2    = 21;
static constexpr pqxx::oid OID_INT4    = 23;
static constexpr pqxx::oid OID_INT8    = 20;
static constexpr pqxx::oid OID_OID     = 26;
static constexpr pqxx::oid OID_FLOAT4  = 700;
static constexpr pqxx::oid OID_FLOAT8  = 701;
static constexpr pqxx::oid OID_NUMERIC = 1700;

if (oid == OID_BOOL)   { obj[col] = (s[0] == 't' || s[0] == '1'); continue; }
if (oid == OID_INT2 || oid == OID_INT4 || oid == OID_INT8 || oid == OID_OID)
    { obj[col] = field.as<long long>(); continue; }
if (oid == OID_FLOAT4 || oid == OID_FLOAT8 || oid == OID_NUMERIC)
    { obj[col] = field.as<double>(); continue; }
obj[col] = field.c_str();
```

`field.as<T>()` now only called when the type is confirmed correct — zero exceptions on the
happy path. Also fixes the pre-existing issue where a `BIGINT` column was being narrowed to
`int` (now uses `long long`).

---

## PERF-C — Connection Pool Blocking (HIGH) [Fixed in prior session]

`DbConnection::acquire()` default timeout changed from 0 (infinite block) to 5000 ms.
Throws `PoolExhaustedException` on exhaustion. `JsonRpcDispatcher` catches it and returns
HTTP 503 before this session began.

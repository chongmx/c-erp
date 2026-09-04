# 042 — Runtime Verification Results

**Date:** 2026-08-02
**Verifies:** `041` (S-39, S-40, S-44, S-47, C-3, C-4)
**Environment:** WSL Ubuntu **noble** (24.04) — same release as the GCP target
**Scripts:** `scripts/verify_security_fixes.sh`, `scripts/verify_s40_buckets.sh`, `scripts/negctl_s39.cpp`
**Outcome:** All fixes verified working. **Two new pre-existing bugs found (S-48, S-49); S-49 fixed.**

---

## 1. Results

| Check | Result |
|---|---|
| S-39 baseline — PDF still renders | ✅ 200, 18 615 bytes, valid `%PDF` |
| S-39 — `$(...)`, backtick, `;` payloads | ✅ Reach the handler (**200**), execute nothing |
| S-39 — negative control on old code | ✅ **Creates the marker file** — payload is live |
| S-44 — `--enable-local-file-access` gone | ✅ Source-clean; PDFs unaffected |
| C-3 — parent cycle rejected | ✅ Rejected, DB unchanged |
| C-4 — delete with children refused | ✅ Refused with counts, category intact |
| S-47 — identity audit rows | ✅ `create`/`write`/`unlink` on `res.users` logged with `uid=1` |
| S-40 — per-client buckets | ✅ A limited at attempt 11; B unaffected |
| S-40 — XFF last-element | ✅ Forged prefix cannot borrow or poison another bucket |
| Loopback binding | ✅ `ss` shows `127.0.0.1:8069` only |
| `secure_cookies` | ✅ Cookie carries `Secure` |
| wkhtmltopdf on noble | ✅ `0.12.6.1 (with patched qt)` present and working |

---

## 2. Why the first two verification attempts were wrong

Worth recording, because both failure modes produce **false passes**.

**Attempt 1 — the payload never left my shell.** Nested PowerShell → `wsl.exe` → `bash -lc`
quoting silently ate `$(...)`. The negative-control C++ file came out with `idStr="2"` — no
payload at all — and dutifully reported "inert". Fix: write test files with the editor, never
through nested shell heredocs.

**Attempt 2 — the payload never reached the handler.** Probes returned **404**, which looks
like a pass but means routing rejected the request before any code ran. Cause: the marker path
`/tmp/pwned` contains slashes, and a `/` in a URL segment adds a path component, so
`/report/pdf/{1}/{2}` no longer matched.

The corrected probes use slash-free markers in the server's CWD and return **200** — the
handler ran, the exec path executed, and nothing was created. A router-reachability sweep is
included in the script for exactly this reason:

```
id='2$x' -> 200   id='2(x)' -> 200   id='2`x`' -> 200   id='2;x' -> 200   id='2%20x' -> 200
```

**Drogon does not filter these characters.** `038` left exploitability open pending this test;
the answer is that S-39 **was genuinely exploitable**, confirmed by the negative control
(`scripts/negctl_s39.cpp`) which reproduces the pre-fix construction and creates the marker.

---

## 3. New finding — S-49: the login rate limiter never counted a failure (HIGH)

**Found by running it.** Thirteen consecutive bad logins from one client were all served
normally — the limiter never engaged.

`recordFailure()` sat *after* the dispatch:

```cpp
auto result = vm->callKw(call);        // AuthViewModel THROWS on bad credentials
...
if (call.method == "authenticate") {
    if (ok) rateLimiter_.recordSuccess(ip);
    else    rateLimiter_.recordFailure(ip);   // ← unreachable on the throw path
}
```

`AuthViewModel` throws `std::runtime_error("Invalid credentials")` for a wrong password, so
control jumped to the catch block and the counter was never incremented. `recordFailure` only
ran if authenticate *returned* with `uid <= 0`, which does not happen.

**Impact:** brute-force protection was entirely non-functional — unlimited password guessing
at full speed. This is independent of S-40: S-40 was about *which IP* the limiter keyed on;
S-49 is that it counted nothing at all. Both had to be fixed for the limiter to work, and
neither was visible from reading the code, which is why ten review cycles passed it — the
calls look correct in sequence.

**Fixed** (`JsonRpcDispatcher.hpp`): the authenticate dispatch is wrapped so the failure is
recorded on both the exception and the return path.

**Verified after fix:**

```
A attempt 10 -> "An internal error occurred"          (allowed)
A attempt 11 -> "Too many failed login attempts."     (limited)
B attempt  1 -> "An internal error occurred"          (unaffected)
```

---

## 4. New finding — S-48: `call_kw` ignores the session cookie (MEDIUM)

The RPC endpoint resolves a session **only** from `kwargs.context.session_id`, never from the
`Cookie` header. Measured directly:

| Request | Result |
|---|---|
| `Cookie: session_id=<valid>` (capital C) | ❌ "Session expired" |
| `cookie: session_id=<valid>` (lowercase) | ❌ "Session expired" |
| body `context.session_id` | ✅ works |
| `GET /report/pdf/...` with the same cookie | ✅ 200 |

The GET route uses `req->getCookie()`; `resolveSessionId_` uses
`extractFromCookie(req->getHeader("Cookie"))`, which returns empty in this Drogon version —
cookies are parsed into a separate map and are not served from the generic header map.

**Impact:**

1. Step 1 of `resolveSessionId_` is dead code. The OWL frontend works only because it sends
   `context.session_id` in the body.
2. **It amplifies S-43.** Every RPC request that omits the body context falls through to
   `sessions_->create()`, allocating a stored `Session` — and `evictExpired()` is never
   called. Confirmed in the log: each cookie-only request produced a *new* sid
   (`sid=84bd2571…`, `sid=daa125b8…`, `sid=a2ec7729…`). So the memory-exhaustion path is
   reachable by any authenticated client using cookies, not just by unauthenticated spray.
3. It is the same body-carried session id that makes **S-42** (fixation) reachable.

**Not fixed in this pass** — it should be fixed together with S-42 and S-43, since all three
concern session resolution and a partial fix risks breaking the working frontend path. Use
`req->getCookie(SessionManager::cookieName())` in `resolveSessionId_`, matching what the
working GET routes already do.

---

## 5. Change made during verification: `ValidationError`

C-3 and C-4 fired correctly but the user saw **"An internal error occurred"** — SEC-28 gates
`std::runtime_error` behind `devMode`, which is right for SQL faults and wrong for a business
rule the user must act on.

Added `ValidationError` (`core/infrastructure/Errors.hpp`) — always passed through, like
`AccessDeniedError` — plus a dispatcher catch returning **400** with
`name = "cerp.exceptions.ValidationError"`. C-3/C-4 now throw it, so the user sees
*"Cannot delete this category: it has 6 subcategories and 0 products. Reassign or delete them
first."*

This is a reusable primitive; the rental module (`040` §3) will need it throughout.

---

## 6. Minor observations

- **Invalid credentials returns "An internal error occurred."** Correct-by-accident from a
  security standpoint (no user enumeration) but poor UX and it hides real faults. Consider a
  deliberate `AccessDeniedError("Invalid credentials")`.
- **wkhtmltopdf works on noble** — `0.12.6.1 (with patched qt)`, installed at
  `/usr/local/bin`. This weakens but does not close **S-46**: this WSL instance was installed
  earlier, and `install_wkhtml.sh` still has no `noble` branch and silently falls back to the
  jammy package. Verify on the actual GCP VM before relying on it.
- **No local-file images in the current templates** — dropping
  `--enable-local-file-access` (S-44) caused no visible regression. Re-check if a logo is
  added later.

---

## 6a. nginx validated against the real binary

Run unprivileged from a temp prefix (`/etc/nginx` untouched, high ports 8080/8443) against
**nginx 1.24.0**, proxying to the live app. `scripts/test_nginx_proxy.sh`,
`scripts/test_nginx_s40_forge.sh`.

| Check | Result |
|---|---|
| `deploy/nginx/c-erp.conf` parses on 1.24 | ✅ |
| HTTPS → app, HTTP → app | ✅ 200 both |
| `$proxy_add_x_forwarded_for` appends the real peer **last** | ✅ |
| Forged headers cannot buy a fresh rate-limit bucket | ✅ |
| `/websocket` retains all five forwarding headers | ✅ |

### Three config bugs found by review before this ran

All would have shipped:

1. **`http2 on;` would have prevented nginx starting.** Ubuntu 24.04 ships nginx **1.24**,
   where HTTP/2 is a `listen` parameter; the standalone directive is 1.25.1+. Confirmed:
   `nginx version: nginx/1.24.0 (Ubuntu)`.
2. **The `/websocket` block silently dropped every forwarding header.** nginx inherits
   `proxy_set_header` from the enclosing block *only* when the current block declares none.
   That location sets `Upgrade`/`Connection`, so `X-Real-IP` and `X-Forwarded-For` were lost —
   an S-40 bypass on that path alone, invisible because WebSocket would still work.
3. **Upstream `keepalive 32` was inert** — nginx sends `Connection: close` upstream unless
   cleared.

### The X-Forwarded-For premise, measured

```json
{"real_ip":"127.0.0.1","xff":"9.9.9.9, 127.0.0.1","host":"127.0.0.1"}
```

Client-forged `9.9.9.9` lands **first**; the true peer nginx observed lands **last**. Reading
the last element is therefore unforgeable — and the common "read the first element"
implementation would have handed an attacker their chosen bucket.

### Forged-IP rotation, through nginx

14 login attempts, each with a different forged `X-Real-IP` **and** `X-Forwarded-For`:

```
allowed=2  throttled_by_nginx=8  throttled_by_app=4
```

Rotating forged IPs bought the attacker nothing. Both layers engaged — nginx's `limit_req`
fired first (defence in depth working as designed), the app's limiter caught the rest.

### The control that matters for deployment

The identical rotation **bypassing nginx**, straight to the app:

```
throttled: 0/14
```

Every forged IP got its own bucket. This is correct behaviour — the peer was loopback, which
`trusted_proxies` trusts, so the forwarded header is honoured. But it means
**`http_interface = 127.0.0.1` is load-bearing for S-40, not tidiness**: if the app is ever
reachable directly, rate limiting is trivially bypassable by header forgery. The binding check
in `deploy/README.md` §2 is the control that enforces it.

---

## 7. Verification artefacts

```
scripts/verify_security_fixes.sh   S-39 / C-3 / C-4 / S-47 + router reachability sweep
scripts/verify_s40_buckets.sh      S-40 bucket separation + XFF last-element
scripts/negctl_s39.cpp             negative control: pre-fix code, proves payload is live
scripts/diag_session.sh            session-resolution matrix (found S-48)
```

All are idempotent and safe to re-run. They need a running server, a reachable DB, and
`admin`/`admin`; override with `BASE`, `DBN`, `LOGIN`, `PASSWD`.

**These are throwaway harnesses, not the test suite.** They should be folded into the Google
Test / `ERP_TEST` harness in Stage 3 (`040`) so S-39, S-40, S-47, S-48 and S-49 each gain a
permanent regression test. S-49 in particular would never have been caught by a test that only
asserted the code compiles.

---

## 8. Status after this pass

| Finding | Status |
|---|---|
| S-39 command injection → RCE | ✅ Fixed and verified (exploitability confirmed pre-fix) |
| S-40 rate limiter keyed on proxy IP | ✅ Fixed and verified |
| S-49 rate limiter counted no failures | ✅ Found and fixed during verification |
| S-44 local file access | ✅ Fixed, no regression |
| S-47 identity/privilege audit | ✅ Fixed and verified |
| C-3 category cycles | ✅ Fixed and verified |
| C-4 category delete | ✅ Fixed and verified |
| S-48 call_kw ignores cookie | 🆕 **Open** — fix with S-42/S-43 |
| S-42 session fixation | Open |
| S-43 unbounded sessions | Open — **worse than estimated**, see §4 |
| S-41 domain field allowlist | Open |
| C-1 recursive product_count | Open |
| S-45 ORDER BY rebuild | Open |
| S-46 wkhtmltopdf on noble | Open — verify on the VM |
| Config untracked / password rotated | Open |

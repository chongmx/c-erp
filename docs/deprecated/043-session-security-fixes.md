# 043 — Session Security Fixes (S-42, S-43, S-48)

**Date:** 2026-08-02
**Implements:** S-42 (fixation), S-43 (unbounded sessions), S-48 (cookie ignored)
**Build:** clean · **Runtime-verified:** yes · **Unit-tested:** yes
**Scripts:** `scripts/verify_session_fixes.sh`, `scripts/test_sessionmanager.cpp`

---

## 1. Why these three together

All three are session *resolution* bugs and they interlock. S-48 (the cookie being ignored)
is what made S-43 reachable from ordinary authenticated traffic, and it is the same
body-carried session id that made S-42 exploitable. Fixing any one alone risked breaking the
working frontend path, since the frontend depends on the very behaviour S-48 describes.

| Finding | Severity | Status |
|---|---|---|
| S-42 session id not rotated on login | MEDIUM | ✅ Fixed, verified |
| S-43 unbounded session growth | MEDIUM | ✅ Fixed, verified |
| S-48 `call_kw` ignores the session cookie | MEDIUM | ✅ Fixed, verified |

---

## 2. S-48 — `call_kw` ignored the session cookie

**Cause.** `resolveSessionId_` used
`SessionManager::extractFromCookie(req->getHeader("Cookie"))`. Drogon parses cookies into a
separate map and does not serve them from the generic header map, so this returned empty
**always**. The GET routes were unaffected because they use `req->getCookie()`.

**Fix.** One helper, `cookieSid_()`, wrapping `req->getCookie(SessionManager::cookieName())` —
matching what the working GET routes already did — used by all three resolution paths.

Both paths now work, verified:

| Request | Before | After |
|---|---|---|
| `Cookie: session_id=<valid>` | ❌ "Session expired" | ✅ |
| body `context.session_id` | ✅ | ✅ (frontend unbroken) |

---

## 3. S-43 — unbounded session growth

Three separate defects, all required for the DoS:

**(a) `evictExpired()` was never called.** Expired sessions stayed in the map forever; `get()`
returned `nullopt` but left the entry. Now scheduled on the Drogon event loop every 60 s from
`Container::startSessionEviction_()`.

**(b) Every unresolved request minted a stored session.** `resolveSessionId_` ended with
`sessions_->create()` — before the auth check — so any client allocated a permanent `Session`
per HTTP request. With S-48 that included every cookie-bearing request.

It now returns `""`. This is behaviourally identical for callers: an unresolved id previously
produced a fresh *anonymous* session, and the caller falls back to a default `Session{}` which
is anonymous too. Only the logged sid differs. Real sessions are created at authentication.

**(c) The store had no ceiling.** Added `maxSessions_` (default 50 000). When full, `create()`
drops expired entries first, then the least-recently-used **anonymous** session — authenticated
users are never evicted to make room for an anonymous flood, so under attack the attacker's own
sessions are what get reclaimed.

Session TTL is now configurable via `session_ttl_minutes` (default 60) — an operational knob
worth having regardless, and it made (a) directly testable.

---

## 4. S-42 — session fixation

**Cause.** On successful login the pre-auth session was promoted in place
(`sessions_->update(cookieSid, …)`), so an id observed before authentication remained valid
after it. `get_session_info` hands out a valid anonymous id to anyone, and `resolveSessionId_`
also accepts an id from the request body — so an attacker could fixate a known id, wait for the
victim to log in, and hold an authenticated session. Account takeover with no credential theft.

**Fix.** `SessionManager::rotate(oldId)` copies the session to a freshly generated id, resets
its timestamps, and erases the old entry. The dispatcher calls it on every successful
`authenticate`.

One subtlety worth recording: the rotated id **must** be written back into the response
(`result["session_id"] = cookieSid`). The OWL frontend replays that value via
`context.session_id`, so returning the old id would have logged every user straight back out —
a fixation fix that bricks login is not a fix.

---

## 5. Verification

### Runtime (`scripts/verify_session_fixes.sh`)

```
S-48   PASS  Cookie header accepted by call_kw
       PASS  context.session_id still accepted (frontend path unbroken)
S-42   PASS  session id changed across authentication
       PASS  pre-auth id rejected after login (fixation closed)
       PASS  rotated id works
```

The S-42 check follows the actual attack: take an anonymous id from `get_session_info`,
authenticate while presenting it, then try to use the **pre-auth** id. It is rejected, and the
rotated id works.

### Eviction timer, end to end

With `session_ttl_minutes = 1`, five anonymous sessions were created and left idle:

```
[sessions] evicted 5 expired; 0 live - Container.hpp:352
```

Config restored to 60 afterwards.

### Unit tests (`scripts/test_sessionmanager.cpp`) — 19 assertions, all passing

Covers what HTTP cannot reach quickly:

- expiry actually reclaims (100 sessions, 1 s TTL → all evicted)
- store respects the cap (500 creates against a cap of 50 → size 50)
- **an authenticated session survives a 200-session anonymous flood** against a cap of 20
- `rotate()` carries uid / login / isAdmin / groupIds, invalidates the old id, updates
  `sessionId`, and returns empty for unknown or expired ids
- ids are 32 hex chars, distinct

### Regression

Suites from `042` re-run green: S-39 probes still inert, C-3/C-4 still guarded, S-47 audit rows
still written, S-40/S-49 buckets still per-client.

---

## 6. Files

**Modified**
```
core/infrastructure/SessionManager.hpp    rotate(), maxSessions_, anonymous-first eviction,
                                          evictExpiredLocked_ / evictOldestAnonymousLocked_
core/infrastructure/JsonRpcDispatcher.hpp cookieSid_() helper (S-48); resolveSessionId_ no
                                          longer creates (S-43); rotation on login (S-42)
core/infrastructure/HttpServer.hpp        HttpConfig::sessionTtlMinutes
core/Container.hpp                        startSessionEviction_(); TTL from config
config/system.cfg                         session_ttl_minutes
```

**New**
```
scripts/verify_session_fixes.sh           runtime checks for S-42 / S-43 / S-48
scripts/test_sessionmanager.cpp           19-assertion unit test
docs/043-session-security-fixes.md        this file
```

---

## 7. Stage 1 status

| # | Item | Status |
|---|---|---|
| 1 | S-39 command injection → RCE | ✅ Fixed, verified (exploitability proven pre-fix) |
| 2 | S-40 rate limiter keyed on proxy IP | ✅ Fixed, verified |
| 3 | S-47 identity/privilege audit | ✅ Fixed, verified |
| 5 | **S-42 session fixation** | ✅ **Fixed, verified** |
| 6 | **S-43 unbounded sessions** | ✅ **Fixed, verified** |
| — | **S-48 cookie ignored** | ✅ **Fixed, verified** |
| — | S-49 rate limiter counted nothing | ✅ Fixed, verified |
| 8 | C-3 category cycles | ✅ Fixed, verified |
| 9 | C-4 category delete | ✅ Fixed, verified |
| 11 | S-44 local file access | ✅ Fixed, no regression |
| 4 | Untrack `system.cfg`, rotate DB password | ⬜ Open |
| 7 | S-41 domain field allowlist | ⬜ Open |
| 10 | C-1 recursive `product_count` | ⬜ Open |
| 12 | S-45 ORDER BY rebuild | ⬜ Open |
| 13 | Strip dead ACL entries | ⬜ Open |
| — | S-46 wkhtmltopdf on noble — verify on the VM | ⬜ Open |

**Ten of sixteen Stage 1 items closed**, including every HIGH. What remains is one config
chore, three low-severity code items, and one thing that can only be checked on the target VM.

---

## 8. Note on the verification scripts

`scripts/verify_*.sh`, `test_sessionmanager.cpp` and `negctl_s39.cpp` are throwaway harnesses,
not the test suite. They should be folded into the Stage 3 `ERP_TEST` harness (`040` §1) so
S-39, S-40, S-42, S-43, S-47, S-48 and S-49 each gain a permanent regression test.

`test_sessionmanager.cpp` is closest to production shape and is the natural first file to port
— it needs only a `main()` swap for the `ERP_TEST` macro.

# 041 — Security Fixes Implementation Record

**Date:** 2026-08-02
**Baseline:** `0c798e5`
**Implements:** S-39, S-40, S-44, S-47 (incl. S-35/S-37 residue), C-3, C-4 — from `038`/`039`/`040`
**Build:** `cmake --build ./build` — clean, no errors
**Status:** Implemented and **runtime-verified** — see `042-runtime-verification-results.md`

> **Verification outcome:** all fixes confirmed working against a running server.
> S-39's exploitability was confirmed *pre-fix* by a negative control, so the fix is
> demonstrated rather than assumed. Verification also uncovered two pre-existing bugs:
> **S-49** (the login rate limiter never counted a failed attempt — fixed) and
> **S-48** (`call_kw` ignores the session cookie — open, to be fixed with S-42/S-43).
> A `ValidationError` exception type was added so C-3/C-4 messages reach the user.

---

## 1. Summary

| Finding | Severity | Status | Approach |
|---|---|---|---|
| **S-39** Command injection → RCE | HIGH | ✅ Fixed | Removed the shell entirely; removed request data from temp paths |
| **S-40** Rate limiters see only proxy IP | HIGH | ✅ Fixed | Proxy-aware `ClientIpResolver` + nginx config |
| **S-47** Identity/privilege changes unaudited | HIGH | ✅ Fixed | `audit_()` helper in `BaseViewModel`; 19 call sites |
| **S-44** `--enable-local-file-access` | LOW–MED | ✅ Fixed | Flag dropped from both invocations |
| **C-3** Category cycles hang the browser | MED | ✅ Fixed | Recursive-CTE ancestry check on write |
| **C-4** Category delete scatters subtree | MED | ✅ Fixed | Refuse delete with children/products |
| **S-38** CSV routes bypass record rules | MED | ✅ Already fixed | Verified in `d087308` — 5 `setUserContext` sites in `IrModule.cpp` |
| **S-35** Custom ViewModels bypass record rules | MED | ⚠️ Partial | See §5 — honest limits |

Two new files, ten modified. No schema change. No API change.

---

## 2. S-39 — command injection in PDF generation

### Root cause

`/report/pdf/{model}/{id}` and `/portal/api/{invoice,order,delivery}/{id}/pdf` built a temp
path from raw URL segments and passed it to `std::system()`:

```cpp
std::string tmpBase = "/tmp/erp_report_" + model + "_" + idStr;   // ReportModule.cpp:1309
...
+ " \"" + tmpHtml + "\"" + " 2>/dev/null";
int ret = std::system(cmd.c_str());                               // :1442
```

The id guard did not constrain the string: `std::stoi("12$(id)")` returns `12` **without
throwing**, and `$(...)` expands inside double quotes in `sh`. The portal route needed only a
customer login.

### Approach — remove the class, not the instance

Point fixes (escaping, stricter regex) would leave a `std::system()` call with request-derived
input one refactor away from reopening. Instead, two shared primitives in
**`core/infrastructure/ProcessRunner.{hpp,cpp}`** (new):

**`runProcess(argv, &exitCode, timeoutMs)`** — `fork()` + `execvp()` with an argv array.
No shell exists, so no argument can be reinterpreted as syntax and quoting stops being a
security concern. Also adds a **30 s timeout**: `std::system()` had none, and wkhtmltopdf can
hang on malformed HTML, permanently consuming a Drogon worker thread. The child's
stdout/stderr go to `/dev/null`, replacing the old `2>/dev/null`.

**`SecureTempDir`** — RAII `mkdtemp()` directory, mode 0700, removed with its contents by the
destructor. Files inside use fixed literal names (`doc.html`, `footer.html`, `out.pdf`).

This removes two problems at once. Request data no longer reaches the filesystem path, and the
old **deterministic** paths were independently a symlink-attack target — any local process
could pre-plant a symlink at `/tmp/erp_report_sale.order_12.html` and have the server write
through it. An unpredictable 0700 directory closes that too.

### Changes

- `modules/report/ReportModule.cpp` — `SecureTempDir`; argv vector; `runProcess`;
  `Content-Disposition` now uses the parsed `recordId` and a charset-filtered model name
  (`idStr` in a response header was CRLF-injectable — same class, different sink).
- `modules/portal/PortalModule.cpp` — same; `portalRunWkhtmltopdf()` lost its `tmpBase`
  parameter and `makePdf()` lost `idStr` entirely, so the unsafe value has no path to the
  function. All three PDF routes now use `std::to_string(recordId)` in their filenames.

### Residual

`recordId` is an `int` parsed by `std::stoi`; `model` in the report route is still used for the
DB lookup, but `renderDoc_` rejects unknown models and the value no longer reaches a shell or
a filesystem path.

---

## 3. S-40 — proxy-aware client IP

### Root cause

Three sites used `req->getPeerAddr().toIp()`. Behind nginx that is always `127.0.0.1`, so the
per-IP limiter became one global bucket:

1. Ten failed logins from anyone locked out **every** backoffice user and **every** portal
   customer for 5 minutes — an unauthenticated DoS on both login endpoints.
2. `recordSuccess()` calls `table_.erase(ip)` on that shared bucket, so an attacker holding any
   valid credential could reset the counter every ~9 attempts and brute-force indefinitely.

### Approach

**`core/infrastructure/ClientIp.hpp`** (new, header-only — a few string ops, no heavy includes,
and `JsonRpcDispatcher` is itself header-only, so PERF-E's TU-weight concern does not apply).

`ClientIpResolver::operator()(req)`:

1. If the socket peer is **not** in the trusted-proxy list → return the peer. Header trust is
   meaningless when the app is reached directly; otherwise a client could pick its own bucket.
2. Else prefer `X-Real-IP` (single-valued; nginx overwrites any client value).
3. Else take the **last** `X-Forwarded-For` element.

**Why the last element.** `$proxy_add_x_forwarded_for` *appends* the peer nginx saw:

```
client sends nothing    ->  "203.0.113.7"
client sends "1.2.3.4"  ->  "1.2.3.4, 203.0.113.7"
```

The last element is the address nginx observed and the only one a client cannot forge. Taking
the **first** element — the common implementation of this — reads attacker-controlled data.

IPv4-mapped IPv6 (`::ffff:127.0.0.1`) is normalised so it matches a `127.0.0.1` entry.

### Wiring

| File | Change |
|---|---|
| `core/infrastructure/HttpServer.hpp` | `HttpConfig::trustedProxies` (default `127.0.0.1,::1`) |
| `core/Container.hpp` | Parses `trusted_proxies`; explicit `False` disables header trust (the shared `get()` maps `""`/`False` to the default, so opting out must be explicit) |
| `core/factories/Factories.hpp` | `ServiceFactory::trustedProxies()` so modules can build a resolver |
| `core/infrastructure/JsonRpcDispatcher.hpp` | `clientIp_` member; both `authenticate` sites |
| `modules/portal/PortalModule.{hpp,cpp}` | `trustedProxies_` member; resolver captured by the login lambda |
| `config/system.cfg` | `trusted_proxies`, plus `http_interface=127.0.0.1` and `secure_cookies=True` |

### nginx

**`deploy/nginx/c-erp.conf`** (new) — full production config: TLS, HSTS, the
`X-Real-IP`/`X-Forwarded-For` block S-40 depends on, WebSocket upgrade, 6 MB body cap matching
the CSV limit, 60 s proxy timeouts (above the app's 30 s PDF timeout so nginx never masks the
real error), gzip, and edge `limit_req` zones keyed on `$binary_remote_addr` as defence in
depth. Ends with the multi-tenant variant from `040` §2.

---

## 4. S-47 / S-37 — identity and privilege auditing

### Root cause

`AuditService::log()` was called only from `GenericViewModel`. `d087308` fixed the five modules
review v10 named (account, sale, stock, purchase, mrp), but v10 scoped the finding to those
five and never enumerated the ViewModel registry. Eight custom ViewModels with mutating
handlers still did not audit — including the two most audit-critical models in the system.

### Approach

Rather than eight independent copies of the audit call, added `audit_()` to
**`modules/base/BaseViewModel.hpp`**, so a custom ViewModel joins the audited path with one
line per handler and cannot get the model name or the readiness check subtly wrong.
`modelName()` is virtual, so the right model is recorded automatically. It never throws —
audit failure must not break the operation being audited.

### Call sites (19 total)

| Model | File | Operations |
|---|---|---|
| `res.users` | `modules/auth/AuthViewModel.hpp` | create, write, unlink |
| `res.groups` | `modules/auth/AuthModule.cpp` | create, write, unlink |
| `res.company` | `modules/auth/AuthModule.cpp` | create, write, unlink (+ `setUserContext`) |
| `res.partner` | `modules/base/BaseModule.cpp` | create, write, unlink |
| `product.category` | `modules/product/ProductModule.cpp` | create, write, unlink |
| `ir.report.template` | `modules/report/ReportModule.cpp` | write |
| `portal.partner` | `modules/portal/PortalModule.cpp` | write, `set_portal_password` |
| `mail.message` | `modules/mail/MailModule.cpp` | create |

Two deliberate choices:

- **`res.users` write** is audited after the handler's password and group-membership branches,
  so credential changes and privilege grants are both covered by the one call.
- **`set_portal_password`** is logged under its own operation name rather than `write`, so
  portal credential changes are greppable in the trail instead of hiding among field edits.

---

## 5. S-35 — partial, and why

S-35 (custom ViewModels bypass record rules) is **not fully closed**, and the distinction
matters:

- ✅ ViewModels that operate through a `BaseModel` proto now call `setUserContext()` —
  `res.company` was fixed here; the account/sale/stock/purchase/mrp set was fixed in v9.
- ⚠️ ViewModels built on **hand-written SQL** — `ProductCategoryViewModel`,
  `GroupsViewModel`, `PortalPartnerViewModel`, `ReportTemplateViewModel` — cannot be fixed by
  adding a call. `setUserContext()` only affects `BaseModel`'s query builder; these bypass it
  entirely. Enforcing rules there means rewriting each query to merge the rule domain.

For these four the practical impact is low (categories, groups, templates are not per-user
data, and the portal ViewModel has its own `partner_id` scoping). But it is a real gap, and
patching it invisibly would be worse than naming it.

**The durable fix is the structural one in `040` §1.2** — move audit, rule-domain merge and OCC
into `BaseViewModel` so they are inherited by construction, plus a boot-time assertion that
every ViewModel with a mutator either inherits the enforced path or is on a named allowlist.
S-35, S-37, S-38 and S-47 are four instances of one defect; this pass fixed the instances that
could be fixed cheaply, and the pattern fix remains scheduled before the rental module adds
nine more models.

---

## 6. Verification status

**Done:** full build clean; no `std::system` outside comments; no `getPeerAddr` outside
`ClientIp.hpp`; no `--enable-local-file-access`; S-38's `setUserContext` calls confirmed
present.

**Not done — these need a running server and are the next step:**

| # | Check | How |
|---|---|---|
| 1 | S-39 no longer executes | `curl -b "$C" 'http://127.0.0.1:8069/report/pdf/sale.order/1$(touch%20/tmp/pwned)'` then `ls /tmp/pwned` → must not exist |
| 2 | PDF still renders | Fetch a normal invoice/order PDF from both backoffice and portal; confirm the footer survives (patched-Qt `--footer-html`) |
| 3 | S-44 did not break the logo | Any template using a local `file://` image will now render blank — check the templates in use |
| 4 | S-40 limits per client | 11 bad logins from host A must **not** lock out host B, through nginx |
| 5 | S-40 header spoofing | `curl -H 'X-Forwarded-For: 1.2.3.4'` must not change the bucket (nginx overwrites `X-Real-IP`) |
| 6 | S-47 rows appear | Create a user, grant a group, delete the user; `SELECT * FROM audit_log WHERE model IN ('res.users','res.groups')` |
| 7 | C-3 rejected | Try to set a category's parent to its own child → error, no hang |
| 8 | C-4 rejected | Try to delete a category with children → error naming the counts |
| 9 | PDF timeout | Confirm a hung render returns 503 after ~30 s rather than pinning a worker |

**Item 3 is the most likely regression.** Dropping `--enable-local-file-access` is correct for
S-44 but will blank any local-file image a document template references. If a logo disappears,
the fix is an explicit allowlisted asset path — not restoring the flag.

---

## 7. Files

**New**
```
core/infrastructure/ProcessRunner.hpp     shell-free exec + SecureTempDir (declaration)
core/infrastructure/ProcessRunner.cpp     implementation
core/infrastructure/ClientIp.hpp          ClientIpResolver (header-only)
deploy/nginx/c-erp.conf                   production reverse-proxy config
docs/041-security-fixes-implementation.md this file
```

**Modified**
```
core/Container.hpp                        trusted_proxies parsing; ServiceFactory + dispatcher wiring
core/factories/Factories.hpp              ServiceFactory::trustedProxies()
core/infrastructure/HttpServer.hpp        HttpConfig::trustedProxies
core/infrastructure/JsonRpcDispatcher.hpp ClientIpResolver at both authenticate sites
modules/base/BaseViewModel.hpp            audit_() helper (+ AuditService include)
modules/base/BaseModule.cpp               res.partner audit
modules/auth/AuthViewModel.hpp            res.users audit
modules/auth/AuthModule.cpp               res.groups + res.company audit, res.company setUserContext
modules/mail/MailModule.cpp               mail.message audit
modules/portal/PortalModule.{hpp,cpp}     S-39, S-40, S-44, portal.partner audit
modules/product/ProductModule.cpp         audit + C-3 cycle guard + C-4 delete guard
modules/report/ReportModule.cpp           S-39, S-44, ir.report.template audit
config/system.cfg                         loopback bind, secure_cookies, trusted_proxies
```

---

## 8. Remaining Stage 1 items (from `040` §1.1)

Not in this pass:

| # | Item | Sev | Effort |
|---|---|---|---|
| 4 | Untrack `system.cfg`, rotate DB password | HIGH | 1 h |
| 5 | **S-42** rotate session ID on authenticate | MED | 2 h |
| 6 | **S-43** `evictExpired()` timer; stop storing anonymous sessions | MED | 3 h |
| 7 | **S-41** validate domain field names against FieldRegistry | MED | 4 h |
| 10 | **C-1** recursive `product_count` | MED | 2 h |
| 12 | **S-45** rebuild ORDER BY from validated tokens | LOW | 2 h |
| 13 | Strip dead ACL entries | LOW | 30 m |
| — | **S-46** wkhtmltopdf on noble — verify on the VM | MED | 2 h |

`config/system.cfg` now carries the corrected production values but **is still tracked in
git**, and the password in history still needs rotating. That is item 4 and it is unchanged by
this pass.

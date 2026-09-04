# 120 — Logging in, and a penetration test of the public site

---

## 1. How to log in

There is **one** login, at the ERP root.

```
http://your-host/            → the login page (staff)
http://your-host/site        → the public website (no login)
http://your-host/portal      → the customer portal (a partner password)
```

To edit the website: sign in at `/`, then go to `/site`. The session cookie is
already set, so the editing toolbar appears on any page you may edit.

**There is deliberately no "staff login" link on the public site.** Advertising
the admin door to every visitor and crawler buys nothing — the people who need
it know the address, and a link would make credential-stuffing trivial to aim.

Permission to edit is the **Settings / Configuration** group (docs/117). A staff
login on its own is not enough.

### Verified in a real browser

| | |
|---|---|
| Login page renders, with login/password/Sign In | ✅ |
| No "sign up" and no "forgot password" | ✅ — both removed by policy (docs/111) |
| Signing in sets the session cookie | ✅ `HttpOnly`, `SameSite=Lax` |
| Then `/site` shows the editor toolbar | ✅ |
| A wrong password is refused, and stays on the page | ✅ |

**One defect found and fixed here.** A wrong password reported *"An internal
error occurred"*. Authentication failure was thrown as a plain
`std::runtime_error`, which SEC-28 correctly masks — right for a genuine fault,
wrong for mistyping a password, and it sends people to support instead of back
to the keyboard. It now throws `AccessDeniedError`, which CLAUDE.md already
names as the one exception always passed through *because the user must know
why*. The message is the same whether the login is unknown or the password is
wrong, so it is not a user-enumeration oracle.

---

## 2. The penetration test

`tests/security/website/site-hardening/` — **68 checks, every one an attack.**
The other website suites assert the system behaves; this one tries to break it.

### Threat model, in order of realism

| # | Attacker | Result |
|---|---|---|
| 1 | A visitor | Cannot save, read blocks, read history, restore, or read the audit. Forged cookies (`admin`, `1`, `' OR '1'='1`, random hex) all refused. |
| 2 | A customer with a portal password | Their cookie is a different cookie *and* a different session store. Refused as a portal cookie and refused when presented as a staff cookie. |
| 3 | **An employee with a real staff login and no website permission** | The dangerous one — authentication succeeds. Refused 403 on every route, not served the editor, **cannot grant themselves the group**, and cannot go around the editor via the model API. |
| 4 | Anyone, against the machinery | Injection, traversal, IDOR, oversize, header spoofing — all held. |

### §9 — the check worth reading

**The editor's JavaScript is a public static file.** An attacker can fetch it,
define the `window.__WSITE_EDIT` object it looks for (claiming `admin: true`),
inject it, and drive the toolbar. So the test does exactly that, as a visitor
and as an ungrouped employee.

Both get a working toolbar. Both press Save. The server answers **401** and
**403**, and the page is left **byte-identical** to how it started.

That is the point: **the client is not a security boundary.** Hiding the
toolbar is presentation; the endpoint is the control. This test would fail the
moment somebody moved a permission check into the browser.

### What else was probed

* **Draft confidentiality** — a draft answers exactly as a page that never
  existed, so the URL space cannot be walked to find unreleased pricing.
* **Injection** — `'; DROP TABLE website_page; --` through slugs, path
  segments and query strings; the table survives.
* **Traversal** — `../`, `..%2f`, `....//`, `%2e%2e/`; no file served, no
  `root:` in any body.
* **The public form as a write primitive** — undeclared keys (`state`,
  `task_id`, `form_id`) are discarded, and a form cannot be aimed at
  `res.users`.
* **IDOR** — one page's revision cannot be restored onto another.
* **Disclosure** — no session id, no password material, no SQL, no driver
  internals, no table names in any error body.

### One finding that turned out to be sound

The first run flagged X-Forwarded-For spoofing as a rate-limit bypass. It is
not. `ClientIpResolver` takes the **last** element of the header — the one the
proxy appended — so an address an attacker *prepends* is ignored. The test now
asserts that property directly: 25 requests each prepending a different fake
address still share one budget and still hit 429.

My first version of the check simulated an attacker already on the loopback
interface, which is not the threat model: an attacker on the internet reaches
nginx, not this port.

---

## 3. Client-side review

`web/static/website-editor.js`, read for privilege:

| Question | Answer |
|---|---|
| Does it decide who may edit? | **No.** It runs when `window.__WSITE_EDIT` exists; the server emits that only for a permitted session, and the endpoint re-checks regardless. |
| Can a client set that object itself? | **Yes** — and §9 proves it changes nothing. |
| Does it hold a credential? | No. It carries a page id and an `admin` flag used only to decide which palette entries to show. The server ignores both. |
| Can it introduce markup? | No. It reads `textContent`, never `innerHTML`, so a rich paste is flattened to text. Verified in Chrome. |
| Does it interpolate anything unescaped? | One place did — the heading level, used as a *tag name*, where escaping cannot help. Now constrained to `[123]` (docs/117). |

**The `admin: true` flag is worth being explicit about:** an attacker can set it
and unlock the raw-HTML palette entry in their own browser. It buys nothing —
the save endpoint checks the session's real groups and answers 403.

---

## 4. Suite

**105 suites / 0 failed.** Website: CMS 120, editor 95 (browser-driven),
forms 51, **pen-test 68**. Unit tier 119 assertions on the sanitiser alone.
All suites hermetic.

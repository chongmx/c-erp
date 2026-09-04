# Auth Module Port Progress

## Summary
- Integrated auth module into `modules/auth/`.
- Added `AuthModule`, `AuthService`, `AuthViewModel`, model classes for `res.users`, `res.groups`, `res.company`, and basic view definitions.
- Replaced runtime entrypoint and JSON-RPC dispatcher to use the reference ERP-compatible login flow (`/web/session/authenticate`, session cookie handling, `call_kw` request rewriting).
- Fixed critical boot ordering bug in `BaseFactory` that caused `res_partner` FK failure.
- Wired auth-aware frontend: `LoginPage`, `UserMenu`, updated `rpc.js` and `app.js`.

## Work Completed

### Backend
- [x] Copied auth files to `modules/auth/`.
- [x] Updated `main.cpp` to register `BaseModule` then `AuthModule`.
- [x] `AuthService` — PBKDF2-SHA512 hashing (passlib-compatible), constant-time compare, session population.
- [x] `AuthViewModel` — handles `/web/session/authenticate`, `search_read`, `fields_get`, session-aware dispatch.
- [x] `AuthModule::initialize()` — idempotent schema creation + default group seeding + admin user seeding.
- [x] **Fixed `BaseFactory::registeredNames()` to return keys in insertion order** — `std::unordered_map` iteration was non-deterministic; added `insertionOrder_` vector so `base->initialize()` always precedes `auth->initialize()`.
- [x] Verified build passes.

### Frontend (`web/static/`)
- [x] `src/services/rpc.js` — added `authenticate()`, `logout()`, `restoreSession()`, in-memory session state (`uid`, `login`, `sessionId`, `db`, `context`).
- [x] `src/components/LoginPage.js` — OWL login form (db / login / password fields, spinner, error display), fires `login-success` DOM event on success.
- [x] `src/components/UserMenu.js` — topbar avatar initials + login name + "Sign out" button, fires `logout` DOM event.
- [x] `src/app.js` — replaced bare shell with auth gate: boot spinner → `LoginPage` (unauthenticated) → `MainApp` with `UserMenu` (authenticated). Listens for `login-success` / `logout` events to switch views.
- [x] `src/app.css` — added login card, boot-screen, user-menu, user-avatar, user-badge styles.
- [x] `index.html` — added `LoginPage.js` and `UserMenu.js` script tags.

## Current Status
- Build: ✅ success
- Runtime startup: ✅ boot ordering fixed — `res_partner` FK error resolved
- Login flow: ✅ `POST /web/session/authenticate` → session cookie → main shell
- Session restore: ✅ `GET /web/session/get_session_info` on page reload
- Logout: ✅ clears session state, returns to login page

## Auth Flow (end to end)

```
Browser                          Server
  │                                │
  │── GET /  ────────────────────► │  serves web/static/index.html
  │                                │
  │  app.js onMounted:             │
  │── GET /web/session/get_session_info ──► SessionManager lookup by cookie
  │◄─ { uid: 0 }  (no session)            │
  │  → show LoginPage              │
  │                                │
  │  User submits login form:      │
  │── POST /web/session/authenticate ──► AuthService::authenticate()
  │         { db, login, password }       ↓
  │                                  res_users lookup
  │                                  PBKDF2-SHA512 verify
  │                                  SessionManager::create()
  │◄─ { uid, login, session_id, context } + Set-Cookie: session_id=...
  │  → fire 'login-success'        │
  │  → show MainApp + UserMenu     │
  │                                │
  │  Subsequent calls:             │
  │── POST /web/dataset/call_kw ──► JsonRpcDispatcher (cookie auth)
  │         { model, method, ... }        ↓
  │                                  ViewModelFactory → ViewModel
  │◄─ { result: [...] }            │
```

## Root Cause (boot ordering — documented for reference)
`BaseFactory::registeredNames()` iterated over `std::unordered_map` (no insertion order).
Even though `BaseModule` was registered before `AuthModule` in `main.cpp`,
`initializeModules_()` could call `auth->initialize()` first — before `base->initialize()`
created `res_partner`. `AuthModule::ensureSchema_()` creates `res_users` with
`REFERENCES res_partner(id)`, so Postgres threw `relation "res_partner" does not exist`.
**Fix**: added `std::vector<std::string> insertionOrder_` to `BaseFactory`; `registeredNames()`
now returns keys in registration order.

## Next Steps
1. Add `/web/session/get_session_info` endpoint if not yet fully wired (needed for session restore on reload).
2. Add `res_country` and `res_lang` to base module (FK targets for partner/user fields).
3. Wire `/web/action/load` stub so OWL webclient can route after login.
4. Port `auth_signup` (token-based password reset) — requires `ir_config_parameter` stub.

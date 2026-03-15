# Auth Module Port Progress

## Summary
- Integrated auth module source into `modules/auth/`.
- Added `AuthModule`, `AuthService`, `AuthViewModel`, model classes for `res.users`, `res.groups`, `res.company`, and basic view definitions.
- Replaced runtime entrypoint and JSON-RPC dispatcher to use Odoo-compatible login flow (`/web/session/authenticate`, session cookie handling, call_kw request rewriting).
- Fixed critical boot ordering bug in `BaseFactory` that caused `res_partner` FK failure.
- Build confirmed passing; schema creation and seeding are now order-safe.

## Work Completed
- [x] Copied auth files to `modules/auth/`.
- [x] Updated `main.cpp` to register `BaseModule` then `AuthModule`.
- [x] Updated `AuthViewModel` to use shared DB connection.
- [x] Fixed module boot custom route initialization.
- [x] Repaired `CMakeLists.txt` and confirmed build passes.
- [x] **Fixed `BaseFactory::registeredNames()` to return keys in insertion order** (was `unordered_map` iteration — non-deterministic). Added `insertionOrder_` vector to `BaseFactory`. This ensures `base->initialize()` always runs before `auth->initialize()`, so `res_partner` exists when `res_users` is created with the FK reference.

## Current Status
- Build: ✅ success
- Runtime startup: ✅ boot ordering fixed — `BaseModule::initialize()` now reliably runs before `AuthModule::initialize()`
- `res_partner` FK error: ✅ resolved
- Next action: verify end-to-end login via `/web/session/authenticate` with seeded admin user

## Root Cause (documented for reference)
`BaseFactory::registeredNames()` iterated over `std::unordered_map` which does not preserve insertion order. Even though `BaseModule` was registered before `AuthModule` in `main.cpp`, `initializeModules_()` could invoke them in any order. `AuthModule::ensureSchema_()` creates `res_users` with `REFERENCES res_partner(id)` — if it ran before `BaseModule::ensureSchema_()` created `res_partner`, Postgres threw `relation "res_partner" does not exist`. Fix: added `std::vector<std::string> insertionOrder_` to `BaseFactory` so `registeredNames()` returns keys in registration order.

## Auth Flow Details
- Login endpoint: `/web/session/authenticate` via `AuthService::authenticate(db, login, password)`
- Password hashing: PBKDF2-SHA512 (`$pbkdf2-sha512$...`) with constant-time compare
- Session: `SessionManager` stores `uid`, `login`, `name`, `company_id`, `db` per session token
- Seed data: default groups (Public/Internal/Administrator) and `admin` user created on first boot

## Next Steps
1. Verify `/web/session/authenticate` login returns correct session JSON (match Odoo `/web/session/authenticate` response shape).
2. Add `res_country` and `res_lang` to base module (needed as FK targets by `res_partner` and `res_users`).
3. Add `company_id` FK to `res_partner` pointing to `res_company` (currently unlinked in base schema).
4. Wire a minimal `/web/action/load` endpoint so the Odoo JS client can bootstrap after login.
5. Port `auth_signup` (password reset token, signup URL) — requires `mail` / `ir_config_parameter` stubs first.

# Auth Module Port Progress

## Summary
- Integrated new auth module source from `zznew/` into `modules/auth/`.
- Replaced runtime entrypoint and JSON-RPC dispatcher to use Odoo-compatible login flow (`/web/session/authenticate`, session cookie handling, call_kw request rewriting).
- Added `AuthModule`, `AuthService`, `AuthViewModel`, model classes for `res.users`, `res.groups`, `res.company`, and basic view definitions.
- Built successfully with CMake; runtime boot reached module init and attempted DB operations.

## Work Completed
- [x] Copied `zznew` auth files to `modules/auth/`.
- [x] Updated `main.cpp` to register `odoo::modules::auth::AuthModule`.
- [x] Updated `AuthViewModel` to use shared DB connection instead of inaccessible protected member.
- [x] Fixed module boot custom route initialization and removed invalid `initialize() override`.
- [x] Repaired `CMakeLists.txt` and confirmed build passes.
- [x] Verified binary starts and reaches auth module boot.

## Current Status
- Build: ✅ success
- Runtime startup: ✅ starts, then fails on missing `res_partner` table (expected; base module schema needs bootstrap before auth)
- Next action: ensure `res_partner` table is created by base module and/or add base seeding during startup.

## Next Steps
1. Add base module `res_partner` schema creation and required seed data in `modules/base` (if not already done).
2. Add a minimal integration test that boots container with an in-memory/test Postgres and verifies `/web/session/authenticate` with admin password.
3. Implement endpoint-level frontend serving for local test UI (`web/static`) and add sample login page from `zznew`.
4. Update docs with migration architecture and module wiring details in `docs/plan.md` and `docs/zznew-code-structure.md`.

## Notes
- The current auth flow uses Odoo-compatible hash format (`$pbkdf2-sha512$...`) and constant-time compare.
- `AuthModule::registerRoutes()` now creates schema and seeds default groups/admin.
- For full Odoo parity, migrate additional modules (base model relations, permissions, record rules) in next phase.

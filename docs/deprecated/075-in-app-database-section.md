# 075 — In-app Database section (backup / restore / export / import)

## Goal

Let a **company admin** manage their **own** company's database — snapshot, download,
upload, restore, delete — from **Settings → Database & Backups**, without shell access
and without ever being able to touch another tenant's data.

The user's constraint: *"take care of the security part of the snapshot, export, import,
roll back."* Chosen model (confirmed): **in-app, hard-gated**.

## Security envelope

Every `/web/db/*` endpoint enforces, in order:

1. **Authenticated + admin** — `dbAdmin_()` resolves the session and requires
   `session.isAdmin`. Non-admins and anonymous callers get refused.
2. **Own tenant only** — every operation is scoped to `session.db`. The client never
   supplies a database name. Backups live under a **per-tenant directory**
   `backups/<session.db>/`, so a company physically cannot see or address another
   company's snapshots. Cross-tenant download returns 404 (path is scoped, not filtered).
3. **Password re-confirmation for destructive ops** — `restore` re-verifies the caller's
   password (`AuthService::verifyPassword` against the `res_users` hash queried **in the
   caller's tenant DB** under a `TenantScope`). Wrong password → refused, DB untouched.
4. **No shell** — `pg_dump` / `pg_restore` are invoked via `fork` + `execvp` with an argv
   array (`core/DbBackup.hpp`); no string is ever passed to a shell, so filenames and
   labels can't inject. `PGPASSWORD` is passed through the child env, never the command line.
5. **Filename validation** — `DbBackup::validFile` rejects anything that isn't a bare
   `*.dump` (no `/`, no `..`), closing path traversal on download/restore/delete/upload.
6. **Full audit** — every op writes `AuditService::log("db.backup", op, …, uid)`.

## Pieces

- `core/DbBackup.hpp` — `fork`/`execvp` runner; `backup` (`pg_dump -Fc`), `restore`
  (`pg_restore --clean --if-exists --no-owner`), `list` (newest-first), filename guards.
- `core/infrastructure/DbConnection.hpp` — `tenantConfig(db)` returns a tenant's
  `DbConfig` for pg_dump/restore (falls back to the default connection).
- `modules/auth/AuthService.hpp` — public `verifyPassword(plaintext, storedHash)` wrapper.
- `core/infrastructure/JsonRpcDispatcher.hpp` — `POST /web/db/{list,backup,restore,delete}`
  (JSON) + raw `POST /web/db/upload?name=` (octet-stream body) + `GET /web/db/download`
  (file response). Helpers: `dbAdmin_`, `dbBackupDir_`, `dbTenantCfg_`,
  `verifySessionPassword_`, `dbAudit_`.
- Frontend: `web/static/src/components/DbBackups.js` (CUSTOM_VIEWS `db.backups`),
  `web/static/src/services/rpc.js` (`dbList/dbBackup/dbRestore/dbDelete/dbUpload/
  dbDownloadUrl`), CSS in `partkeepr.css` (`.db-*`), menu under Settings via
  `ReportModule.cpp` (ir_act_window id 72 → menu 132).

## Test

`tools/test_db_backups.sh` — throwaway PG with two tenants (`mc_a`, `mc_b`). Proves
backup/list/download, restore round-trip (a post-snapshot partner is gone after restore),
and the full security envelope: wrong password refused with no DB change, unauthenticated
refused, and strict per-tenant isolation (B cannot see or download A's snapshots; separate
dirs; 404 for cross-tenant download). **16/16 checks pass.**

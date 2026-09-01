# scripts/deprecated/

Nothing in here is run, referenced, or maintained. It is kept because each file
records *how* something was established, and a one-off that proved a thing is
cheaper to re-read than to re-derive.

Delete the folder whenever you want; nothing depends on it.

| file | why it is here |
|---|---|
| `temp.sh` | A byte-for-byte copy of `scripts/set_admin_password.sh` (only the trailing newline differs). Use that one. |
| `probe_domain_leak3.sh` | The S-49 blind-extraction probe. Superseded by `tests/security/injection/domain-field-allowlist/`, which asserts the same oracle instead of printing it. The file is also truncated mid-`curl` — it was never finished. |
| `negctl_s39.cpp` | Negative control for S-39: reproduces the pre-fix `std::system` interpolation so "no marker file appeared" against the fixed build means something. Its job is done; the fix is covered by `tests/security/hardening/security-fixes/`. |
| `negctl_run_tests.sh` | Negative control for the *runner* — plants a failing and a silently-dying script and asserts both are scored red. Broken since docs/109: it plants `scripts/verify_zzz_*.sh`, which the new runner never discovers (it globs `tests/**/test.sh`). Worth rebuilding against `tests/run.sh`, which is why it is kept rather than deleted. |
| `precision_demo.cpp` | The measurement that decided int64 money over `double` (docs/047). The cases it found are now asserted in `tests/unit/money/test_money.cpp`, which cites this file. |
| `test_money_migration.sql` | Dry-run of the P2 money migrations inside a rolled-back transaction. The migrations shipped; `tests/tools/verify_ledger_integrity.sql` is the standing check. |
| `cleanup_test_data.sh` | Removed suite debris that accumulated before the scripts cleaned up after themselves (docs/092). Obsolete since docs/104: `tests/run.sh` snapshots, restores a baseline, and restores your data back, so nothing accumulates. Destructive — do not run it speculatively. |
| `_patch_auth.py` | A one-off that added cookie jars to four `scripts/verify_rental_*.sh` files when the `/rental/` routes started authenticating. Those four scripts no longer exist. |
| `diag_session.sh` | Ad-hoc probe of how `call_kw` resolves a session (cookie vs body context). Answered; the behaviour is now covered by `tests/security/auth/session-fixes/`. |
| `render_dashboard_preview.py` | Rendered the rental dashboard to static HTML so layout could be eyeballed. Superseded by the real browser driver, `tests/lib/render.mjs` (`tests/docs/browser-render-checks.md`), which renders the actual component instead of a copy of its geometry. |
| `render_grid_preview.py` | Same, for the unit grid. |
| `render_portal_units_preview.py` | Same, for the portal "My Units" page. |

# 077 — Integration suite made hermetic (0 failures)

## Before

`docs/070` documented ~6–7 integration tests that failed on any database without
hand-seeded demo data. They were not code defects — the probes assumed baseline
records that a real, cleaned database does not carry:

| Test | Why it failed |
|------|---------------|
| `verify_money_display`, `verify_money_recompute`, `verify_tax_engine` | `SELECT id FROM sale_order LIMIT 1` returned empty (0 sale orders), so the report/recompute/tax probes had no record to drive — the empty id interpolated into a malformed request ("Invalid JSON"). |
| `verify_money_roundtrip` | Hardcoded `product_product id=3` / `account_move id=4` with exact expected values — hand-seeded ids that no longer exist. |
| `verify_no_double_audit` | `mrp.bom` probe used `product_id=1` (absent → create SKIPped), leaving <4 models probed. |
| `verify_security_fixes` | PDF exec-path probe hardcoded `sale.order/2`; also printed `*** ONE OR MORE CHECKS FAILED ***`, a verdict string `run_tests.sh` doesn't recognise (it greps for `FAILURES`), so a completed-with-failures run was mis-scored "no verdict". |
| `verify_ir_cron` | Timing-sensitive; passed in isolation. |

## Fix — make the suite carry its own data

1. **`scripts/seed_test_fixtures.sh`** — a shared, idempotent seeder. Through the
   real API (so scaling/totals are correct) it guarantees ≥1 sale order with an
   **untaxed** line exists (untaxed because `verify_money_recompute` rewrites the
   first line and asserts `subtotal == total`). Creates nothing if a line already
   exists. Sourced by `run_tests.sh` before the verify loop, and by each
   data-dependent script (so they are hermetic standalone too).

2. **`verify_money_roundtrip`** — rewritten hermetic: discovers a real product and
   invoice at runtime and asserts the *relationship* `major == micros / 1e6` in SQL
   (no float rounding, no hardcoded id or value).

3. **`verify_no_double_audit`** — the `stock.picking` / `mrp.bom` probes now resolve
   a real `picking_type_id` / `product_id` at runtime, so ≥4 models always probe.

4. **`verify_security_fixes`** — PDF probe uses a discovered sale-order id; verdict
   string corrected to `*** FAILURES ***` (the documented convention in CLAUDE.md).

5. **`verify_ir_sequence` §5** — previously *always SKIPped* (needed an ambient draft
   order that never existed). With fixtures present it ran for the first time and was
   found to be non-repeatable — it consumed whatever draft happened to exist. Rewritten
   to **create its own draft order** each run and poll for the sequence commit, so it is
   deterministic and idempotent.

## After

```
47 passed, 0 failed   —   Everything green.
```

Green on two consecutive runs (the hermetic tests no longer depend on or mutate
shared ambient state), so `./scripts/run_tests.sh` is now a trustworthy gate: a
green run can no longer be a run that silently skipped its most important probes.
This supersedes the "known baseline failures" caveat in `docs/070`.

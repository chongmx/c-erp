# 083 — Tax Report (Malaysian SST-02)

The third Accounting slice: the **SST-02** output-tax return — the sales & service
tax a Malaysian business reports bi-monthly to RMCD — built on the financial-report
engine (docs/081) and the tax engine already in place.

## Tax classification

Sales Tax and Service Tax are distinct SST-02 lines, so `account.tax` gains a
`tax_group` (`sales` | `service` | `other`), editable on the tax form. On startup:

- a migration classifies any unclassified tax by name/scope (`… service …` →
  service; a sale-scope tax → sales);
- the **standard Malaysian rates** are seeded idempotently — **Service Tax 8 %**,
  **Sales Tax 10 %**, **Sales Tax 5 %** — so a fresh chart is Malaysia-ready. The
  existing generic 15 % taxes are left intact.

## The return

`GET /web/account/report?report=tax_report` computes output tax from the **posted tax
lines** (`tax_line_id`) of customer invoices and credit notes over the period, grouped
into **Sales Tax** / **Service Tax** sections. For each tax code it shows the taxable
amount and the tax; SST is single-stage, so the taxable amount is derived exactly as
`tax ÷ rate`. Sections subtotal, and the report ends with **Total Tax Payable**.
Customer credit notes (`out_refund`) net down the tax via `SUM(credit − debit)`.

It appears as the **Tax Report (SST-02)** tab in Accounting ▸ Reporting ▸ Financial
Reports, with the same date filter and Print/PDF as the other statements.

## Verified

`scripts/verify_sst_tax_report.sh` (in the suite) posts a Service Tax 8 % invoice
(RM80 on RM1,000) and asserts the accounting: the SST taxes are seeded/classified,
the report renders, **Total Tax Payable equals the net posted output tax in the
ledger**, and every line reconciles (`taxable amount × rate == tax`). Browser-verified
(0 JS errors): the Service Tax section shows *Service Tax 8 % · 1,000.00 · 80.00* and
**Total Tax Payable 80.00**.

> A test-data note that bit once: `account.move.line` rows created without a `date`
> are skipped by the date-filtered reports, so a dateless line whose recompute-tax
> line *is* dated unbalances the Trial Balance. Real invoices always date their lines;
> the SST test now does too, and existing dateless lines were backfilled from their
> move date.

Full suite: **51 passed, 0 failed**, green on two consecutive runs.

## Next

Build order continues: **Assets** (register, depreciation, Generate Entries),
then **Budgets**, then the config round-out.

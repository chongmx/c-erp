# 081 — Financial statement reports (Accounting → Reporting)

The first slice of the Odoo14-style Accounting reporting suite: the statutory
financial statements a Malaysian business needs for SSM filing / audit, computed
from the real ledger (posted `account.move.line`, BIGINT micro-units) so they are
double-entry correct by construction.

## Reports

| Report | Basis | Key rule |
|--------|-------|----------|
| **Trial Balance** | as at a date | per-account Σdebit / Σcredit / balance; total debit == total credit |
| **Profit & Loss** | a period | Income (credit-normal) − Expenses (debit-normal) = Net Profit |
| **Balance Sheet** | as at a date | Assets = Liabilities + Equity, with P&L rolled into **Current Year Earnings** |
| **General Ledger** | a period | every posted line per account, with opening → running → closing balance |
| **Aged Receivable** | as at a date | open customer invoices bucketed Not-due / 1-30 / 31-60 / 61-90 / 90+ |

Account classification uses the standard `account_type` prefixes (`asset_*`,
`liability_*`, `equity*`, `income*`, `expense*`), so it adapts to whatever chart of
accounts is configured.

## Backend (`modules/report/ReportModule.cpp`)

- `financialReport_(txn, report, date_from, date_to)` → a uniform JSON
  `{ title, subtitle, columns[], rows[] }` where each row is
  `{ type: line|section|subtotal|total, cells:[…] }` — one shape the on-screen UI
  and the print view both render.
- `GET /web/account/report?report=&date_from=&date_to=` → JSON (auth-gated).
- `GET /web/account/report/print?…` → a self-contained printable HTML document
  (company header, styled table) for **Print → PDF** from the browser.

## Frontend (`web/static/src/components/AccountReports.js`)

A `account.report` CUSTOM_VIEW: a tab bar for the five reports, a date filter
(range vs. as-at per report), **Refresh** and **🖨 Print / PDF**, and a dark-theme
table that styles section headers, subtotals and totals. Menu:
**Accounting → Reporting → Financial Reports** (act_window 73 → menu 25).

## Verification

`scripts/verify_financial_reports.sh` (in the suite) asserts the accounting
invariants, not just that the endpoint responds:

- Trial Balance: **total debit == total credit**
- Balance Sheet: **total assets == total liabilities + equity**
- P&L **net profit == the Balance Sheet's Current Year Earnings**
- the print/PDF view renders

Browser-verified (0 JS errors): the Balance Sheet shows Assets 1,820.00 =
Liabilities + Equity 1,820.00. Full suite: **49 passed, 0 failed**.

## Next (not in this slice)

Credit Notes & Refunds, SST tax codes + SST-02 return, e-Invoice/MyInvois, Assets &
Budgets — the other Accounting areas, to be built on this same report/ledger base.

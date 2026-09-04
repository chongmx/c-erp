# 087 — Bank Accounts register + adjustable Accounting Dashboard

Two of the four items that were flagged *Revisit* on the coverage map, now that
their scope is pinned down. (**Payment Acquirers** stays parked; **Settings** is
inventoried at the bottom of this doc and will be built last.)

## Bank Accounts ("Add a Bank Account")

Scoped as requested: a bank account is a master record plus **a register of lines —
index, description, date, debit, credit**.

- **`account.bank.account`** — name, bank, account number, journal, currency,
  active. Menu: **Configuration → Bank Accounts**.
- **`account.bank.account.line`** — `sequence` (the index), `date`, `name`
  (description), `debit`, `credit` (BIGINT micro-units like the rest of the ledger).

`BankAccountFormView` renders the register with the columns above plus a **running
Balance** per row, a **Totals** footer, and a Balance stat tile. The index and the
running balance are **computed for display**, never stored, so they cannot drift; on
save, `sequence` is renumbered from the row order.

## Accounting Dashboard — adjustable

`GET /web/account/dashboard` returns six cards computed live from the ledger:
**Customer Invoices** (unpaid), **Vendor Bills** (to pay), **Bank**, **Cash**,
**Assets** (book value), **Budgets** (planned) — each with an amount and a count.

Adjustable, as asked: a **Customize** button reveals a per-card checkbox panel, and
the selection is persisted server-side in `ir_config_parameter`
(`account.dashboard.cards`) via `?cards=a,b,c`, so it survives a reload and is shared
rather than being a per-browser preference. Menu: **Accounting → Dashboard** (first
entry, the app's landing view).

## Verified

`scripts/verify_bank_dashboard.sh` (in the suite): both menus resolve; a bank account
with three register rows (+1000, +250, −400) reports **balance RM850 = Σdebit −
Σcredit** and reads back in index order; the dashboard returns all six cards; and the
card selection **saves and is read back** (then the default set is restored so the app
is left usable). Browser-verified (0 JS errors): dashboard cards with live figures +
the Customize panel; the bank register showing `# · Description · Date · Debit ·
Credit · Balance` with a Totals row.

Full suite: **55 passed, 0 failed**.

---

## Settings — inventory (to build last)

What the reference ERP's **Accounting → Configuration → Settings** actually exposes, so we can
pick from a concrete list. Items marked ✅ already exist in c-erp under another menu
and would just be surfaced here.

**Fiscal / company**
- Fiscal year end (last day/month), fiscal periods
- Company currency ✅ (`res.currency`), multi-currency toggle + rate provider
- **Lock dates** — journal-entry lock, tax-return lock, "all users" lock date
- Rounding method / cash rounding

**Customer invoices**
- Default payment terms ✅, default sales journal ✅
- Invoice numbering (sequence & prefix) ✅ (INV / RINV series)
- Sending: email invoices, print/PDF layout ✅ (document templates)
- Warnings on customers, credit limits
- Snailmail / online payment (needs Payment Acquirers — parked)

**Vendor bills**
- Default purchase journal ✅, bill digitisation (OCR — out of scope)
- Purchase receipt handling, 3-way matching

**Taxes**
- Default sales/purchase tax ✅ (`account.tax`), tax rounding (per line / per tax)
- **Tax return periodicity** (bi-monthly for SST-02), reminder day
- Fiscal positions ✅, cash-basis vs accrual tax

**Analytics & management**
- Analytic accounting ✅, analytic tags/groups
- **Budgets** ✅, budgetary positions ✅
- **Assets** ✅ + asset types ✅, deferred revenue/expense

**Bank & cash**
- Bank accounts ✅ (this doc), bank feeds/sync (out of scope)
- Reconciliation models, cash journals ✅
- Payment Acquirers — **parked at your request**

**Suggested first cut when we build Settings:** a single Settings screen with
sections that (a) surface the ✅ items as links to their existing menus and (b) add
the genuinely new switches — **fiscal year end, lock dates, tax periodicity, default
taxes/journals, and multi-currency**. Lock dates are the highest-value new item since
they protect closed periods.

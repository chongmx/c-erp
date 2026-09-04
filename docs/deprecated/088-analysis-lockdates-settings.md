# 088 — Analysis reports, Lock Dates, Settings (Accounting build complete)

The last slice: the analysis & audit reports, period **Lock Dates**, the
**Settings** screen, and the remaining dropdown entries. With this, every item on
the reference ERP-14 Accounting coverage map is built except **Payment Acquirers**, which is
parked at your request.

## Analysis & audit reports

Five reports added to the same engine/UI as the financial statements (docs/081) —
they appear as tabs in **Reporting → Financial Reports**, sharing its date filter
and Print/PDF:

| Report | What it shows |
|---|---|
| **Aged Payable** | open vendor bills bucketed Not-due / 1-30 / 31-60 / 61-90 / 90+ (mirror of Aged Receivable) |
| **Partner Ledger** | per-partner receivable/payable movements, opening → running → closing |
| **Journals Audit** | per journal: entry count, debit, credit, and every entry inside it |
| **Invoice Analysis** | per customer: invoice count, untaxed, tax, total, outstanding |
| **Product Margins** | per product: qty sold, revenue, cost, margin, margin % |

## Lock Dates

Two config parameters — `account.lock_date` (all users) and
`account.tax_lock_date` (periods whose tax return is filed). `handleActionPost`
refuses to post any entry dated **on or before** the latest applicable lock, raising
a `ValidationError` so the user sees *"This period is locked…"* rather than an
Internal Error. Lifting the lock lets the entry post normally.

## Settings

**Configuration → Settings** (`/web/account/settings`), a card-per-section screen
that saves as you edit:

- **Fiscal year** — last day / last month
- **Lock dates** — the two dates above (accent-highlighted, since they are the
  destructive-ish control)
- **Taxes** — default sales/purchase tax, **tax return periodicity** (defaults to
  bi-monthly for SST-02), tax rounding
- **Default journals** — sales / purchase, plus a multi-currency switch
- **Configured elsewhere** — chips naming the 14 config screens that have their own
  menus, so this screen orients rather than duplicates

Writes are **allowlisted** server-side: a key outside the known set is rejected, so
the endpoint can't be used to write arbitrary `ir_config_parameter` rows.

## Remaining dropdown entries

- **Customers ▸** Products, Customers · **Vendors ▸** Products, Vendors
  (`res.partner` filtered by `customer_rank` / `vendor_rank`).
- **Accounting ▸ Journals ▸** Sales / Purchases / Bank and Cash / Miscellaneous —
  entries filtered by move type.
- A one-time backfill sets `customer_rank` / `vendor_rank` from existing invoices
  and bills, so those two lists aren't mysteriously empty on a database whose
  partners were never ranked.

## Verified

`scripts/verify_lockdates_settings.sh` (in the suite): all five reports render; the
settings endpoint loads and saves; **an unknown settings key is rejected**; and the
lock date behaves — posting into a locked period is **refused as a ValidationError**,
the entry **stays draft**, a later date still posts, and lifting the lock lets the
blocked entry post. The test removes its probe entries and clears the locks, so it is
repeatable and leaves the app usable. Browser-verified (0 JS errors): the Settings
screen (5 sections, 10 inputs, 14 links), 11 report tabs, and the new dropdown items.

Full suite: **56 passed, 0 failed**.

> Frontend gotcha, twice now: OWL templates don't expose JS globals. `parseFloat`
> (docs/085) and `String()` here both threw "an error occured in the owl lifecycle"
> until moved into component methods (`remainingOf()`, `sid()`). If a new view dies
> on mount, look for a global in the template first.

## Status

Everything on the coverage map is built except **Payment Acquirers** (parked).

**Update (docs/090):** the map was generous about one entry — "Employee Expenses,
covered by vendor bills" was not the same thing as an expense claim, and there
was no `hr.expense` at all. It exists now: Employees ▸ Employee Expenses and
Accounting ▸ Expense Reports, with a submit → approve → post → reimburse
workflow. Payment Acquirers remains the only parked item.

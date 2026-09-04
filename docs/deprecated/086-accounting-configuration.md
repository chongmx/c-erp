# 086 — Accounting configuration round-out

The sixth Accounting slice: the remaining entries in the reference ERP's **Configuration**
dropdown — Currencies, Account Types, Fiscal Positions, Incoterms and Journal
Groups. These are reference data, so they ride on the polished generic form
(docs/078) rather than needing bespoke views.

## What was added

| Menu | Model | Notes |
|------|-------|-------|
| **Currencies** | `res.currency` | model already existed in `base`; this wires the Configuration menu to it |
| **Account Types** | `account.account.type` *(new)* | the classification list behind `account.account.account_type` — name, technical code, internal group |
| **Fiscal Positions** | `account.fiscal.position` *(new)* | name, country, notes, auto-apply, active, **+ a Tax Mapping sub-table** |
| ↳ tax mapping | `account.fiscal.position.tax` *(new)* | one substitution row: *tax on product → tax to apply* |
| **Incoterms** | `account.incoterms` *(new)* | code + name reference list |
| **Journal Groups** | `account.journal.group` *(new)* | a named set of journals (`journal_ids_json`) |

## Seeds (idempotent)

- **Account types** — the 13 classifications the chart of accounts uses
  (`asset_cash`, `asset_receivable`, `liability_payable`, `income`, `expense`,
  `expense_depreciation`, …), so the list always covers what the chart references.
- **Incoterms** — the 11 standard terms (EXW, FCA, FAS, FOB, CFR, CIF, CPT, CIP,
  DAP, DPU, DDP).

Both insert only what is missing (`WHERE NOT EXISTS`), so they never fight a
hand-edited row.

## UI

All five use the generic form, which since docs/078 renders a proper card with a
two-column grid and an **Add new…** ＋ beside every many2one. The fiscal position's
`tax_ids` is a One2many, so the form renders it as the editable **Tax Mapping**
sub-table with *+ Add a line* — no bespoke component required.

## Verified

`scripts/verify_account_config.sh` (in the suite): each Configuration menu resolves
to the right model; both reference lists are seeded; **every `account_type` used by
the chart exists in the Account Types list**; all five models create + read back
through the API; and a fiscal position accepts a tax substitution row linked to it.
Repeatable — it cleans up its own `QA …` records. Browser-verified (0 JS errors):
Currencies 6 rows, Account Types 13, Incoterms 11, Journal Groups 1, and the Fiscal
Position form showing the card + Tax Mapping table.

Full suite: **54 passed, 0 failed**.

## Next

Remaining in the build order: **analysis & audit reports** (Invoice Analysis,
Product Margins, Journals Audit, Partner Ledger, Aged Payable), then **Lock Dates**.
Still flagged *Revisit* pending your input: Settings, Add a Bank Account, Payment
Acquirers, and the Accounting Dashboard.

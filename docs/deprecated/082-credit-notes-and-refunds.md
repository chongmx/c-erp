# 082 — Credit Notes & Vendor Refunds

The second Accounting slice: customer **Credit Notes** (`out_refund`) and vendor
**Refunds** (`in_refund`). The data model and payment reconciliation already
supported the refund move types; what was missing were the menus, the credit-note
form mode, and a real reversal (`action_reverse` was a stub).

## How a credit note is created — reversal

`account.move.action_reverse(invoice_id)` builds the credit note by **reversing** a
posted invoice: every ledger line is copied with **debit and credit swapped** into a
new draft move whose `move_type` is flipped (`out_invoice → out_refund`,
`in_invoice → in_refund`). Swapping the legs of a balanced entry yields a balanced
entry with the opposite effect, so the credit note is correctly signed *by
construction* — the invoice's receivable **debit** becomes the credit note's
receivable **credit** — without re-deriving anything. It returns the new move id.

Business rules (per request):

- **Numbering** — a credit note is a "reverse invoice" and draws from its **own
  continuous series with prefix `RINV`** (`RINV000001`, …), separate from the `INV`
  series. Seeded by migration 1021; `handleActionPost` routes `out_refund` to it.
- **Reference document** — the credit note's Source (`invoice_origin`) stays the
  originating **sales order** (copied from the invoice's origin), not the invoice.
- **Line tagging** — every reversed line carries the **invoice number** it reverses
  in its name and `ref` (`… (reverses INV000208)`), so the credit note is
  self-documenting.
- The move links back via `reversed_entry_id`, starts as **draft** for review, then
  posts and can be reconciled against the invoice (existing payment flow).

## UI

- Menus: **Customers → Credit Notes**, **Vendors → Refunds** (act_windows 74/75,
  filtered by `move_type`); Vendors also gains **Payments**.
- `InvoiceFormView` labels the document by `move_type` ("Credit Note" / "Vendor
  Refund") and shows an **Add Credit Note / Add Refund** button on a posted
  invoice/bill → `action_reverse` → the draft credit note (found under its menu).

## Verified

`scripts/verify_credit_note.sh` (in the suite) asserts the accounting, not just a
200: type flips to `out_refund`, links back, **balances (Σdebit == Σcredit)**, the
receivable **flips sign**, Source = the invoice's SO origin, lines carry the invoice
number, and posting yields an **`RINV…`** number. Browser-verified (0 JS errors):
Credit Notes list + a posted credit note (`RINV000001`, "Reversal of INV000193").
Full suite: **50 passed, 0 failed**.

## Follow-ups

Standalone hand-entered credit notes (New on the list) still use invoice-sign
convention in `recompute_totals`; the reversal path is the correct, tested workflow.
Next Accounting areas: Tax Report / SST-02, Assets, Budgets.

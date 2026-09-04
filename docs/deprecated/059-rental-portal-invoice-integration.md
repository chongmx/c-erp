# 059 — Rental invoices in the customer portal

**Date:** 2026-08-05
**Implements:** `057` §2 · migration 813
**Status:** ✅ Complete and verified

---

## The starting position was better than expected

Your instinct was right: the portal invoice feature already existed, and rental invoices are
ordinary `account_move` rows, so they were already flowing through it. `PortalModule.cpp`
registers, all cookie-authenticated and partner-scoped:

```
/portal/api/invoices              list
/portal/api/invoice/{id}/detail   line detail
/portal/api/invoice/{id}/print    printable HTML
/portal/api/invoice/{id}/pdf      PDF download
/portal/api/invoice/{id}/proof    customer uploads proof of payment
```

So this was **integration, not construction**. What was missing was the origin link.

---

## Tying the invoice to its rental origin

the reference ERP ties an invoice to the sale order it came from, and this codebase already follows that
pattern:

| | |
|---|---|
| `account_move.invoice_origin` | a **registered** Char field on the model — so it reaches the API and portal for free |
| `account_move.sale_id` | a raw column plus an FK, added by **SaleModule's own** migration (`SaleModule.cpp:1375`) |

Rental mirrors it exactly rather than inventing a parallel scheme. Migration 813 adds
`account_move.rental_contract_id` with an FK — the module that owns the referenced table adds
the column, as sale does.

Billing now sets both:

- `invoice_origin` = the contract name (`RENT/2026/0001`), or `Rental <period>` for a walk-in
- `rental_contract_id` = the contract, or **NULL** for a walk-in

**Nullable is the point.** A walk-in genuinely has no contract, and the invoice is no less
real for that. Inventing a placeholder contract to satisfy an FK would put a row in the
database that represents nothing.

### The line-level link already existed

`rental_invoice_link` records which tenancy and which period every invoice line covers. That
*is* the "what am I actually paying for" answer, so the portal detail endpoint joins it rather
than storing anything new:

```json
"covers": [{"unit_code": "PT-A1", "unit_name": "Alice unit",
            "period_start": "2026-09-01", "period_end": "2026-09-30",
            "amount": 150.0}]
```

### Rental-ness is derived from the links, not the contract id

`portalRenderDoc` titles the document "Rental Invoice" when
`EXISTS (SELECT 1 FROM rental_invoice_link WHERE move_id = am.id)` — **not** when
`rental_contract_id IS NOT NULL`.

Using the contract id would have titled a walk-in's invoice "Sales Invoice", because a walk-in
has no contract. The link rows exist for every generated rental invoice either way. The test
asserts both directions: a walk-in invoice *is* detected as rental, and a genuinely
non-rental invoice is *not*.

---

## What the customer sees

The invoice list gained `origin` and `is_rental`, because a storage tenant's list is otherwise
a column of identical amounts differing only by date:

```json
{"id": 280, "name": "SAL/2026/0088", "amount_total": 172.5,
 "origin": "PTEST/RENT/0001", "is_rental": true, "payment_state": "not_paid"}
```

The invoice lines already named the unit and period — `Unit PT-A1 (Alice unit) — 2026-09-01
to 2026-09-30` — since the billing engine writes that label.

---

## Verification: a real session, a real PDF, a real refusal

`verify_rental_portal.sh` — 30 checks. It does not assert against SQL predicates standing in
for the portal; it logs in.

Portal auth lives on `res_partner` (`portal_password_hash`, `portal_active`), not `res_users`,
and the hash is PBKDF2 with a random salt — so it cannot be forged in SQL. The test uses the
`portal_reset_password` admin action, which is also the flow a real operator follows.

```
with no session:  invoices -> 401,  pdf -> 401
as Alice:         her invoice listed, origin shown, is_rental true
                  Bob's invoice NOT in her list
                  detail names the unit and the period
                  PDF: HTTP 200, 20524 bytes, magic '%PDF'
```

### The negative control

An access check exercised only with the *right* customer proves nothing. So Bob logs in and
asks for Alice's invoice:

```
Bob -> Alice's invoice:   detail=404   pdf=404   print=404
Bob -> his OWN invoice:   pdf=200
```

The last line matters as much as the first three: without it, three 404s would be equally
consistent with the whole feature being broken.

---

## Nothing new was needed for access control

`portalRenderDoc` already scopes with `WHERE am.id=$1 AND am.partner_id=$2`, returning empty
for someone else's invoice, which the route turns into a 404. That was correct before this
work and is now *proved* correct rather than assumed.

`ir.rule` record rules scoped by `partner_id` remain worth adding as defence in depth behind
the explicit scoping, but they are not what stands between two customers today.

---

## My Units

`GET /portal/api/units` plus a page in the portal's own stack
(`portal.html` / `portal.js`). Per `046` §7 — customers want one number and a list, so no
dashboard and no charts:

- a **hero balance** with overdue called out separately, because that is the part needing
  action
- a **card per unit**: code, type, zone, since, rate, next period

Scoped on `rental_contract_line.partner_id` — the customer on the tenancy itself, since a
walk-in has no contract to scope through.

Three details worth recording:

**The monthly total normalises the billing interval**, exactly as the dashboard's MRR does. A
quarterly tenancy contributes a third of its amount, or the customer's own page would tell
them they pay three times what they do.

**Next period is shown only for auto-billed units.** A walk-in is invoiced by hand, so
printing a next date would be a promise the system cannot keep. Asserted in the test.

**The balance comes from the same invoices the Invoices tab shows** — asserted equal to an
independently computed SQL residual, so the two pages cannot tell the customer different
things.

The portal now lands on My Units rather than Invoices: for a storage tenant that is the
answer to "what am I renting and what do I owe", which is why they logged in.

---

## Verification summary

```
verify_rental_portal   43 checks   origin link (contract + walk-in), coverage detail,
                                   rental-vs-sales titling both ways, real portal
                                   session, genuine PDF download, cross-customer
                                   negative control, My Units scoping + balance
                                   agreement + walk-in date suppression
```

`scripts/render_portal_units_preview.py` renders the page from live data through the real
stylesheet, provisioning portal access the way an operator would.

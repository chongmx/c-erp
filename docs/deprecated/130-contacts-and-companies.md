# 130 — Contacts and companies: finishing the partner hierarchy

> **Status:** phase 0 is DONE and green. Phases 1–5 are planned, not started.
> **Reference:** Odoo 14 `odoo/addons/base/models/res_partner.py`, vendored at
> `zzref2/odoo14/`. Line numbers below are from that file.

---

## 1. Why this document exists

A user could not add a contact to a customer's company. The system answered:

```
You cannot create records for another company.
```

The guard was right and the caller was wrong. `res.partner` carries **two**
different notions of "company", and the contact form had been written against
the wrong one:

| field | means | set by |
|---|---|---|
| `parent_id` → `res.partner` | the customer's company — Acme Sdn Bhd, who you sell to | the user |
| `company_id` → `res.company` | the **tenant** that owns the row — which ERP subscriber's data this is | the server |

The form put the customer's partner id into `company_id`. No `res.company` has
that id, so the multi-company rule refused the write. Adding a contact to a
customer looked forbidden when it was simply mis-addressed.

Odoo 14 draws the same two lines, which is the strongest evidence the split is
right rather than merely ours:

```python
parent_id  = fields.Many2one('res.partner', string='Related Company', index=True)   # 158
child_ids  = fields.One2many('res.partner', 'parent_id', string='Contact')          # 160
company_id = fields.Many2one('res.company', 'Company', index=True)                  # 216
company_name = fields.Char('Company Name')                                          # 230
```

Two failures made this expensive, and both are worth naming because the fixes
below are shaped by them:

* **`res_partner` had no `parent_id` at all.** There was no partner→partner
  relation, so there was nowhere correct to put the company. `company_id` was
  the only field that looked like an answer.
* **`create()` accepted `parent_id` and silently discarded it**, returning a
  fresh id and reporting success. Unknown keys were dropped without a word, so
  the API said "done" while linking nothing.

---

## 2. What is already done (phase 0)

Shipped and green locally; **not yet deployed**.

| change | where |
|---|---|
| `parent_id INTEGER` + FK to `res_partner(id)` `ON DELETE SET NULL` | `modules/base/BaseModule.cpp` `ensureSchema_` |
| `CHECK (parent_id IS NULL OR parent_id <> id)` | same |
| `BEFORE INSERT OR UPDATE` trigger walking the chain for longer cycles | same |
| index `res_partner_parent_idx` | same |
| registered `Many2one("res.partner")`, on the form, domain `is_company = true` | `ResPartner::registerFields` / `PartnerFormView` |
| `company_id` relabelled **"Owner Company"** so the two stop colliding | same |
| `create`/`write` **reject** unknown fields instead of dropping them | `modules/base/BaseModel.hpp` `rejectUnknownFields_` |
| contact form sends `parent_id`, never `company_id` | `web/static/src/app.js` `ContactFormView` |

Tests now standing guard:

* `tests/integration/core/contact-company-link/` — 20 checks, the relation itself
* `tests/functional/11-customer-company/` — company → people → sell → rent
* `tests/security/access/partner-tenant-isolation/` — both halves: adding
  customers/suppliers/contacts must work, **and** tenants must not see each other

Our cycle guard is **stronger than Odoo's**: theirs is `_check_recursion()` in
the ORM (line 315), so raw SQL or another module can still build a loop. Ours
holds in the database.

---

## 3. What is missing, measured against Odoo 14

| # | Odoo 14 | ours | value |
|---|---|---|---|
| 1 | `commercial_partner_id` computed (289–297) | — | **high** |
| 2 | child inherits parent's tenant (369, 539) | — | **high** (correctness) |
| 3 | `type`: contact / invoice / delivery / other (185) | — | **high** (already has callers) |
| 4 | address inheritance from parent (344–349) | — | medium |
| 5 | `child_ids` One2many (160) | — | low |
| 6 | clear `company_name` when `parent_id` is set (529) | — | low |
| 7 | `name_get` → "Acme, Jane Tan" (647) | — | low |

---

## 4. Phase 1 — `commercial_partner_id`

**The problem it solves.** "Show me everything for this customer" currently
needs an OR across two columns:

```sql
WHERE partner_id IN (SELECT id FROM res_partner WHERE id = :acme OR parent_id = :acme)
```

That is what `tests/functional/11-customer-company` does by hand. It does not
index well, and it is wrong at depth — a contact under a *branch* under Acme is
missed. Odoo stores the answer instead (289–297):

```python
if partner.is_company or not partner.parent_id:
    partner.commercial_partner_id = partner
else:
    partner.commercial_partner_id = partner.parent_id.commercial_partner_id
```

**Design.** A **stored** column, maintained by trigger. We have no computed-field
machinery, and a trigger keeps it true for SQL that never goes through the model
— the same reasoning as the cycle guard.

```sql
ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS commercial_partner_id INTEGER
    REFERENCES res_partner(id) ON DELETE SET NULL;
CREATE INDEX IF NOT EXISTS res_partner_commercial_idx
    ON res_partner(commercial_partner_id);
```

Trigger, `BEFORE INSERT OR UPDATE OF parent_id, is_company`:

* `is_company` **or** no `parent_id` → itself
* otherwise → the parent's `commercial_partner_id`

Plus an `AFTER UPDATE` cascade: when a partner's own `commercial_partner_id`
changes, recompute descendants. Bound the walk at 64 hops, as the cycle trigger
does.

**Backfill** for existing rows, in the same migration:

```sql
UPDATE res_partner SET commercial_partner_id = id
 WHERE is_company OR parent_id IS NULL;
-- then iterate depth-first for the rest; depth is 1 today
```

**Tests.** `tests/integration/core/commercial-partner/`:

* a company is its own commercial partner
* a contact resolves to its company
* a contact under a *branch* resolves to the **top** company (the depth case the
  OR query gets wrong)
* re-parenting a contact updates it, and its descendants
* deleting the company clears it without deleting the contact
* the functional test's revenue query, rewritten as
  `WHERE commercial_partner_id = :acme`, returns the same total as the OR form

**Effort:** ~1 day. **Risk:** low — additive, nothing reads it yet.

---

## 5. Phase 2 — the tenant descends the hierarchy

Odoo sets the child's tenant from the parent (369) and cascades on write (539):

```python
if self.parent_id:
    self.company_id = self.parent_id.company_id.id
...
if partner.child_ids:
    partner.child_ids.write({'company_id': company_id})
```

We stamp the *current user's* company. Usually the same value — but not
guaranteed, and the failure is a contact in a different tenant from its own
company, which is exactly the class of bug this whole document is about.

**Design.** In `ResPartner`, when `parent_id` is set, take the parent's
`company_id` rather than the session stamp; reject the write if the parent
belongs to a tenant the user cannot see. Cascade to children on change.

**Tests.** Extend `partner-tenant-isolation/`:

* a contact created under a company inherits **that company's** tenant
* moving a company between tenants moves its contacts with it
* a contact cannot be parented to a company in another tenant — refused, not
  silently re-stamped

**Effort:** ~1 day. **Risk:** medium — touches the multi-company path (docs/094).
Run `tests/security/access/` in full.

---

## 6. Phase 3 — address types

`sale_order` **already has** `partner_invoice_id` and `partner_shipping_id`
(`modules/sale/SaleModule.cpp:98–99`). There is no way to create a partner that
*is* an invoice address, so those fields can only point at whole contacts. Odoo
(185):

```python
type = fields.Selection([('contact','Contact'), ('invoice','Invoice Address'),
                         ('delivery','Delivery Address'), ('other','Other')],
                        default='contact')
```

**Design.** `type VARCHAR NOT NULL DEFAULT 'contact'` with a CHECK on the four
values. Form gains the selector, shown only when `parent_id` is set — an address
type is meaningless on a standalone partner. The sale-order address pickers
filter on `parent_id = <customer> AND type IN ('invoice','delivery')`.

**Tests.** `tests/integration/sale/order-addresses/`:

* a company with separate invoice and delivery addresses
* a sale order defaults both to the customer when none exist
* the pickers offer only that customer's addresses — never another customer's
* invoicing uses `partner_invoice_id`, delivery uses `partner_shipping_id`

**Effort:** ~2 days including the frontend. **Risk:** medium — changes sale-order
behaviour; `tests/functional/01-sell` must stay green.

---

## 7. Phase 4 — address inheritance

Odoo copies `('street','street2','zip','city','state_id','country_id')` from
parent to contact when `type == 'contact'` (32, 344–349), only if the parent
actually has an address.

**Design.** Server-side on create when `parent_id` is set and the contact's own
address fields are empty. Deliberately **not** a live sync: Odoo warns that
changing a contact's company should be rare, and a background sync overwriting a
hand-typed address is worse than retyping it. We have no `street2`; add it here
or drop it from the set — decide during implementation.

**Tests.** Inheritance on create; an explicitly supplied address is **not**
overwritten; later edits to the parent do **not** clobber the child.

**Effort:** ~half a day. **Risk:** low.

---

## 8. Phase 5 — the small ones

* **`child_ids`** — a One2many convenience. `search([('parent_id','=',id)])`
  already answers it; add only when a screen needs it.
* **Clear `company_name` when `parent_id` is set** (529). Stops the free text and
  the relation disagreeing. Needs a data check first: today the form writes both,
  and `partner-tenant-isolation` asserts nothing *relies* on the text.
* **`name_get` → "Acme, Jane Tan"** (647). Cosmetic, but it is how a user tells
  two people named Lee apart in a dropdown.

**Effort:** ~1 day combined. **Risk:** low.

---

## 9. Sequencing and migration numbers

Phases 1 → 2 → 3 → 4 → 5, in that order. Phase 2 depends on phase 1 only for
tests; phase 3 is independent but should follow 2 so tenant rules are settled
before address pickers start filtering.

`MigrationRunner` reserves **1–99 for core/base** (`MigrationRunner.hpp`), and
base currently registers none — it does its schema work in `ensureSchema_`.
Highest version in use anywhere is **1041**. Phase 0 followed the local
convention and used `ensureSchema_`.

**These phases should use real migrations**, because they backfill data rather
than just adding columns, and `ensureSchema_` gives no ordering or
once-only guarantee:

| version | name |
|---|---|
| `10` | `res_partner_commercial_partner_id` (phase 1, column + trigger + backfill) |
| `11` | `res_partner_tenant_descends` (phase 2 backfill) |
| `12` | `res_partner_address_type` (phase 3) |

---

## 10. What must not regress

Every phase re-runs these three, and none may go red:

* `tests/security/access/partner-tenant-isolation/` — tenants stay separate,
  **and** ordinary users can still add customers, suppliers and contacts. Both
  halves. The bug that started this was a *usability* failure caused by a
  *security* field.
* `tests/functional/11-customer-company/` — company → people → sell → rent.
* `tests/functional/01-sell/` — phase 3 touches sale orders.

Two standing rules, both learned here:

* **A write that cannot be honoured must fail loudly.** `rejectUnknownFields_`
  exists because a silent drop turned a missing feature into a lie. Do not add
  per-model exemptions to quiet a caller; fix the caller.
* **Never assert only that a screen returns 200.** The form bug was invisible to
  every API test; it took driving the browser to see it. See
  `tests/docs/browser-render-checks.md`.

---

## 11. Open questions

1. **Do we want `commercial_partner_id` stored or derived?** Stored costs a
   trigger and a backfill; derived costs an OR on every report. Recommendation:
   stored — Odoo made the same call, and invoicing will want to group by it.
2. **How deep can hierarchies go?** Odoo allows arbitrary depth. Our triggers cap
   the walk at 64. If we only ever need company → contact, a depth-1 constraint
   would be simpler and faster; it would also be hard to relax later.
3. **Should `company_id` stay on the contact form at all?** It is now labelled
   "Owner Company", but the safest answer may be to remove it from the form and
   let the server own it entirely. Needs a call on whether multi-company admins
   ever legitimately re-assign a partner's tenant by hand.

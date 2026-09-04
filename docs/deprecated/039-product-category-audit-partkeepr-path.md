# 039 — Product Category Integration Audit & PartKeepr Path

**Date:** 2026-08-02
**Baseline:** HEAD `0c798e5`
**Relates to:** `029`/`030` (PartKeepr plans), `038` (pre-deploy audit)
**Status:** Proposed — for review

---

## 0. Correction to `037` §3 and `038` §3.3

Both earlier documents summarised PartKeepr status as "PK1 landed, a frontend-only category
tree". That undersold it. `product.category` has a **custom server-side ViewModel with
hand-written SQL** (`ProductModule.cpp:384`), server-computed counts, `name_search`, full
CRUD, and a tree that filters the product list **with descendant rollup**. It is a real
integration, not a UI shell.

The accurate statement is: **the category feature is ~80% integrated and has nine specific
defects**, three of which are user-visible today and one of which will hang the product
browser. PK2–PK7 remain unimplemented — that part of `038` §3.3 stands.

---

## 1. What actually works

Verified by reading `modules/product/ProductModule.cpp` and `web/static/src/app.js`:

| Capability | Where | State |
|---|---|---|
| Hierarchical schema (`parent_id` self-FK) | `ProductModule.cpp:682` | ✅ |
| Collapsible tree UI, depth indentation, expand/collapse | `app.js:8427` `ProductCategoryTree` | ✅ |
| Selecting a category filters products | `app.js:8875` `productFilteredAction` | ✅ |
| **Filter includes all descendants** | `app.js:8862` `categoryDescendantIds` → `[['categ_id','in',ids]]` | ✅ |
| `child_count` computed server-side | `ProductModule.cpp:435` subquery | ✅ |
| `product_count` computed server-side | `:436` subquery | ⚠️ see C-1 |
| Parent name resolved via JOIN | `:438` `LEFT JOIN ... par` | ✅ |
| `name_search` for many2one autocomplete | `:563` | ✅ |
| Full CRUD (create/write/unlink) | `:477`–`:549` | ⚠️ see C-4, C-6, C-7 |
| `categ_id` on product, in form + list views | `:132`, `:313`, `:342` | ✅ |

So: the browsing experience a PartKeepr user expects — pick a category, see everything under
it — **is present and works**.

---

## 2. Defects

### C-1 — `product_count` disagrees with the filter it sits next to (HIGH visibility)

`ProductModule.cpp:436` counts **direct assignments only**:

```sql
(SELECT COUNT(*) FROM product_product pp WHERE pp.categ_id = pc.id) AS product_count
```

But the filter (`app.js:8878`) rolls **descendants** up. So a parent category renders as
`Semiconductors (0)` while clicking it shows 200 products. Every non-leaf category in a real
tree displays a misleading zero. This is the single most noticeable defect and it makes the
hierarchy look broken even though it isn't.

**Fix (~2 h):** recursive CTE for the rollup count, keeping the direct count as a separate
field if the UI wants both:

```sql
WITH RECURSIVE sub AS (
    SELECT id FROM product_category WHERE id = pc.id
    UNION ALL
    SELECT c.id FROM product_category c JOIN sub ON c.parent_id = sub.id
)
SELECT COUNT(*) FROM product_product WHERE categ_id IN (SELECT id FROM sub)
```

### C-2 — Domain is silently ignored except two hardcoded fields (MEDIUM)

`handleSearchRead` (`:410`) walks the domain and honours **only** `active` and `parent_id`
leaves. Every other leaf hits `continue` and is discarded:

```cpp
if (field == "active" && op == "=" ...) { ... }
else if (field == "parent_id" && op == "=") { ... }
// anything else: silently dropped
```

`search_read('product.category', [['name','ilike','resistor']])` returns **every category**.
No error, no warning. Any current or future caller that relies on a domain to *restrict*
category results gets the full table instead — including anything that might later be used
for access scoping.

**Fix (~3 h):** use the standard `Domain`/`domainFromJson` compiler like every other model,
or at minimum fail loudly on unhandled leaves rather than dropping them.

### C-3 — Parent cycles are permitted and hang the browser (MEDIUM, availability)

Two halves, both required:

- `handleWrite` (`:522`) sets `parent_id` with **no ancestry check** — a category can be made
  its own descendant.
- `categoryDescendantIds` (`app.js:8862`) recurses with **no visited set**:

```js
const walk = (parentId) => {
    cats.forEach(c => { if (pid === parentId) { result.push(c.id); walk(c.id); } });
};
```

One cycle → unbounded recursion → stack overflow, and `result` grows without limit. The
product browser becomes unusable **for every user**, and the only repair is a DB edit. A
single mis-drag in the category editor is enough.

**Fix (~2 h):** reject the write server-side if the new parent is the record or one of its
descendants (recursive CTE); add a `Set` guard to `walk()` as belt-and-braces.

### C-4 — Deleting a category silently scatters its subtree and uncategorises its products (MEDIUM)

`handleUnlink` (`:540`) is an unguarded `DELETE`. Both foreign keys are `ON DELETE SET NULL`:

- `product_category.parent_id` (`:682`) → **all children are promoted to root level**
- `product_product.categ_id` (`:697`) → **all products in it become uncategorised**

Deleting "Semiconductors" scatters every subcategory to the top of the tree and strips the
category from every product beneath it. No confirmation, no error, and the damage is not
obviously attributable to the delete.

**Fix (~2 h):** refuse to unlink when `child_count > 0` or `product_count > 0`, with a message
naming the counts — the reference ERP's behaviour. Offer reparent-then-delete as the explicit alternative.

### C-5 — `product.category` is not audited — and it is one of eight (MEDIUM → see S-47)

**This section was wrong in its first draft and is corrected here.**

The original claim was that "the product module was missed by `d087308`, so categories *and
products* are unaudited". Two errors:

1. **`product.product` IS audited.** It is registered as
   `GenericViewModel<ProductProduct>` (`ProductModule.cpp:654`), and `GenericViewModel`
   calls `AuditService::instance().log()` in `handleCreate`/`handleWrite`/`handleUnlink`.
   Only `product.category` — which has a custom ViewModel — bypasses it.
2. **The method was wrong.** `grep -c AuditService <module>.cpp == 0` does *not* mean
   unaudited: models on the generic path are covered from the template, not from the module
   file. The correct test is *"does this model have a **custom** ViewModel with mutating
   handlers?"* Applying that test turns C-5 from a one-module oversight into **S-47** below.

So the category-specific defect stands (`ProductCategoryViewModel` at `:477`–`:549` mutates
without auditing), but it is one instance of a much larger gap. See §2a.

**Fix (~1 h for categories alone):** add `AuditService::instance().log()` to the three
handlers, matching `GenericViewModel`. But fix it as part of S-47, not on its own.

---

## 2a. S-47 — Identity and privilege changes are not audited (HIGH)

Chasing C-5 correctly surfaced a finding well outside the product module. Enumerating every
`viewModels_.registerCreator` and classifying custom vs. `GenericViewModel`, then checking
which custom ViewModels register `create`/`write`/`unlink`:

| Model | ViewModel | Module | Mutators | Audited |
|---|---|---|---|---|
| **`res.users`** | `AuthViewModel` (`AuthViewModel.hpp:42`) | auth | create, write, unlink | ❌ |
| **`res.groups`** | `GroupsViewModel` (`AuthModule.cpp:80`) | auth | create, write, unlink | ❌ |
| `res.company` | `CompanyViewModel` (`AuthModule.cpp:21`) | auth | create, write, unlink | ❌ |
| `res.partner` | `PartnerViewModel` (`BaseModule.cpp:507`) | base | create, write, unlink | ❌ |
| `product.category` | `ProductCategoryViewModel` (`ProductModule.cpp:384`) | product | create, write, unlink | ❌ |
| `ir.report.template` | `ReportTemplateViewModel` (`ReportModule.cpp:560`) | report | write | ❌ |
| `portal.partner` | `PortalPartnerViewModel` (`PortalModule.cpp:932`) | portal | write | ❌ |
| `mail.message` | `MailMessageViewModel` | mail | create | ❌ (low value) |

Everything else is either `GenericViewModel<T>` (audited from the template) or an audited
custom ViewModel in account / sale / stock / purchase / mrp.

### Why this is worse than a module oversight

`d087308` did exactly what review v10 asked: v10 scoped S-37 to *"the five custom ViewModel
hierarchies (Sale, Account, Stock, Purchase, Mrp)"*, and all five were fixed. The fix matched
the finding. **The finding was scoped too narrowly** — it was derived from the business-document
modules and never enumerated the whole ViewModel registry.

The result is that `audit_log` records business-document changes but **not** the identity and
privilege changes that matter most in an incident:

- **`res.users`** — user creation, password change, deactivation, deletion: no trail.
- **`res.groups`** — group membership *is* the privilege model here (`Session::hasGroup`,
  `checkModelAccess_`). Granting someone admin leaves no trail.

Create a user → grant it admin → act → delete the user, and `audit_log` shows only the
business records touched in step 3, attributed to a uid that no longer exists and whose
creation and privileges were never recorded. That is the exact sequence an audit trail exists
to reconstruct, and it is the one it cannot.

`ir.report.template` matters for a second reason: template edits are the injection vector for
S-44 (`--enable-local-file-access` → local file read into a PDF). That path is currently both
unaudited and unlogged.

**Fix (~1 day):** add `AuditService::instance().log()` to the mutating handlers of all eight
ViewModels above. Then close the class properly rather than case by case — either:

- move the audit call into `BaseViewModel` so it is inherited by construction, with an opt-out
  for read-only models; or
- add a startup assertion that every registered ViewModel exposing a mutator either audits or
  is on an explicit allowlist.

Without one of those, the next custom ViewModel silently reopens this. S-35, S-37, S-38 and
now S-47 are all the same structural defect: *features wired into `GenericViewModel` are
silently absent from custom ViewModels.* Four occurrences is enough to fix the pattern, not
the instances.

**Note for `038`:** S-47 belongs in Stage 1. It is higher-severity than several findings
already there, and the fix is mechanical.

---

### C-6 — No record rules in the product module (LOW here, structural) — *S-35 pattern*

`grep -c "setUserContext\|mergeRuleDomain" modules/product/ProductModule.cpp` → **0**.

`ProductCategoryViewModel` builds raw SQL and never merges the rule domain. Low impact for
categories specifically — they are not per-user data — but it is the same structural pattern
as S-35/S-37/S-38, and it means the module is outside the authorization framework entirely.

### C-7 — No optimistic concurrency control (LOW)

`handleWrite` (`:508`) ignores `__expected_write_date`. Two users renaming the same category
silently last-write-wins, while every model on the `GenericViewModel` path is protected. The
column is already stamped (`write_date=now()`), so only the guard is missing.

### C-8 — No `complete_name` / category path (LOW) — *PK1 spec not met*

`docs/029` PK1 specified a recursive-CTE `category_path` ("Electronics > Semiconductors >
ARM") so context is visible without a tree walk. Not implemented. Category many2ones render
as a bare leaf name, which is ambiguous when the same leaf name appears under several parents
— common in electronics ("Ceramic" under both Capacitors and Resistors).

### C-9 — Silent truncation at 500 categories (LOW)

Default limit 500 in `handleSearchRead` (`:429`); the frontend passes `limit: 500` in both the
tree loader and the product-form loader (`app.js:4001`). Past 500 categories the tree silently
loses nodes — and orphaned children vanish from the tree entirely, since their parent is
missing. A PartKeepr-style parts catalogue reaches 500 categories readily.

### C-10 — Many2one labels ship empty; the form compensates client-side (LOW)

`ProductProduct::serializeFields` (`:172`) emits `categ_id` as `[id, ""]` — the display name is
an empty string, and there is **no** custom product ViewModel to resolve it (products use
`GenericViewModel<ProductProduct>`). The product form works only because it pre-fetches *all*
categories, UoMs and accounts on every form load (`app.js:3999`) and resolves labels locally.
That is three extra round-trips per form open and it breaks past the 500 cap (C-9).

---

## 3. Is the category feature "fully integrated"?

**Browsing: yes.** Tree, descendant filtering, counts, autocomplete and CRUD are all wired
end-to-end through a purpose-built ViewModel.

**Framework integration: no.** The module sits outside four cross-cutting systems every other
module joined — audit (C-5), record rules (C-6), OCC (C-7), and the `Domain` compiler (C-2).
It was written before those existed and never retrofitted.

**Data integrity: no.** Cycles (C-3) and unguarded deletes (C-4) can corrupt the hierarchy
from ordinary UI actions.

Closing C-1 through C-4 is **~9 hours** and takes the feature from "looks broken, is fragile"
to genuinely solid. That is the highest value-per-hour work available in this area, and I'd do
it regardless of what happens with PartKeepr.

---

## 4. PartKeepr path

### 4.1 Status

| Phase | Scope | State |
|---|---|---|
| **PK1** Category tree | Frontend + counts | ✅ Done, with C-1…C-10 above |
| PK2 Footprints | 2 tables | ❌ Not started |
| PK3 Part parameters + SI units | 3 tables | ❌ Not started |
| PK4 Manufacturer part numbers | 1 table | ❌ Not started |
| PK5 Enhanced supplier info | extends A3b | ❌ Blocked — `product.supplierinfo` itself doesn't exist |
| PK6 Min stock + part status | 2 columns | ❌ Not started; min-stock is only meaningful once `stock.quant` exists |
| PK7 Attachments / datasheets | `ir_attachment` | ❌ Blocked — `ir.attachment` doesn't exist |

### 4.2 Dependency reality

Three of the six remaining phases are **blocked on ERP primitives, not on PartKeepr work**:

```
PK5 ──requires──> product.supplierinfo  (Phase A3b, 3–4 d)
PK7 ──requires──> ir.attachment         (3–4 d, schema already designed in 030 §2.1)
PK6 ──requires──> stock.quant           (1.5–2 w) to mean anything
```

Building them in PartKeepr order means building the blockers anyway, in the wrong sequence and
without the ERP benefiting. Build the primitives first; the PartKeepr phases then become thin
layers on top.

**PK2, PK3 and PK4 are genuinely self-contained** — new tables, no dependency on missing ERP
infrastructure.

### 4.3 If you want one PartKeepr phase before deploying, make it PK3

PK3 (part parameters + SI units) is the actual differentiator for electronics: parametric
search — "resistors between 1 kΩ and 10 kΩ, 0603, ≥1 %" — is the thing a parts catalogue
exists for and that a generic ERP cannot do. It is self-contained, needs no missing primitive,
and `docs/030` already contains the full schema, the SI-prefix normalisation rule and the
range-query design.

PK2 (footprints) and PK4 (MPNs) are simpler but are essentially extra reference tables plus
a many2one — real value, low novelty, easy to add later at any time.

### 4.4 Recommended sequencing

```
Now (pre-deploy)   : C-1 … C-4          ~9 h   fix what's visibly broken
                     C-5 … C-7          ~5 h   rejoin audit / rules / OCC
Post-deploy Stage 4: ir.attachment      3–4 d  (038 #21) ──> unblocks PK7
                     product.supplierinfo 3–4 d (038 #22) ──> unblocks PK5
                     stock.quant        1.5–2 w (038 #17) ──> makes PK6 meaningful
Then               : PK3 (parameters)   ~1.5 w  the differentiator
                     PK7, PK5, PK6      ~1 w    thin layers once primitives exist
                     PK2, PK4           ~1 w    reference tables
```

C-8/C-9/C-10 fold naturally into the PK3 work — a parts catalogue needs `complete_name`,
pagination past 500, and proper many2one labels anyway.

---

## 5. Proposed additions to `038` Stage 1

C-3, C-4 and C-5 belong in the pre-deploy pass, not after it:

| # | Item | Effort | Why pre-deploy |
|---|---|---|---|
| 9a | **C-3** cycle prevention + `walk()` visited guard | 2 h | One UI action bricks the product browser for all users |
| 9b | **C-4** guard unlink on children/products | 2 h | Silent, hard-to-attribute data damage |
| 9c | **S-47** audit calls in all 8 unaudited custom ViewModels, incl. `res.users` / `res.groups` (supersedes C-5) | 1 d | Identity and privilege changes leave no trail; S-37 is recorded as closed but its scope was too narrow |
| 9d | **C-1** recursive `product_count` | 2 h | Cheap, and it is the first thing anyone notices |

C-2, C-6, C-7 and C-8…C-10 can follow deployment.

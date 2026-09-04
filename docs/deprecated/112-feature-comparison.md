# 112 — Feature comparison: c-erp vs the reference ERP vs PartKeepr

**Date:** 2026-08-30
**Sources compared (all read from the local trees, not from memory):**

| System | Where | Size |
|---|---|---|
| **c-erp** | this repo @ `feature/multi-company` | 18 modules, ~35.6k lines C++, 108 registered models, 116 tables |
| **the reference ERP.0** (FINAL) | `zzref2/odoo14/` | 361 addons |
| **PartKeepr** (master) | `zzref3/PartKeepr-master/` | 29 bundles, 32 REST-exposed entities |

**Method.** Model/entity names were extracted from source (`registerCreator(...)` for
c-erp, `_name = ...` for the reference ERP, `Entity/*.php` for PartKeepr) and each candidate gap was
then **verified against the live schema** rather than assumed. Every "absent" in this
document was confirmed by querying `pg_tables`.

---

## 1. The summary

c-erp is **not** a subset of either. It occupies a position neither reference does:

- It has **PartKeepr's electronics parts domain** (footprints, SI-normalised parameters,
  faceted search) which **the reference ERP does not have at all**.
- It has **the reference ERP's ERP backbone** (double-entry accounting, WMS, MRP, purchase/sale
  lifecycle, multi-company) which **PartKeepr does not have at all**.
- It adds an **AI agent layer** (part lookup, BOM header mapping, BOM tidy-up, help
  assistant, all prompt-driven from git-tracked files) that **neither reference has** —
  PartKeepr's OctoPart bundle is a single fixed vendor API, not a model-driven assistant.

The honest gaps are concentrated in three places: **inventory operations** (physical
counts, scrap, routes), **communication** (there is essentially no mail layer), and
**PartKeepr's meta-parts** (a genuinely good idea c-erp has no answer to).

---

## 2. Where c-erp already exceeds the references

### vs PartKeepr — not close, on everything outside the parts domain

PartKeepr is a parts *catalogue*. It has no accounting, no orders, no manufacturing, no
multi-company, no tax, no customers. c-erp has all of it. Within the parts domain itself,
c-erp is still ahead in four specific ways:

| Area | PartKeepr | c-erp |
|---|---|---|
| Stock | `StockEntry` — a flat running level per part | `stock_quant` + `stock_move` + `stock_valuation_layer`: multi-location, multi-warehouse, lots/serials, reservations, putaway rules, real costing posted to the GL |
| Parameter values | `normalizedValue` via `SiPrefix` | `value_base` **plus** `quantity_kind`, so `4k7`/`4700Ω`/`0.0047MΩ` all compare, and a capacitance is never silently compared to a resistance |
| Units | `Unit` + `SiPrefix` | `part_unit` with `quantity_kind`, `factor`, `is_base` — the conversion is data, not a prefix table |
| Reorder | `minStockLevel` scalar on the part | `stock_warehouse_orderpoint` — per-warehouse min/max with a procurement action |
| Part identification | `OctoPartBundle` — one vendor API | AI part lookup: multi-candidate, cited sources, editable proposals, staged review queue, provider-agnostic |
| Import | `ImportBundle` CSV presets | CSV import **plus** AI header mapping across KiCad/Altium/Eagle/JLCPCB and AI row tidy-up, with a row-count identity guard |

### vs the reference ERP — in five specific places

1. **An electronics parts catalogue.** the reference ERP has `product.attribute`, which is a
   *variant* mechanism (Red/Blue/XL), not a parameter with a unit and a magnitude. It
   cannot answer "every 0603 X7R capacitor between 80nF and 120nF rated ≥16V". c-erp's
   `part_parameter` + `part_unit` + `part_footprint` + faceted catalogue can.
2. **BOM revisions.** `mrp_bom` carries `revision` / `revision_of_id`. the reference ERP has no
   native BOM revision control (it arrived later, and even then partially).
3. **Electronics BOM lines.** `mrp_bom_line` carries `reference_designators` and
   `fitted` (DNP). the reference ERP's BOM line has neither — they are the two columns every real
   PCB BOM has.
4. **AI-assisted workflows**, prompt-driven from a git-tracked `prompts/` folder that a
   deployment team can edit and review without rebuilding.
5. **Deployment footprint.** A single native binary against PostgreSQL, versus a Python
   stack with a worker model. Relevant here: production is a 964 MB VM.

---

## 3. Gaps vs PartKeepr (the parts domain)

Verified absent from c-erp:

| # | PartKeepr feature | What it does | Assessment |
|---|---|---|---|
| **P1** | **Meta-parts** (`metaPart`, `MetaPartParameterCriteria`, `metaPartMatches`) | A *virtual* part defined by parameter criteria — "any 100nF ±10% 0603 X7R ≥16V" — that resolves to whichever real parts match. A BOM line can then name the requirement instead of one specific MPN. | **The most valuable gap.** It is exactly the abstraction the BOM importer keeps bumping into: a designer specifies a value+package, not a manufacturer. High value, moderate effort. |
| **P2** | **Parameter min/max** (`minValue`, `maxValue`, `normalizedMinValue`, `normalizedMaxValue`) | A parameter can be a *range* or carry a tolerance, not just a point value. | Prerequisite for meta-part matching, and useful alone (tolerance, temperature range). c-erp stores one `value_numeric` + `value_base`. |
| **P3** | **Part status / condition / needs-review** (`status`, `partCondition`, `needsReview`, `productionRemarks`) | Lifecycle and provenance flags: preferred/obsolete, new/used/pulled, "a human should check this". | Cheap to add and immediately useful — the AI lookup should be able to *set* `needs_review` rather than the reviewer holding it in their head. |
| **P4** | **Distributor SKU URL template** (`skuurl`, `Distributor.enabledForReports`) | Builds a direct link to the vendor's product page from the order number. | Small, high daily convenience. `product_supplierinfo` has no URL at all. |
| **P5** | **BOM overage** (`ProjectPart.overage`, `overageType`) | Extra quantity per line, as % or absolute, for assembly loss/attrition. | Genuinely needed for SMT: you do not buy exactly 1000 of a 1000-placement part. |
| **P6** | **Batch jobs** (`BatchJobBundle`: query + update fields) | Saved bulk-edit operations over a selection. | Useful at catalogue scale; c-erp has no bulk-edit at all. |
| **P7** | **Stock statistics snapshots** (`StatisticBundle`) | Periodic snapshots of stock levels for trend charts. | Low priority — `stock_valuation_layer` already holds the history to derive this. |
| **P8** | **Grid presets** (`FrontendBundle.GridPreset`) | Per-user saved column/filter layouts on list views. | UX polish, no data model risk. |
| **P9** | **Manufacturer IC logos** (`ManufacturerICLogo`) | Logo images to identify a chip from its marking. | Niche but delightful for bench work. |

Not gaps — already covered by a better c-erp equivalent: `StockEntry` (→ stock moves +
valuation), `StorageLocation`/`Category` (→ `stock_location` hierarchy), `Project`/
`ProjectRun` (→ `mrp_bom` + `mrp_production`, per docs/105), `ImportBundle` (→ AI BOM
import), `OctoPartBundle` (→ AI part lookup), `SystemPreference` (→
`ir_config_parameter`).

---

## 4. Gaps vs the reference ERP

the reference ERP ships 361 addons. Most are irrelevant here (110+ `l10n_*` localisations, ~40
`website_*` CMS modules, events, surveys, livechat, e-learning, POS). What follows is
filtered to what a self-hosted electronics manufacturing and rental business would
actually use. **All verified absent.**

### Tier 1 — real operational gaps

| the reference ERP model / addon | What is missing | Why it matters |
|---|---|---|
| `stock.inventory`, `stock.inventory.line` | A **physical stock count** document: draft → counted → applied, with the variance posted. | c-erp has `stock.quant.set_on_hand`, which is a silent overwrite with no document, no variance and no approval. Every real warehouse does periodic counts. **Highest-value stock gap.** |
| `stock.scrap` | Scrapping damaged/lost stock to a scrap location with a valuation entry. | Today the only way to write stock off is to adjust the quant, which loses both the reason and the GL effect. |
| **Mail layer** — `mail.thread`, `mail.activity`, `mail.followers`, `mail.template`, `mail.mail` | Chatter on records, scheduled activities/reminders, followers, email templates, an outgoing queue. c-erp's `mail_message` is a 6-column stub and there is **no SMTP anywhere in the codebase**. | This is the single largest functional gap. It means: no "email this invoice to the customer", no reminders, no audit conversation on a record. Note the portal already *needs* it — an admin-issued password-reset link (docs/111) has to be sent by hand precisely because nothing can send mail. |
| `account.reconcile.model` | Rules that auto-match bank statement lines to invoices. | c-erp has bank reconciliation but every line is matched manually. |
| `product.packaging` | Purchase/sale in packs: reel of 5000, tray of 90, tube of 25. | Very relevant to electronics — components are not bought as loose units. |
| `uom.category` | UoM conversion groups. `uom_uom` has a `category` **text** column but no category table and no cross-unit conversion. | Blocks "buy in reels, consume in pieces" cleanly. |

### Tier 2 — worth having

| the reference ERP model / addon | What is missing |
|---|---|
| `stock.location.route`, `stock.rule`, `procurement.group` | Push/pull routing: MTO, dropship, 2/3-step receipt and delivery. c-erp's flows are hard-wired. |
| `mrp.unbuild` | Disassembling a finished good back into components. |
| `mrp.bom.byproduct` | By-products / co-products from one production run. |
| `purchase.requisition` | RFQ to several vendors, compare, award. |
| `repair.order` | RMA / repair workflow — relevant to an electronics business. |
| `maintenance.equipment`, `maintenance.request` | Machine maintenance scheduling for the shop floor. |
| `auth_totp` | **Two-factor authentication.** Given docs/111 just made the admin the sole account authority, the admin account itself is now the single highest-value target and is password-only. |
| `product.removal` | Removal strategy (FIFO/LIFO/FEFO) when picking. |
| `stock.picking.batch` | Batch/wave picking. |
| `account.tax.group`, `account.tax.repartition.line` | Multi-part tax splits. c-erp's tax engine is simpler (adequate for SST, not for VAT regimes). |
| `account.payment.method` | Payment methods beyond the built-ins. |
| `base_automation` | Rule-driven automated actions on record changes. |

### Tier 3 — deliberately not recommended

`crm`, `point_of_sale`, `website_*`, `mass_mailing`, `event`, `survey`, `hr_holidays`/
`hr_recruitment`/`hr_skills`, `fleet`, `lunch`, `gamification`, all `l10n_*` except any
Malaysian requirement, all `iap_*` cloud services. These are either irrelevant to the
business or a large surface for little return.

---

## 5. Recommended order

Sequenced by value-per-effort, not by section order above.

**Now — closes the most painful gaps**

1. **`stock.inventory`** — the count document. Biggest operational gap; self-contained;
   c-erp already has quants, valuation layers and a document/state pattern to copy.
2. **Parameter min/max (P2) → meta-parts (P1)** — do these together, in that order. This
   is where c-erp can decisively pass PartKeepr rather than match it, and it feeds
   directly into the BOM importer that already exists.
3. **Part status / condition / needs-review (P3)** — a migration and some UI. Wire
   `needs_review` to the AI lookup so a low-confidence proposal flags itself.

**Next — unblocks whole categories**

4. **A minimal mail layer**: an SMTP sender + `mail.template` + "email this document".
   Not the full the reference ERP chatter — just outbound. It immediately improves the portal, the
   reset-link flow (docs/111) and invoicing.
5. **`product.packaging` + `uom.category`** — buy in reels, consume in pieces.
6. **`stock.scrap`** — small, and stops write-offs being invisible.
7. **`auth_totp`** — 2FA for admin accounts.

**Later**

8. BOM overage (P5), distributor SKU URLs (P4), bulk edit (P6), grid presets (P8).
9. Routes/rules, requisition, repair, maintenance, unbuild, by-products.
10. Reconciliation models, tax groups.

---

## 6. Scoreboard

| Domain | PartKeepr | the reference ERP | c-erp |
|---|---|---|---|
| Electronics parts catalogue | ●●● | ○ | ●●● |
| Parameter search / normalisation | ●●● | ○ | ●●●+ (quantity-kind aware) |
| Meta-parts | ●●● | ○ | ○ **gap** |
| Stock / WMS | ● | ●●● | ●●● (no counts, no scrap) |
| Accounting | ○ | ●●● | ●●● (no reconcile models) |
| Manufacturing | ○ | ●●● | ●● (no unbuild/by-products) |
| Purchase / Sale | ○ | ●●● | ●●● (no requisition) |
| BOM management | ● | ●● | ●●●+ (revisions, designators, DNP) |
| Mail / activities | ○ | ●●● | ○ **gap** |
| Multi-company | ○ | ●●● | ●●● |
| Customer portal | ○ | ●●● | ●●● |
| Rental | ○ | ○ | ●●● |
| AI assistance | ● (OctoPart) | ○ | ●●●+ |
| Test coverage | ● | ●● | ●●● (97 suites) |

`○` absent · `●` basic · `●●` partial · `●●●` complete · `+` exceeds both references

---

## 7. Caveats

- Model presence is not feature depth. c-erp having `account.move` does not mean it
  matches the reference ERP's accounting line-for-line; it means the concept exists and is tested.
  Where I judged depth rather than presence, I have said so.
- the reference ERP is the comparison because it is the tree in `zzref2/`. the reference ERP 15–17 added BOM
  revisions and other items listed above as c-erp advantages; against a current the reference ERP some
  of those advantages narrow.
- PartKeepr is effectively unmaintained upstream. It is compared here as a *domain model
  reference* — its ideas, especially meta-parts, are worth taking — not as a live competitor.

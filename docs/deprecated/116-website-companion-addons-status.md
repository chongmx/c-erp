# 116 — the reference ERP website + CRM companion addons: full status and plan

**Date:** 2026-08-30
**Source:** `zzref2/odoo14/addons`, read directly. Every summary below is the
addon's own `__manifest__.py` `summary`, not a paraphrase.

---

## 0. Correcting the claim this document was asked to check

Two things to fix before the list is useful:

1. **The "40+ companion features" figure was mine, and it was about `website`,
   not CRM.** It appears in docs/112 ("~40 `website_*` CMS modules") and
   docs/115 ("~40 companion addons"). CRM is a different, much smaller family.
2. **I undercounted.** The real numbers, counted from the tree:

| Family | Addons | Note |
|---|---|---|
| `website*` | **51** | `website` itself + **50 companions** |
| `crm*` | **6** | `crm` itself + 5 companions |
| Addons that declare a dependency on `website` | **109** | the true blast radius of "install the CMS" |

So: 50 website companions, not 40, and CRM contributes 5 of its own.

---

## 1. Status key

| Status | Means |
|---|---|
| **BUILT** | c-erp has an equivalent, shipped and tested |
| **PARTIAL** | some of it exists; the gap is named |
| **READY** | small, useful, no prerequisite — can be built now |
| **BLOCKED** | worth having, but needs a prerequisite c-erp lacks |
| **DECLINED** | deliberately not planned — reason given |

`DECLINED` is a recommendation, not a refusal. Any line can be moved to READY
on request; §5 says what each would cost.

---

## 2. The 50 `website` companions

### Core / infrastructure

| # | Addon | the reference ERP's summary | Status | Notes |
|---|---|---|---|---|
| 1 | `website` | Enterprise website builder | **BUILT** | docs/115 — pages, menu, blocks, publishing, SEO, robots, sitemap. **docs/117 adds in-place editing** (the reference ERP's `web_editor`, adapted): edit the live page, gated server-side on the Settings/Configuration group. Drag-and-drop and image upload are still out. |
| 2 | `website_mail` | Website Module for Mail | **BLOCKED** | needs the mail layer (docs/112 §Tier 1) |
| 3 | `website_partner` | Partner module for website | **PARTIAL** | `res.partner` exists; no public partner page |
| 4 | `website_payment` | Payment integration with website | **BLOCKED** | no payment acquirers in c-erp |
| 5 | `website_profile` | Access the website profile of the users | **DECLINED** | public user profiles — no use case here |
| 6 | `website_links` | Generate trackable & short URLs | **DECLINED** | marketing attribution; no campaign tooling to feed |
| 7 | `website_form` | Build custom web forms | **BUILT** | A1 — forms, typed fields, public `POST /site/form/<slug>`, submissions. Field allow-list, honeypot, rate limit, length caps. 51 integration checks. |
| 8 | `website_google_map` | Show your company address on Google Maps | **BUILT** | A3 — a `map` block. The author gives a place name; the SERVER builds a sandboxed frame against a fixed provider. No paste-an-embed field, which would be a hole through every other block's protection |
| 9 | `website_customer` | Publish your customer references | **BUILT** | A3 — a `references` block: name, note, optional logo, all escaped |
| 10 | `website_blog` | Publish blog posts, announces, news | **BUILT** | A4 — a post is a page with `page_kind='post'` (+ date, author, excerpt). `/site/blog` index, drafts invisible, future-dated posts held back from index and sitemap |
| 11 | `website_twitter` | Twitter scroller snippet in website | **DECLINED** | third-party embed, dead API tier |

### eCommerce — `website_sale` and friends (13)

| # | Addon | the reference ERP's summary | Status | Notes |
|---|---|---|---|---|
| 12 | `website_sale` | Sell your products online | **BLOCKED** | needs payment acquirers + a cart. The big one. |
| 13 | `website_sale_management` | Website - Sales Management | **BLOCKED** | bridge, follows 12 |
| 14 | `website_sale_stock` | Manage product inventory & availability | **BLOCKED** | follows 12; c-erp *has* the stock data |
| 15 | `website_sale_delivery` | Add delivery costs to online sales | **BLOCKED** | needs carriers (`delivery`, absent) |
| 16 | `website_sale_comparison` | Compare products based on their attributes | **BLOCKED** | follows 12. c-erp's part parameters would make this *better* than the reference ERP's |
| 17 | `website_sale_wishlist` | Allow shoppers to enlist products | **BLOCKED** | follows 12 |
| 18 | `website_sale_coupon` | Coupon & promotion programs | **DECLINED** | no coupon engine; not a promotions business |
| 19 | `website_sale_coupon_delivery` | Free shipping in coupon reward | **DECLINED** | follows 18 |
| 20 | `website_sale_digital` | Sell digital products | **DECLINED** | nothing digital to sell |
| 21 | `website_sale_slides` | Sell your courses online | **DECLINED** | no e-learning |
| 22 | `website_sale_product_configurator` | bridge | **DECLINED** | follows 12 + a configurator c-erp lacks |
| 23 | `website_sale_stock_product_configurator` | bridge | **DECLINED** | as above |
| 24 | `website_membership` | Publish your members directory | **DECLINED** | not a membership organisation |

### CRM-facing (5)

| # | Addon | the reference ERP's summary | Status | Notes |
|---|---|---|---|---|
| 25 | `website_crm` | Generate leads from a contact form | **BLOCKED** | needs a CRM model; pairs with #7 |
| 26 | `website_crm_partner_assign` | Publish resellers, forward leads | **DECLINED** | no reseller channel |
| 27 | `website_crm_livechat` | View livechat sessions for leads | **DECLINED** | follows livechat |
| 28 | `website_crm_sms` | SMS to website visitors with a lead | **DECLINED** | needs an SMS gateway |
| 29 | `website_form_project` | Task suggestion form on your website | **BUILT** | A2 — a form's `target_model` routes its submission to a `project.task`, from an allow-list of exactly one model |

### Events (9)

| # | Addon | Status |
|---|---|---|
| 30–38 | `website_event`, `_crm`, `_crm_questions`, `_meet`, `_meet_quiz`, `_questions`, `_sale`, `_track`, `_track_exhibitor`, `_track_live`, `_track_live_quiz`, `_track_quiz` | **DECLINED** — this business does not run ticketed events. Nine addons, a whole `event` module underneath, zero use. |

### eLearning + community (5)

| # | Addon | Status |
|---|---|---|
| 39 | `website_slides` (eLearning platform) | **DECLINED** |
| 40 | `website_slides_forum` | **DECLINED** |
| 41 | `website_slides_survey` (certification) | **DECLINED** |
| 42 | `website_forum` (FAQ / Q&A) | **DECLINED** — the Help Centre (docs/101) already answers this need internally |
| 43 | `website_mail_channel` (visitors join mail channels) | **DECLINED** |

### Messaging / marketing (6)

| # | Addon | the reference ERP's summary | Status | Notes |
|---|---|---|---|---|
| 44 | `website_livechat` | Chat with your website visitors | **DECLINED** | needs a live-chat stack and staffing |
| 45 | `website_jitsi` | Create Jitsi room on website | **DECLINED** | video rooms |
| 46 | `website_mass_mailing` | Attract visitors to subscribe to mailing lists | **BLOCKED** | needs the mail layer |
| 47 | `website_sms` | Send SMS to website visitors | **DECLINED** | no SMS gateway |
| 48 | `website_hr_recruitment` | Manage your online hiring process | **DECLINED** | small team; no recruitment pipeline |

### The `crm*` family (6)

| Addon | the reference ERP's summary | Status |
|---|---|---|
| `crm` | Track leads and close opportunities | **BLOCKED** → then READY. The prerequisite for #25. |
| `crm_livechat` | Create lead from livechat conversation | **DECLINED** |
| `crm_sms` | Add SMS capabilities to CRM | **DECLINED** |
| `crm_iap_lead` | Buy lead lists by country/industry | **DECLINED** — a paid the reference ERP cloud service |
| `crm_iap_lead_enrich` | Enrich leads from email domain | **DECLINED** — same |
| `crm_iap_lead_website` | Leads from website traffic (IAP) | **DECLINED** — same |

---

## 3. The tally

| Status | Count |
|---|---|
| BUILT | **6** — `website`, `website_form`, `website_form_project`, `website_customer`, `website_google_map`, `website_blog` |
| READY — build now | 0 |
| PARTIAL | 1 — `website_partner` |
| BLOCKED — behind a prerequisite | 11 |
| DECLINED — recommended against | 39 |
| **Total tracked** | **57** — `website` + 50 companions, `crm` + 5 companions |

## 3a. Phase A — COMPLETE (2026-08-30)

All four steps landed. **Nothing is left in READY**: everything still open is
either behind a prerequisite (§4 Phase B) or declined.

| Step | Item | Tests |
|---|---|---|
| A1 | Form builder | `tests/integration/website/forms/` — 51 checks |
| A2 | Forms → tasks | same suite, §5 |
| A3 | Reference + map blocks | `tests/integration/website/cms/` §7b |
| A4 | Blog | same suite, §7c |

Plus `tests/unit/website/test_render.cpp` — **92 assertions** on the sanitiser,
slug validator and block renderer, in the millisecond tier where the whole
catalogue of XSS vectors is affordable.

**Suite: 103 suites / 0 failed. Unit tier: 212 assertions.** Both new
integration suites are hermetic.

Two defects the tests caught, both the same shape — a check that lived only in
the model, which `BaseModel::write()` walks past:

* `page_kind` was writable to any value through the generic write path. Fixed
  with a `CHECK` constraint, so the database refuses it on every path.
* (Earlier, same class: `hr_attendance.worked_hours` — docs/113.)

**39 of 57 are recommended against**, and that is the finding, not an evasion:
the reference ERP website family is mostly *other businesses' shopfronts* — ticketed
events, e-learning, forums, memberships, reseller channels, SMS campaigns. Each
one built here would be a public, unauthenticated surface maintained for nobody.

If you want any DECLINED line built anyway, say which — §5 prices them.

---

## 4. Implementation plan

### Phase A — the four READY items (no prerequisite)

| Step | Item | What gets built |
|---|---|---|
| A1 | `website_form` (#7) | A **form builder**: named forms with typed fields, a public `POST /site/form/<slug>`, submissions stored and listed in the back office. Server-side field allow-list per form, honeypot + rate limit, no email needed. |
| A2 | `website_form_project` (#29) | Route a form's submissions to `project.task` — c-erp already has tasks and stages. |
| A3 | `website_customer` (#9) + `website_google_map` (#8) | Two new block types: a customer-reference list and a map embed (allow-listed provider + coordinates, never raw iframe HTML). |
| A4 | `website_blog` (#10) | Posts as a page subtype: date, author, excerpt, `/site/blog`, index + post pages, in the sitemap. |

### Phase B — unblock, then build

Each of these is a prerequisite that unlocks several companions at once. In
value order:

| Step | Prerequisite | Unlocks |
|---|---|---|
| B1 | **Mail layer** (docs/112 Tier 1 — SMTP + templates) | #2, #46, and form-submission notifications for A1; also fixes the hand-delivered reset link (docs/111) and share links (docs/114) |
| B2 | **CRM** (`crm.lead`, stages, pipeline) | #25 — the contact form becomes a lead |
| B3 | **Payment acquirers + cart** | #12–#17, the eCommerce block. The largest single piece of work in this document. |

### Phase C — eCommerce, if B3 is funded

#12 → #13 → #14 → #16 → #17, in that order. Note #16
(`website_sale_comparison`) is the one place c-erp would **beat** the reference ERP out of the
box: its part parameters are a real parametric search, which the reference ERP's
`product.attribute` is not (docs/112 §2).

---

## 5. Cost of the DECLINED lines, if you want them

Rough size, so the decision is yours rather than mine:

| Group | Items | Size |
|---|---|---|
| Events | 9 | large — needs an `event` module first |
| eLearning + forum | 5 | large — content, enrolment, quizzes |
| SMS | 3 | small each, but all need an SMS gateway account |
| Livechat + Jitsi | 3 | large — a realtime stack |
| Coupons / digital / membership / recruitment / reseller / profiles / twitter / links | 12 | small to medium each |
| IAP lead services | 3 | **not buildable** — they are paid the reference ERP cloud endpoints |

---

## 6. Tracking

This document is the tracker. Update the Status column as phases land; the
tally in §3 should always match the tables above.

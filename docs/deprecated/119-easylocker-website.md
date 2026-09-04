# 119 — The Easy Locker Space website

**Built with the CMS, not beside it.** Every page is an ordinary
`website_page` row of content blocks, so all of it is editable in the browser
by anyone with the Settings / Configuration group (docs/117). Nothing about
this site is special-cased in the renderer.

Rebuild or reset it with:

```bash
python3 scripts/seed_easylocker_site.py     # idempotent; matches pages on slug
```

---

## What is there

| Page | Slug | Holds |
|---|---|---|
| Home | `home` | hero, the two unit types, why-us columns, three steps, closing call to action |
| Sizes and prices | `units` | both units in detail, what is and is not included, terms |
| How it works | `how-it-works` | five steps, and how to leave |
| Questions | `faq` | nine questions, expand/collapse |
| Contact | `contact` | the enquiry form and a map |

Menu: Sizes & prices · How it works · Questions · Contact.
Site name, accent (`#e94560`, matching the ERP) and footer are
`ir_config_parameter` rows, editable in Settings.

## The enquiry form

`storage-enquiry`, with name, phone, email, which size, when, and what they are
storing. It is routed to **`project.task`**, so an enquiry becomes a task on
the board rather than an email nobody owns — which matters because the system
still cannot send email (docs/112).

Because it is a public form it carries the usual protections (docs/116 A1): a
field allow-list, a honeypot, per-IP rate limiting and length caps.

## New block types

Four were added for this, all typed data rendered by the server, so the rule
from docs/115 still holds — an author supplies text, never markup:

| Block | For |
|---|---|
| `hero` | headline, sub-headline, up to two calls to action |
| `pricing` | unit types: name, size, price, period, feature list, badge, CTA |
| `steps` | numbered steps — **the number is generated**, never taken from the data |
| `faq` | question/answer pairs, using `<details>` so it works with JavaScript off |

## Two bugs the browser screenshots caught

Neither would have failed an HTTP test.

1. **The form rendered after the last block**, not where its block sat — so the
   contact page put the form below the map. `WebsiteRender::blocks()` now takes
   a `FormResolver` callback: the renderer stays free of a database of its own,
   and the form lands in its own position.
2. **The map was a blank grey rectangle.** An OpenStreetMap embed needs a
   bounding box, and a place name alone gives none. Now coordinates produce a
   real embed with a marker, and a bare place name produces an honest **link**
   instead of a broken frame — because an empty frame is worse than no map.

## Before this goes live

- **The prices are placeholders.** RM 190 and RM 310 are plausible, not
  researched. Set the real ones.
- **The map coordinates are central Kuala Lumpur**, not the yard. Replace
  `lat` / `lon` on the contact page's map block.
- The copy claims 7-day access, CCTV, individual alarms and no deposit. Those
  are commitments — check each one is true of your site before publishing.
- Point the domain at `/site` in nginx (docs/115 §2a).

## Verified

- All five pages render 200, no console errors, no horizontal overflow.
- The homepage's **12 blocks are all editable in place** — hero, pricing,
  steps and columns included — edited, saved, confirmed on the public page and
  restored, through real Chrome.
- The site's own health check (docs/118 E3) reports **zero issues**.

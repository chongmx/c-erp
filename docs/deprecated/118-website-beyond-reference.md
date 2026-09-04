# 118 — Website features that go past the reference ERP

**Status:** plan; phases marked off as they land.

Everything here is checked against `zzref2/odoo14/addons`. "the reference ERP does not
have this" means I looked, and the grep is quoted.

---

## The four, and why each is genuinely ahead

### E1 — Page revision history ✅ BUILT

**the reference ERP keeps exactly ONE previous version.** `ir.ui.view.arch_prev` is a
single `Text` field overwritten on every write, with `reset_arch()` to step
back once (`base/models/ir_ui_view.py:229, 317, 498`). There is no list of
versions, no record of who changed a page, and no way back past one step.

Ours: **every save snapshots the previous content**, with the user and the
timestamp, kept as a list you can browse and restore any entry from.

This is not gold-plating — it closes a hole I opened. The in-place editor
(docs/117) replaces a page's content on Save, and the help had to say "no
revision history — a save replaces what was there". Giving somebody a
click-to-edit button on a live public page without an undo is the wrong trade.

**Bounded on purpose:** the last 20 versions per page, pruned on write. A CMS
history that grows without limit is a table nobody vacuums.

### E2 — JSON-LD structured data ✅ BUILT

`grep -rl "application/ld+json" website/` in the reference ERP returns **nothing**. It
emits OpenGraph and Twitter cards (docs/115 matched that) but no schema.org
JSON-LD, which is what actually drives rich results.

Ours emits, per page:

* **Organization** — site name, URL, derived from company settings;
* **WebSite** — with the site's own name;
* **Article** for a blog post — headline, `datePublished`, author;
* **BreadcrumbList** — home → section → page.

Built as a **JSON object serialised by nlohmann**, never string-concatenated.

**Serialising is necessary but not sufficient, and I got this wrong first.**
nlohmann escapes quotes and backslashes but leaves `/` alone — which is valid
JSON and fatal here: a page titled `</script><script>alert(1)</script>` closed
the block early and everything after it parsed as markup. The test caught it.
The fix is to rewrite `</` as `<\/` after dumping — `\/` is a legal JSON escape
for `/`, so the document stays valid while the sequence becomes inert.

Worth recording because it is counter-intuitive: *using a real JSON library is
not enough on its own* when the JSON is embedded in HTML.

### E3 — Site health check ✅ BUILT

the reference ERP has no built-in site audit. A single screen answering "what is wrong with
my website right now":

* pages with no meta description, or a title over 60 characters;
* published pages reachable from **no menu at all**;
* menu entries pointing at a **draft or deleted** page;
* image blocks with **no alt text** (accessibility, and it is also what a
  screen reader and an image search read);
* internal links in button blocks pointing at a slug that does not exist;
* forms with no fields; posts with no excerpt.

Cheap to build on data already present, and the kind of thing that quietly
decays without a tool to name it.

### E4 — Public parametric parts catalogue ⬜ NOT BUILT

**The biggest one, and the one the reference ERP cannot do at all.**

docs/112 established that the reference ERP's `product.attribute` is a *variant* mechanism
(Red/Blue/XL), not a parameter with a unit and a magnitude — it cannot answer
*"every 0603 X7R capacitor between 80nF and 120nF rated ≥16V"*. c-erp's
`part_parameter` + `part_unit` + `value_base` can, and already does inside the
back office.

Exposing that as a **public, faceted catalogue** under `/site/parts` would put
a genuinely better parametric search on the open web than the reference ERP's eCommerce
offers — **and it needs no payment acquirer, no cart, and no checkout**, which
is what blocks `website_sale` (docs/116 B3).

Deliberately read-only: browse, filter, view a part, and a "request a quote"
button that reuses the existing form. No prices, no stock levels — those are
commercial facts a competitor should not be able to scrape, and publishing them
is a business decision, not a technical default.

---

## Order and state

E1 → E2 → E3 → E4.

| | State | Tests |
|---|---|---|
| E1 revision history | **BUILT** | `tests/integration/website/editor` §8 |
| E2 JSON-LD | **BUILT** | same suite §9 |
| E3 site health check | **BUILT** | `tests/integration/website/cms` §7d — `GET /site/api/health` |
| E4 public parts catalogue | not built | — |

Editor **95 checks**; CMS **105**; forms **51**; unit tier **212 assertions**.
Full suite **104 suites / 0 failed**, all three website suites hermetic.

### The editor is now driven through real Chrome

`tests/integration/website/editor/drive.mjs`. Everything else in that suite
talks to the HTTP API, which proves the server refuses the right callers and
says **nothing** about whether the editor works — `website-editor.js` could
throw on its first line and every one of those assertions would still pass.

It covers the two things that exist only in a browser:

* **contenteditable.** Typing a `<script>` tag stores it as text and re-serves
  it escaped; markup injected as real DOM (what a rich paste produces) is
  **flattened to text** by `textContent`, so `<b>bold</b><i>italic</i>` is
  stored as `bolditalic`. No HTTP test can reach that path, because the path
  is the DOM.
* **absence.** The toolbar is genuinely not in a visitor's DOM — a curl grep
  cannot tell "absent" from "present but hidden".

Also asserted: entering edit mode, add/delete a block, save round-trip,
persistence, and **zero console or page errors** throughout.

Two test bugs it exposed, both mine:

* two puppeteer `dialog` listeners both answered the same dialog, which throws
  and killed the driver before it printed anything — one handler with a mode
  flag instead;
* the browser check was handed a login that §4 had already **granted** the
  editing group, so "staff without the group sees no toolbar" was asserting the
  opposite of what it said. It needed its own user.

### API

```
GET  /site/api/page/{id}/revisions        list (id, author, at, size, note)
POST /site/api/page/{id}/revisions/{rid}  restore that version
```

Both behind the editor group. Restoring snapshots the current content first,
so undoing an undo works; a revision id from another page cannot be restored
onto this one.

## Security notes

- Revisions store **content, not markup**: the same block JSON, restored
  through the same validation as a save. Restoring cannot introduce anything a
  save could not.
- Viewing or restoring a revision is behind the **same group** as editing
  (docs/117), checked server-side. Revisions include unpublished drafts.
- JSON-LD is machine-serialised, never concatenated.
- The health check reads only; it is behind the editor group because its
  output names unpublished pages.

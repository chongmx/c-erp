# 115 — A CMS, adapted from the reference ERP's `website`

**Status:** plan. Phases executed and marked off in order.

Read against `zzref2/odoo14/addons/website/` — 54 Python files, 86 XML files,
20 models. This document says which parts to take, which to leave, and the two
places where copying the reference ERP would be actively wrong for c-erp.

---

## 1. What the reference ERP's `website` gives you

| the reference ERP model | Does |
|---|---|
| `website.page` | url, view/arch, `is_published`, `website_indexed`, `date_publish`, homepage flag, header/footer visibility |
| `website.menu` | a tree: name, url, `page_id`, `parent_id`, sequence, `new_window` |
| `website.seo.metadata` mixin | `website_meta_title` / `_description` / `_keywords`, OpenGraph + Twitter cards |
| `website.published.mixin` | `is_published` + `can_publish` on anything publishable |
| `website.rewrite` | URL redirects (301/302) |
| `website.visitor`, `website.track` | analytics |
| `theme.*` | installable themes |
| snippets | the drag-and-drop block editor |

Public routes: `/` (homepage), arbitrary page paths, `/robots.txt`,
`/sitemap.xml`.

## 2. The two places copying the reference ERP would be wrong here

### 2a. The root URL is already taken

the reference ERP serves the **website** at `/` and its back office at `/web`. c-erp serves
the **ERP application** at `/`. Taking `/` for the CMS would move every user's
bookmark and every existing route's assumptions.

**Decision:** pages are served under **`/site/...`**, with `/site` as the
homepage. Pointing a real domain at the CMS is then an nginx rewrite
(`location / { proxy_pass .../site/; }`), which this deployment already has in
front of it (Cloudflare → nginx → c-erp, docs/044). No existing URL moves.

### 2b. Free-form HTML is the whole attack surface

the reference ERP stores author HTML (`view.arch`) and renders it. It gets away with that
because authoring is restricted to trusted staff *and* it sanitises `fields.Html`
against an allowlist.

A CMS is, by definition, **content written by one person and executed in
another person's browser**. Stored XSS is not a corner case here; it is the
default failure. And a CMS page is public, so the victim is anyone.

**Decision: content is BLOCKS, not markup.** A page is a JSON array of typed
blocks — heading, text, image, columns, button, divider, embed — and **the
server renders the HTML from the data.** The author supplies *text*, never
tags, so for the ordinary blocks there is no injection surface at all: every
value goes out HTML-escaped, by construction, not by remembering to escape.

The one block that must accept markup (`html`, for a payment badge or a map)
goes through an **allowlist sanitiser** — elements, attributes and URL schemes
— and is available to administrators only. That is the same trade the reference ERP makes,
narrowed to one block instead of every field.

This is the "adapt, don't copy" instruction applied to the highest-risk part.

## 3. Scope

**In:**
- `website.page` — slug, title, blocks, published, indexed, SEO meta, homepage
- `website.menu` — tree with sequence, internal page or external URL
- Public serving at `/site/*`, with draft pages invisible
- `/robots.txt`, `/sitemap.xml` (published + indexed pages only)
- Theme settings: accent colour, font, site name, footer text
- Admin CRUD through the ordinary model layer

**Out, deliberately:**
- The drag-and-drop snippet editor (the reference ERP's is a large fraction of the whole
  addon). Blocks are edited as a form; a visual editor can come later on top of
  the same data.
- Themes as installable packages, `website.visitor` analytics, multi-website,
  multi-language URL routing, `website_sale`.

## 4. Phases

### C1 — Data + rendering + security core ✅
`website_page`, `website_menu`, `website_theme` (single row of settings).
`WebsiteRender` turns blocks into HTML with everything escaped; `WebsiteSanitize`
handles the one raw-HTML block.

### C2 — Public serving ✅
`GET /site`, `GET /site/<slug>`, 404 page, menu rendered from the tree,
draft pages 404 to the public but preview-able by a logged-in staff user.

### C3 — SEO ✅
`<title>`, meta description/keywords, OpenGraph + Twitter tags, canonical URL,
`/robots.txt`, `/sitemap.xml`.

### C4 — Admin ✅
Models registered so the generic form/list UI can edit pages and menus, plus
menu entries under Settings.

## 5. Security rules for this module

1. **No author string reaches the page unescaped** except through the `html`
   block, which is sanitised against an allowlist and is admin-only.
2. **The sanitiser is an allowlist, never a blocklist.** Unknown element →
   dropped. Unknown attribute → dropped. `on*` → dropped always. `javascript:`,
   `data:` (except images), `vbscript:` → dropped.
3. **A slug is `[a-z0-9-/]` only**, length-capped, no `..`, no leading `/`. It
   selects a database row, never a file.
4. **Draft is invisible.** An unpublished page is 404 to the public — the same
   answer as a page that does not exist, so the URL space cannot be probed.
5. **Public pages carry no session and set no cookie.**
6. **Editing requires the Settings/Configuration group**, checked server-side.

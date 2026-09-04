# 117 — In-place website editing, adapted from the reference ERP

**Status:** plan, then built. Read against `zzref2/odoo14/addons/web_editor/`
and `website/models/ir_http.py`.

---

## 1. How the reference ERP does it

Three facts from the source, and each one decides something for us.

| the reference ERP | Where |
|---|---|
| **Edit mode is decided on the SERVER.** `values['editable'] = request.uid and request.website.is_publisher()` — the editor is only rendered into the page when the logged-in user is in the publisher group. | `website/models/ir_http.py:369` |
| **Two groups.** `group_website_publisher` edits content; `group_website_designer` implies it and can also edit views and assets. | `website/security/*.xml` |
| **Every editor endpoint is `auth='user'`.** All 14 of them — attachments, assets, image ops, save. None is `auth='public'` except one template renderer. | `web_editor/controllers/main.py` |
| **The save round-trips HTML.** The editor posts markup back and the server maps `data-oe-model` / `data-oe-field` / `data-oe-id` attributes onto model fields. | `web_editor/models/ir_ui_view.py` |

## 2. The one place we must not copy

**the reference ERP saves HTML. We must not.**

docs/115 established that our page content is *typed blocks* rendered by the
server, so the ordinary blocks have no XSS surface at all — the author supplies
text and never markup. A WYSIWYG that round-trips HTML would hand that back:
whatever the editor posted would become the page, and the sanitiser would go
from "the only way markup enters" to "one of two ways markup enters".

So this is an **in-place block editor**, not an HTML editor:

- the page renders normally; edit mode adds an overlay on the real page, so it
  is still *what you see is what you get*;
- text is edited in `contenteditable` elements, but the editor reads
  **`textContent`, never `innerHTML`** — the DOM is the input surface, plain
  strings are the output;
- add / delete / reorder blocks from a palette;
- Save posts the **block array as JSON**, through the same validation the
  admin form uses.

Nothing new can reach the page that could not already reach it through the
back-office form. That is the property worth protecting.

## 3. Authorisation

the reference ERP's shape, with our groups.

| Who | Sees the editor | May save |
|---|---|---|
| Anonymous visitor | no | no |
| Portal customer (a partner password, not a staff login) | no | no |
| Staff **without** `SETTINGS_CONFIGURATION` | no | no |
| Staff **with** `SETTINGS_CONFIGURATION` | yes | yes |
| Administrator | yes | yes — **and only they may use the `html` block** |

Three rules make that real:

1. **The server decides.** `servePage` resolves the staff session and its
   groups, and only then emits the editor bar and its script. A visitor's HTML
   does not contain the editor at all.
2. **The endpoint re-checks.** `POST /site/api/page/<id>/blocks` verifies the
   session and the group itself. The UI gate is a convenience; *this* is the
   control. Hiding a button has never stopped anybody.
3. **The raw-HTML block is admin-only, and refused loudly.** A publisher who
   posts an `html` block gets an error naming it, not a silent drop — a
   silently-discarded block looks like a bug and invites a retry.

### CSRF

Same reasoning as docs/114 §W2, and it still holds: the staff cookie is
`SameSite=Lax` + `HttpOnly`, so a cross-site POST arrives without it and the
endpoint answers 401 before doing anything. No token machinery.

## 4. Validation on save

The endpoint is the only new write path, so it repeats every check the model
does rather than trusting the caller:

- body must parse, `blocks` must be an **array**;
- **at most 200 blocks**, **at most 256 KB** of JSON — a page is content, not a
  payload;
- every block must have a **known `type`**; an unknown one is refused rather
  than stored (stored, it would render as nothing and look like data loss);
- `html` blocks require admin;
- the stored JSON is re-read and re-rendered by the same `WebsiteRender`, so
  escaping is unchanged.

## 5. What this does not do

No image upload (that is an attachment surface of its own), no drag-and-drop
reordering (buttons instead — same result, far less code), no per-user drafts
or revision history, no editing of the menu or the theme from the page. Those
are separate pieces, and none of them changes the security model above.

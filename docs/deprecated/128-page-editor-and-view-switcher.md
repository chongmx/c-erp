# 128 — A page editor, and views that only appear when they work

---

## 1. Website Pages was a wall of JSON

Settings → Website → Website Pages rendered `blocks_json` through the generic
form, which meant a textarea containing one very long line. The only way to see
what a page looked like was to save it and go and look at the site.

It is now a custom view with the record shown two ways:

| tab | what it is |
|---|---|
| **Preview** | the blocks **rendered by the server**, in the site's own stylesheet |
| **Source** | the JSON, pretty-printed and editable |

Plus a page list down the side (slug, `home` and `draft` badges), the block
count, **Open ↗** to the live page, **Refresh preview**, and **Save**.

### The preview is of what you are editing

`POST /site/api/preview` takes the blocks currently in the editor — **unsaved** —
and returns a whole rendered document. Previewing the *saved* state would answer
a different question than the one being asked while typing.

It returns a complete page, not a fragment, because a preview without the
site's own stylesheet is a preview of something else. That document goes into
an iframe with `sandbox=""` — no scripts — so even an administrator previewing
a raw-HTML block cannot execute anything inside the ERP's own page.

### It reuses the real paths

* The same gate as saving: staff **and** the Settings / Configuration group.
* The same block-type allowlist, and the same admin-only rule on `html`.
* Saving goes through `POST /site/api/page/<id>/blocks` — the endpoint that
  checks the group, refuses unknown block types and writes a revision. A direct
  model write would have bypassed all three.

The allowlist was two copies at that point — one in the save handler, one about
to be written into the preview handler. It is now `kKnownBlockTypes()`, one
list, because the way two copies drift is that one starts accepting something
the other refuses, and a preview that renders a block the save path rejects is
a promise the product cannot keep.

A JSON syntax error is reported inline as content, not as a fault — it is the
common case when somebody is editing JSON by hand, and `500 An internal error
occurred` would be a lie about whose mistake it was.

---

## 2. The view switcher offered views that could not work

Every list carried the same six buttons — List, Grouped, Kanban, Pivot, Graph,
Calendar — regardless of the model. Three of them need a particular kind of
field:

* **Pivot** and **Graph** need something to *measure*: an integer, float or
  monetary field. With none, there is nothing to put in the cells.
* **Calendar** needs a date or datetime to place a record on.

On `website.page`, `res.groups`, `ir.report.template` and every other model made
of text, those buttons were present, clickable, and led to an empty screen. A
control that cannot do anything reads as **broken**, not as inapplicable.

`altViews` now derives from the list view's own fields: List, Grouped and Kanban
always, Pivot and Graph only when a measurable field exists, Calendar only when
a date exists.

`effectiveAltView` guards the render: if the selected view is no longer on offer
— the model changed underneath, or the field that justified it is gone — it
falls back to the list instead of rendering a view nothing can populate.

**Export CSV / Import CSV and New were left alone.** They work on any model and
were not the problem.

---

## 3. Files

`modules/website/WebsiteModule.cpp` — the preview endpoint and the shared
allowlist. `web/static/src/components/WebsitePages.js` — the screen.
`web/static/index.html` and `app.js` — registration, plus the `altViews` and
`effectiveAltView` change. `app.css` — the styles.

# 122 — The editor sidebar

---

## 1. The limitation this fixes

In-place editing has a ceiling that is easy to miss because everything visible
works: **you can only edit what is rendered.**

A heading's *level* is a tag name. A button's *link* is an attribute. An
image's *alt text* is for people who cannot see the image. A pricing plan's
*featured* flag is a boolean. None of those have any pixels on the page, so
none of them had an editable surface anywhere — the editor drew a simplified
projection of the block data, and a field it did not draw could not be changed
at all.

docs/121 §1 flagged this as a known limitation and pointed at "the backend".
There was no backend screen for it either.

---

## 2. What it is now

Edit mode is framed the way an editor is framed: a **top bar** and a **right
sidebar**, replacing the single bottom strip that used to cover the last block.

| | |
|---|---|
| **Top bar** | `Editing · <page> · <status>` on the left, **Discard** and **Save** on the right |
| **Blocks tab** | the palette, and a **page outline** |
| **Customize tab** | every field of the selected block |
| **Theme tab** | the palette from docs/121, moved out of its floating box |

Clicking any block on the page selects it and opens Customize on it. The
outline lists the page's blocks with a snippet of their content, so a long page
is navigable without scrolling and it is possible to see what a page is *made
of* at a glance.

### The schema

`SCHEMA` in `website-editor.js` declares what each of the fourteen block types
is made of — the field, its label, and its control (`text`, `area`, `select`,
`bool`, `lines`). The Customize pane is **generated** from it rather than
hand-written fourteen times, so a new block type gets a settings panel by being
described rather than by being drawn twice.

`lines` is worth naming: a textarea that round-trips to an array of strings, so
a pricing plan's feature list is edited as a list instead of as JSON.

### Beyond a snippet panel

* **Insert at the selection.** A new block goes in *after* the selected one and
  is then selected, with Customize open on it. Appending to the end and making
  the user press "move up" eleven times is not editing.
* **Item reordering.** Every repeating item — a plan, a column, a step, an FAQ
  entry — carries its own up / down / delete. Reordering the *contents* of a
  block, not just the blocks.
* **Keyboard.** `Ctrl`/`Cmd`+`S` saves. `Escape` deselects.
* **The outline.** Nothing else tells you what the page contains without
  scrolling it.
* **An unsaved-work guard on the theme.** Applying a theme reloads the page,
  because the palette lives in the page's own `<style>`. If there are unsaved
  block edits it asks first, rather than discarding them silently.

---

## 3. Security — unchanged, and that is the point

The sidebar is more capable, which changes nothing about who may use it.

`website-editor.js` is still a **public static file** with no authority. It is
still the case that anyone can fetch it, define `window.__WSITE_EDIT`
themselves, inject it, and get a full sidebar — and that
`POST /site/api/page/<id>/blocks` answers **401** to a visitor and **403** to an
employee without the Settings / Configuration group, leaving the page
byte-identical. `tests/security/website/site-hardening/attack.mjs` proves it by
doing exactly that.

Two habits carried into the new code:

* **Values, never markup.** The property controls are `<input>`, `<textarea>`
  and `<select>`; their `.value` is a string. Nothing read from a control is
  ever assigned as HTML, and the in-page harvest still reads `textContent`.
* **Labels are `textContent`.** The outline renders block content as a label;
  that content is author-supplied, so it goes in as text.

The server's block-type allowlist is unchanged, so a `type` the schema does not
know is still refused on save.

---

## 4. Tests

`tests/integration/website/editor/` — the browser driver gained nine checks,
because none of this exists outside a DOM:

* the sidebar and top bar appear, and the viewing bar gets out of the way;
* the outline lists the page's blocks;
* clicking a block selects it and opens Customize on it, with controls;
* adding a block from the palette **selects the new block**;
* **a field with no inline representation is editable, and changing it
  re-renders the page** — the assertion the whole feature exists for.

That last one is driven on a heading's `level`, which is a tag name: there is
no way to express it in the rendered page, so if the sidebar were removed the
check could not be made to pass by any other route.

### One thing the first version got wrong

The probe originally assumed the test page's first block was a heading. It was
a text block, so the check failed while the feature worked — verified by
driving the real site, where headings do expose the control. Rewritten to
*add* a heading from the palette first, which made it deterministic and picked
up the "adding selects the new block" assertion on the way past.

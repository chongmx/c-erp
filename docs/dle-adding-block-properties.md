# Document Layout Editor — Developer Reference

`DocumentLayoutEditor` (DLE) lives in `web/static/src/app.js`.
The PDF backend lives in `modules/report/ReportModule.hpp`.

---

## Architecture overview

```
┌─────────────────────────────────────────────────────────────────┐
│  DocumentLayoutEditor (DLE)                                     │
│  Settings → Technical → Document Template                       │
│                                                                 │
│  Left panel      Centre panel     Right sidebar                 │
│  ─────────────   ─────────────    ──────────────                │
│  Block list       Live preview    Property panel                │
│  Add/remove/      (iframe)        (accordion groups             │
│  reorder blocks                    per selected block)          │
└────────────────────────────┬────────────────────────────────────┘
                             │ onSave()
              ┌──────────────┴──────────────┐
              ▼                             ▼
  ir_report_template               ir_config_parameter
  (per-model settings:             key = 'layout.blocks.{model}'
   margins, font, footer,          value = JSON array of blocks
   paper format, etc.)             e.g. layout.blocks.account.move
```

**Blocks** are self-contained units. Their per-block configuration is stored
entirely in the JSON array under `ir_config_parameter`. Some block types
(e.g. `footer_bar`) additionally **sync** critical props to `ir_report_template`
columns on save so the PDF backend can access them directly.

---

## Block object structure

```js
{
  type:    "footer_bar",   // string — matches a key in DLE_BLOCK_DEFS
  visible: true,           // boolean — show/hide toggle
  props:   {               // object — free-form per-block config
    content:       "www.example.com",
    top_sep:       true,
    top_sep_color: "#cccccc",
    line_width:    0.5,
    show_page_num: true,
    page_num_fmt:  "Page {p} of {t}",
    text_source:   "custom",
    // ... all other properties the block type defines
  }
}
```

---

## Two paths for adding a new customisation feature

### Path A — Block-level prop (stored in blocks JSON only)

Use this when the property affects a specific block's appearance and does NOT
need to be read by the PDF backend directly from the database.

**Files touched:** `app.js` only.

| Step | What |
|------|------|
| 1 | Add the prop descriptor to `DLE_PROP_DEFS[block_type]` |
| 2 | Use the prop in `dleBlockHtml()` for the `case 'block_type':` branch |
| (optional) | If the prop needs to affect PDF output, see the Sync Pattern below |

**Example — adding `line_width` to `footer_bar`:**

```js
// Step 1 — DLE_PROP_DEFS
footer_bar: [
    // ...existing props...
    { key: 'line_width', label: 'Line Weight', type: 'number', unit: 'pt', min: 0.25, max: 4 },
],

// Step 2 — dleBlockHtml() case 'footer_bar'
const lineWidth = props.line_width || 1.5;
if (props.top_sep) {
    fStyle += `border-top:${lineWidth}pt solid ${props.top_sep_color}`;
}
```

No backend changes needed. The prop is automatically saved/loaded with the
block JSON.

---

### Path B — Global template setting (stored in `ir_report_template`)

Use this for page-level settings that apply to the whole document (margins,
font, paper format) AND are read directly by the PDF backend.

**Files touched:** `ReportModule.hpp` and `app.js`.

#### B-1. Add the DB column

In `ReportModule.hpp` → `ensureSchema_()`:

```cpp
txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS my_prop INTEGER NOT NULL DEFAULT 0");
```

#### B-2. Expose the column in read/write handlers

**`getSettingById_()`** — add to SELECT and result mapping:
```cpp
// SELECT string
"COALESCE(my_prop, 0) AS my_prop, "

// result loop
rec["my_prop"] = row["my_prop"].as<int>(0);
```

**`handleWrite()`** — read from `vals`, add to both UPDATE branches:
```cpp
int myProp = vals.value("my_prop", 0);
// ...UPDATE SET ..., my_prop=$N WHERE ...
// ...pqxx::params{..., myProp}
```

#### B-3. Read it in the PDF route

In the inline SELECT inside the `/report/pdf/{1}/{2}` lambda:
```cpp
"COALESCE(my_prop, 0) AS mp "
// ...
int pdfMyProp = srows[0]["mp"].as<int>(0);
```

#### B-4. Add to `docSettings` in `setup()`

```js
docSettings: { ..., my_prop: 0 },
```

#### B-5. Load in `onDocTypeChange()`

```js
this.state.docSettings.my_prop = tpls[0].my_prop ?? 0;
```

#### B-6. Save in `onSave()`

```js
// CORRECT — 0 is valid; never use || default when 0 is falsy
my_prop: Number.isInteger(this.state.docSettings.my_prop) ? this.state.docSettings.my_prop : 0,
```

#### B-7. Add a named handler method (critical OWL pitfall)

```js
setMyProp(ev) {
    this.state.docSettings.my_prop = parseInt(ev.target.value);
    this.rebuildHtml();   // if the prop affects preview output
}
```

> **OWL pitfall — inline assignment:** `t-on-change` handlers **must** be named
> class methods when the body is an assignment. Inline arrow functions with
> assignment expressions compile to a statement, producing
> `TypeError: vN is not a function` at runtime. Simple method calls with no
> assignment (e.g. `() => this.toggleBlock(idx)`) are fine inline.

#### B-8. Wire up the sidebar template

```xml
<div class="dle-prop-row">
    <label>My Property</label>
    <select class="dle-prop-input" t-on-change="setMyProp">
        <t t-foreach="[0,1,2,3]" t-as="n" t-key="n">
            <option t-att-value="n"
                    t-att-selected="state.docSettings.my_prop === n ? true : undefined"
                    t-esc="n"/>
        </t>
    </select>
</div>
```

---

### Sync Pattern — block prop that must reach the PDF backend

Some block-level props (e.g. `footer_bar`'s page-number settings) need to be
available in `ir_report_template` so the PDF route can read them without parsing
the blocks JSON. Handle this in `onSave()` after the block is found:

```js
// Extract from the relevant block
const footerBlk = this.state.blocks.find(b => b.type === 'footer_bar' && b.visible !== false);
const fp = footerBlk ? (footerBlk.props || {}) : {};

// Include the synced values in the ir_report_template write call
await RpcService.call('ir.report.template', 'write', [[id], {
    // ...other settings...
    footer_show_page_num: fp.show_page_num !== false,
    footer_page_num_fmt:  fp.page_num_fmt  || 'Page {p} of {t}',
    footer_text_source:   fp.text_source   || 'custom',
    footer_line_color:    fp.top_sep ? (fp.top_sep_color || '#cccccc') : '#cccccc',
    footer_line_width:    fp.top_sep ? (parseFloat(fp.line_width) || 0.5) : 0.5,
}], {});
```

This pattern requires the corresponding DB columns to exist (Path B-1 and B-2).

---

## Property types available in `DLE_PROP_DEFS`

| `type` | Rendered as | Notes |
|--------|-------------|-------|
| `text` | Single-line input | Supports `placeholder` |
| `textarea` | Multi-line input | — |
| `number` | Number input | Supports `unit`, `min`, `max` |
| `color` | Color picker + hex input | — |
| `select` | Dropdown | Requires `options: [{ v, l }, ...]` |
| `boolean` | Checkbox | — |
| `label` | Info-only text | `key` is ignored at runtime |
| `divider` | Section header | Created via `_D('Label')` helper |

---

## Shared property group helpers

These generate standard prop sets used across multiple block types:

| Helper | Props included |
|--------|---------------|
| `_typo()` | `font_size`, `font_family`, `color`, `text_align`, `bold`, `italic` |
| `_spac()` | `padding`, `margin_bottom` |
| `_bgbd()` | `bg_color`, `border_width`, `border_color`, `border_style` |
| `_layout()` | `same_line`, `width` |

---

## `dleStyleStr(props)` — CSS style string builder

Called inside `dleBlockHtml()` to convert a props object into an inline style
string. Handles: `font_size`, `font_family`, `color`, `bg_color`, `text_align`,
`padding`, `margin_bottom`, `border_width/color/style`, `bold`, `italic`.

Use `const st = dleStyleStr(props)` then append custom overrides before
constructing `style="${st}"`.

---

## `dleBlockHtml(type, props, model)` — per-block HTML renderer

Add or modify a `case 'block_type':` branch. Rules:

- Always read props defensively with `|| default` or `?? default`.
- Return a complete HTML snippet (or `''` for invisible structural blocks).
- For browser-only previews, fake data is fine (e.g. "Page 1 of 3").
- Embed relevant props as `data-*` attributes if the PDF backend or future
  tooling might parse the saved HTML.

---

## `dleBuildHtml(blocks, model)` — full document assembler

Iterates the blocks array, skips `visible === false` blocks, handles layout
control blocks (`row_start`, `col_break`, `row_end`), and inserts clearfix
divs after floating blocks (`to_address`, `doc_details`). Called by
`rebuildHtml()` and by `onSave()` before persisting.

---

## Checklist — Path A (block prop only)

| Step | Location |
|------|----------|
| Add descriptor to `DLE_PROP_DEFS[type]` | `app.js` |
| Use prop in `dleBlockHtml()` `case 'type':` | `app.js` |
| (Optional) Sync to `ir_report_template` in `onSave()` | `app.js` + backend |

## Checklist — Path B (global template setting)

| Step | Location | Pitfall |
|------|----------|---------|
| `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` | `ReportModule.hpp` `ensureSchema_()` | — |
| SELECT + serialise in `getSettingById_()` | `ReportModule.hpp` | — |
| Parse + UPDATE in `handleWrite()` | `ReportModule.hpp` | Update both WITH-NAME and WITHOUT-NAME UPDATE branches |
| Read in PDF route inline SELECT | `ReportModule.hpp` PDF lambda | — |
| Add to `docSettings` in `setup()` | `app.js` DLE | — |
| Load in `onDocTypeChange()` | `app.js` DLE | — |
| Save in `onSave()` | `app.js` DLE | Use `Number.isInteger(x) ? x : default`, NOT `x \|\| default` (zero is falsy) |
| **Named method** on class | `app.js` DLE | OWL `t-on-change` with assignment body → `vN is not a function` |
| Call `this.rebuildHtml()` in handler | `app.js` DLE | Without this, preview won't update |
| Wire up sidebar template | `app.js` DLE XML template | Use `t-att-selected` for select options |

# 106 — Manufacturing files on a product, and what a file *is*

Status: **done**. `./scripts/run_tests.sh` → 75 passed, 0 failed. Migration
**1041** applied.

---

## 1. Most of this already existed

The gap analysis said product file attachments were missing. They were not.
`ir_attachment` with `res_model`/`res_id`, a content-addressed filestore,
`POST /web/attachment/upload`, `GET /web/content/{id}`, and a generic
`AttachmentPanel` **already mounted on the product form** — plus invoices, sales
orders, purchase orders and transfers.

Two real things were missing, and one of them was the whole reason it did not
work in practice.

## 2. The allowlist refused every manufacturing format

Uploads accepted `pdf, png, jpg, gif, svg, csv, txt, xlsx, docx, zip`. A Gerber,
a drill file, a pick-and-place or a STEP model was rejected outright, so
"attach the fab package to the product" was impossible even though the machinery
was all there.

Now also accepted:

| Kind | Extensions |
|---|---|
| Gerber | `.gbr .ger .gtl .gbl .gto .gbo .gts .gbs .gm1 .gko .gbp .gtp .gpt .gpb` |
| Drill / route | `.drl .xln .drd .tap` |
| Placement | `.pos .xy` |
| Geometry | `.step .stp .iges .igs .stl .dxf .3mf` |
| EDA source | `.kicad_pcb .kicad_sch .sch .brd .net` |

**The allowlist stays an allowlist.** It was extended by naming inert data
formats, not by loosening the rule. Everything added is read by fabrication
tools and never executed by the server or the browser, and the new types are
served as `application/octet-stream` so a browser downloads rather than renders
them. `.exe` is still refused — asserted in the test.

## 3. `document_type` — what the file *is*

A PCB fabrication package is a dozen files called `top.gtl`, `bot.gbl`,
`outline.gm1`. A flat list of those is a directory listing, not a UI.

Every attachment now carries a `document_type` from a fixed vocabulary:

```
gerber  drill  placement  pcb-design  schematic  netlist
3d-model  drawing  datasheet  specification
image  data  archive  document  other
```

**Classified automatically from the filename**, because nobody labels sixteen
Gerber layers by hand and an unlabelled package is exactly the pile this feature
exists to organise. An explicit `document_type` on the upload always wins, so the
guess is a starting point rather than a verdict. A value outside the vocabulary
is rejected and the guess is used instead — a typo cannot quietly create a group
of one.

Ambiguous extensions deliberately fall through to `document`: a PDF may be a
datasheet, an assembly drawing or a test report, and guessing between those is
worse than leaving it for a person to say. That is what `datasheet` and
`specification` are for — set them explicitly.

## 4. The panel groups instead of listing

`AttachmentPanel` now renders sections in the order someone works through a
build:

**Fabrication** (gerber, drill, placement) → **Design source** (pcb-design,
schematic, netlist) → **Mechanical** (3d-model, drawing) → **Documents**
(everything else, so nothing is ever dropped).

Below them, when — and only when — the record has fabrication data, a
completeness line: **✓ Gerber · ✓ Drill · ✕ Placement**. It answers the question a
fab package actually raises, and staying hidden otherwise means a product with
just a datasheet is never nagged about an incomplete PCB package it was never
going to have.

## 5. Two mistakes worth recording

### Editing an applied migration does nothing

The column was first added by editing migration **1040**, which creates
`ir_attachment`. Migrations are applied once and recorded; changing the body of
one that has already run affects **fresh databases only** and silently does
nothing to every existing install. The column never appeared, and the failure
looked like a build problem rather than a migration one.

Correct fix: a **new** migration, 1041, which adds the column, indexes it, and
back-classifies existing rows from their filenames so history groups the same way
new uploads do.

### `req->getParameter` cannot see a multipart body

The explicit `document_type` override was read with `req->getParameter`, which
only sees the query string. Against a multipart upload it always returned empty,
so every file silently fell back to the guess — including ones uploaded with a
correct explicit type. The neighbouring fields (`res_model`, `res_id`, `name`)
all use `parser.getParameter<std::string>` and were fine.

It failed silently and *plausibly*: files still got a sensible type, so nothing
looked broken. Only asserting the override specifically caught it.

## 6. Not done

- **No `document_type` picker in the UI.** Detection covers the manufacturing
  formats; overriding to `datasheet` or `specification` currently requires the
  API. The panel should offer it.
- **Files are not attached to a BOM revision.** Gerbers belong to Rev C, not to
  the board in general. `ir_attachment` is already generic, so
  `res_model = 'mrp.bom'` works today — what is missing is the product form
  showing its own files *plus* those of its active BOM revision. See docs/105 §5c.
- No preview or thumbnail for images, and no Gerber viewer.

# 105 — Integrating PartKeepr's model into the reference ERP's, properly

Status: **plan, not built.** Nothing in this document has been implemented.

---

## 1. The collision, named precisely

PartKeepr has an entity called **Project**. So does the reference ERP. They are not the same
thing, and neither is what the other means.

| | PartKeepr "Project" | the reference ERP `project.project` | the reference ERP `mrp.bom` |
|---|---|---|---|
| Holds | a list of parts + quantities | tasks and stages | a list of components + quantities |
| Answers | "what does this build need, and reserve it" | "who is doing what by when" | "what is this product made of" |
| Instance of work | *implicit* — the project **is** the build | n/a | separate (`mrp.production`) |

A PartKeepr Project conflates **two** the reference ERP concepts:

1. the **structure** — what the thing is made of → `mrp.bom`
2. the **build** — I am making three of these, hold the parts → `mrp.production`

PartKeepr merges them because it has no manufacturing concept at all. the reference ERP
separates them, and the separation is correct: the structure is stable and
reusable, the build is one dated event that consumes stock. Merging them means
you cannot build the same assembly twice without copying its parts list.

### The decision

> **`mrp.bom` is the one and only parts-list table.** A PartKeepr project becomes
> a BOM. Building it becomes a manufacturing order. `project.project` stays task
> management and merely *links* to the build.

I read "one project and BOM list table" as *one canonical BOM, not two competing
ones* — which is exactly right and is what this plan does. I have deliberately
**not** merged task-management into it, because a table that is both a work
breakdown and a material structure ends up expressing neither well. If you did
mean a single literal table for both, say so before Phase 1 — it changes the
schema materially and is the one assumption here worth confirming.

## 2. Three roles, three tables, one direction of reference

```
   product.product         mrp.bom                 mrp.production
   "a 100nF 0603 X7R"      "Rev C of the           "build 5 of Rev C,
   "PCBA-MAIN Rev C"        MAIN board"             started Tuesday"
   "ENCLOSURE-ASSY"              │                        │
          ▲                      │ lines                  │ consumes
          └──────────────────────┴────────────────────────┘

   project.project / project.task  ──links to──▶  mrp.production
   "route the board", "order the enclosure"
```

Everything a part *is* — parameters, footprint, manufacturer, MPN, stock,
storage location — already lives on `product.product` and its satellites. A BOM
line points at a product and inherits all of it. **No electronics data belongs on
a BOM line.** That separation already exists and must be preserved.

## 3. What `mrp.bom` is missing for a PCBA

Current schema is bare:

```
mrp_bom       id, product_id, code, bom_type, product_qty, product_uom_id,
              company_id, active, subcontractor_id
mrp_bom_line  id, bom_id, product_id, product_qty, product_uom_id, sequence
```

### 3.1 On `mrp_bom`

| Field | Why |
|---|---|
| `bom_kind` | `pcba` \| `mechanical` \| `kit` \| `general`. Drives which columns the form shows and which validations run. A mechanical BOM has no designators; a PCBA must have them. |
| `revision` | Boards are revised constantly — Rev A, Rev B, Rev C. Without it you cannot answer "which revision did we build in March", and every electronics workflow assumes it exists. |
| `revision_of_id` | Points at the previous revision, so a board has a lineage rather than a pile of unrelated BOMs. |

`bom_type` (normal / phantom / subcontract) stays as it is and is **orthogonal**
to `bom_kind`: a PCBA can perfectly well be subcontracted.

### 3.2 On `mrp_bom_line` — the important one

| Field | Why |
|---|---|
| `reference_designators` | `C1,C2,C5,C7`. The single most important PCBA field. One line says "100nF 0603 X7R ×8" and the designators say *where*. |
| `fitted` | Default true. A **DNP** ("do not populate") line stays in the BOM — the pad exists, the part is not placed for this variant. Deleting it loses the information that the position exists. |
| `note` | "orientation critical", "match with R14". |

**The validation that earns its keep:** the number of designators must equal the
quantity. `C1,C2,C5` with quantity 4 is a BOM error, and it is by far the most
common one in hand-edited PCBA BOMs. It is cheap to check and expensive to miss —
it becomes a board with an unpopulated pad or a missing line item on a quote.

### 3.3 New: `mrp_bom_line_alternate`

```
id, bom_line_id, product_id, priority, note, approved
```

An electronics BOM line is rarely one specific part. "100nF 0603 X7R 50V" is
satisfied by Murata, TDK, Yageo or Samsung, and which one you fit depends on who
has stock this week. This is the **approved manufacturer list**, and without it
either the BOM lies (naming one part you substitute in practice) or you maintain
a BOM per supplier.

the reference ERP core has no equivalent. It is the largest genuine addition in this plan
and the one that makes the difference between a toy BOM and one a buyer can work
from.

## 4. The PCBA case, worked through

A main board with three passives and a microcontroller:

```
product.product   PCBA-MAIN            (the board as a sellable/stockable thing)
  mrp.bom         code MAIN, kind=pcba, revision=C, product_qty=1
    line  100nF 0603 X7R    qty 8   designators C1,C2,C5,C7,C11,C12,C14,C19
            alternates: Murata GRM…, TDK C0603…, Yageo CC0603…
    line  10k 0402 1%       qty 4   designators R1,R2,R3,R8
    line  STM32G071         qty 1   designators U1
    line  0R 0603           qty 2   designators R20,R21   fitted=false   (DNP)
```

Building three of them is `mrp.production` with `bom_id` set and `product_qty=3`:
it reserves 24 capacitors, 12 resistors and 3 MCUs, and consuming them writes the
stock moves. That is PartKeepr's "attach parts to the project", done properly and
repeatably.

## 5. The mechanical assembly, and why nesting already works

```
product.product   ENCLOSURE-ASSY
  mrp.bom         kind=mechanical
    line  PCBA-MAIN        qty 1     ◀── a product that has its own BOM
    line  Housing, milled  qty 1
    line  M3×8 screw       qty 6
    line  Gasket           qty 1
```

Multi-level works today with no schema change: a BOM line points at a product,
and that product may have a BOM of its own. Two useful behaviours follow from
`bom_type`, which already exists:

- **normal** — the PCBA is built as its own manufacturing order and stocked, then
  consumed by the enclosure build. Right when the board is a stocked item.
- **phantom** — the PCBA is not built separately; its components are pulled
  straight into the enclosure's build. Right for a sub-assembly that never exists
  on a shelf.

**What is missing is the reverse view.** `mrp_bom_line` has no index for
"which assemblies use this part", and that question is asked every time a
component goes end-of-life or a supplier fails. A **where-used** report over
`mrp_bom_line.product_id`, recursing upward, is small and disproportionately
useful.

## 5a. Multiplicity — "one project has many BOMs"

Correct, and in three separate senses. They are worth keeping apart because they
have different consequences.

### (i) One project → many BOMs

A "Mk3 controller" project produces `PCBA-MAIN`, `PCBA-POWER`, `ENCLOSURE-ASSY`
and a final assembly. Four products, four BOMs, one project.

**This invalidates Phase 5 as I first wrote it.** I proposed
`project.task.production_id` — a single link from one task to one build. That is
too narrow in both directions: a project spans several assemblies, and one
assembly may be built for several projects (a shared power board used by two
products). It needs a **many-to-many**:

```
project_production_rel   project_id, production_id
```

or, if you want the project to know its intended structures before anything is
built, additionally:

```
project_bom_rel          project_id, bom_id
```

The first is the one that matters — it links a project to the *builds* done for
it, which is what makes "what did this project consume" answerable.

### (ii) One product → many BOMs

Also true, and **already permitted**: the only unique index on `mrp_bom` is its
primary key, so nothing stops several BOMs for one `product_id` today.

The problem is that nothing *distinguishes* them either. `mrp_bom` has no
revision, no variant discriminator and no explicit ordering, so with two BOMs for
one product there is no defined answer to "which one does a manufacturing order
use". It would pick arbitrarily.

Legitimate reasons to have several:

| Reason | Example |
|---|---|
| Revision | Rev B and Rev C of the same board |
| Variant | the 24 V build and the 12 V build of one product |
| Method | made in-house, or subcontracted |

So `revision` is **not** a nice-to-have from §3.1 — it becomes load-bearing the
moment a second BOM exists for a product. Alongside it the selection rule has to
be written down and enforced:

> A build picks the BOM for that product that is `active`, with the lowest
> `sequence`, then the lowest id. Superseded revisions are **archived, not
> deleted**.

Archiving rather than deleting matters because `mrp_production.bom_id` already
exists: a finished build permanently records *which revision it was built from*.
Delete the old BOM and every historical order loses the thing that explains it.
That column is already there and is the audit trail — it just needs the
discipline around it.

### (iii) One BOM → many BOMs

Nesting, covered in §5. Works today with no schema change.

## 5b. A kit is not manufactured — and the reference ERP already has the mechanism

A PCBA is **made**: components are consumed, a board comes out, and it takes time
at a work centre. A kit is **packed**: nothing is transformed, the components are
simply picked together and go out in one box.

That difference is exactly what `bom_type` already expresses, and it must not be
re-invented:

| | `bom_type` | What happens | Stock effect |
|---|---|---|---|
| **PCBA** | `normal` | A manufacturing order consumes components and produces the board. Work orders optional. | Components out, board in. The board is a stockable product. |
| **Kit** | `phantom` | **No manufacturing order at all.** At delivery the kit line explodes into its component lines, and those are picked. | Components out. The kit itself is never stocked. |
| **Mechanical assembly** | either | `normal` when the assembly is built and stocked; `phantom` when it only exists inside its parent. | Depends. |

So the two fields are orthogonal and both are needed:

- **`bom_type`** — normal / phantom / subcontract. Drives **behaviour**: whether a
  build exists, whether the parent is stockable, who does the work.
- **`bom_kind`** — pcba / mechanical / kit / general. Drives **documentation and
  validation**: whether reference designators are required, which document slots
  the product shows, which columns the BOM form displays.

**One rule to enforce:** `bom_kind = kit` implies `bom_type = phantom`. A kit that
produces a manufacturing order is a modelling mistake — it would reserve
components to make a thing that does not physically exist. The form should set it
and refuse to let it be changed, rather than leaving the two fields free to
disagree.

A corollary worth stating: because a kit is never stocked, **you cannot ask how
many kits you have**. The answer is derived — the minimum, across components, of
what you hold divided by what one kit needs. If that number is wanted on screen it
has to be computed, not read from `stock.quant`.

## 5c. Manufacturing documents on a product

A PCB is not just a BOM. It has a fabrication package — Gerbers per layer, an
Excellon drill file, a pick-and-place file, an assembly drawing, the schematic,
often a STEP model of the finished board.

### What already exists

More than the earlier gap analysis credited:

- `ir_attachment` with `res_model` / `res_id`, a content-addressed filestore,
  `POST /web/attachment/upload` and `GET /web/content/{id}`.
- A generic `AttachmentPanel` front-end component, **already mounted on the
  product form** as well as invoices, sales orders, purchase orders and transfers.

So "a product can have many files, with a UI to view and access them" was already
true. One thing blocked it in practice:

### What was blocking it, now fixed (docs/106)

The upload allowlist accepted only `pdf, png, jpg, gif, svg, csv, txt, xlsx,
docx, zip` — every manufacturing format was refused. It now also accepts Gerber
(`.gbr .ger .gtl .gbl .gto .gbo .gts .gbs .gm1 .gko …`), Excellon drill
(`.drl .xln .drd .tap`), placement (`.pos .xy`), geometry
(`.step .stp .iges .igs .stl .dxf .3mf`) and EDA project files
(`.kicad_pcb .kicad_sch .sch .brd .net`).

The allowlist stays an allowlist — it was extended by naming inert data formats,
not by loosening the rule. Executables are still refused, and the new types are
served as `application/octet-stream` so a browser downloads rather than renders
them.

### What is still worth building

**A `document_type` on the attachment**, from a fixed vocabulary: `gerber`,
`drill`, `placement`, `assembly-drawing`, `schematic`, `datasheet`, `3d-model`,
`specification`, `other`.

Without it the panel is a flat list, and a real PCB fabrication package is 12–16
files whose names are `top.gtl`, `bot.gbl`, `outline.gm1`. A flat list of those is
not a UI, it is a directory listing. With a type, the product form can show
labelled groups — "Fabrication (8)", "Assembly (3)", "Documentation (2)" — and
answer the question that actually gets asked: *do we have a complete fab package
for this revision?*

**Attach to the BOM revision, not only the product.** Gerbers belong to Rev C, not
to the board in general. Since `ir_attachment` is generic on `res_model`/`res_id`,
this needs no schema change: attach with `res_model = 'mrp.bom'`. The product form
should then show its own files plus those of its active BOM revision, so nobody
has to know which of the two a file was filed against.

## 6. Import is the on-ramp, not a nice-to-have

Nobody types a 200-line PCBA BOM. It comes out of KiCad or Altium as CSV with
roughly: `Designator, Value, Footprint, Quantity, MPN, Manufacturer`.

The importer should resolve each row to a product in this order:

1. **MPN** against `part_manufacturer_info` — exact and unambiguous.
2. **Value + footprint** against `part_parameter` + `part_footprint` — resolves
   "100nF" + "0603" using the SI-aware matching that already exists.
3. **No match** → stage it for review. Never invent a part.

That third step should reuse the `part.lookup` staging pattern already built:
a queue, per-row issues at warning/error level, and a human applying them. The
same argument applies — a wrong capacitor that lands silently becomes a board
that does not work — and reusing the pattern means one review UI, not two.

## 7. Phasing

| Phase | Work | Why this order |
|---|---|---|
| **1** | `bom_kind`, `revision`, designators, `fitted`, designator/quantity validation | Small, self-contained, and immediately makes a PCBA BOM expressible. Nothing else depends on it. |
| **2** | `mrp_bom_line_alternate` + AML on the BOM form | The thing that makes a BOM usable for purchasing. Needs Phase 1's form work in place. |
| **3** | Where-used report, BOM explosion view | Pure read model over data Phases 1–2 produce. |
| **4** | CSV BOM import via the `part.lookup` staging pattern | Depends on 1–2 existing to import *into*, and is the point where real BOMs can arrive. |
| **5** | `project_production_rel` many-to-many (see §5a) | Smallest and least urgent; the two sides are useful apart. **Not** the single `task.production_id` originally proposed — a project spans several assemblies. |

Phase 1 gains one item from §5a: alongside `revision`, the **BOM selection rule**
(active, lowest sequence, lowest id) must be implemented and tested, because
multiple BOMs per product are already possible and currently resolve arbitrarily.

## 8. What not to build

- **A PartKeepr-style project parts-list table.** It is `mrp.bom`. Adding a second
  parts list is the exact duplication this plan exists to prevent.
- **Electronics fields on BOM lines.** Value, footprint, tolerance and manufacturer
  belong to the product. A BOM line records *how many* and *where*, nothing else.
- **A separate PCBA module.** `bom_kind` on the existing model is enough; a parallel
  module would fork the build, reservation and costing logic that already works.

## 9. The one open question

Whether `project.project` should remain separate from `mrp.bom` (this plan) or be
merged with it. This plan keeps them apart and links them, because a table that is
both a work breakdown and a material structure serves neither well — but it is
your call, and it is worth settling before Phase 1 rather than after.

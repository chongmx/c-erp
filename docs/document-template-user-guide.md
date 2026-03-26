# Document Template — User Guide

Access via **Settings → Technical → Document Template**.

---

## Overview

The Document Template editor lets you customise the layout, content, and
appearance of PDFs generated for Invoices, Sales Orders, Purchase Orders,
and Delivery Orders. Changes are reflected immediately in the live preview and
saved to the database when you click **Save**.

---

## Left panel — Blocks

The left panel shows the blocks that make up the document, in order from top
to bottom. Each row shows the block's icon, name, and two action buttons.

### Block types

**Content blocks** produce visible content:

| Block | What it renders |
|-------|----------------|
| Document Header | Company name, document title, and accent line |
| Company Logo | Logo image |
| Company Address | Your company's address |
| Recipient Address | Customer / partner address |
| Document Details | Invoice/order number, date, due date |
| Items Table | Line items with qty, price, subtotal |
| Totals Summary | Subtotal, taxes, grand total |
| Payment Terms | Payment term text |
| Bank Details | Bank account info |
| Notes | Free-form notes |
| Text Block | Custom static text |
| Footer Bar | Fixed footer area (company website, page numbers) |
| Footer Separator | Thin line just above the footer |
| Page Fill Spacer | Elastic spacer that pushes the footer to the bottom |

**Layout blocks** control multi-column layouts:

| Block | Purpose |
|-------|---------|
| Row Start | Begin a side-by-side column group |
| Column Break | Divide columns inside a row |
| Row End | Close a column group |
| Spacer / Gap | Fixed vertical whitespace |
| Separator Line | Horizontal rule |

### Reorder blocks

Drag blocks up or down within the list to reorder them.

### Add a block

Click **+ Add Block** at the bottom of the left panel to open the block
picker. Click any block type to insert it at the end of the list.

### Show / Hide a block

Click the **eye** icon on the block row (or use the **Visible** checkbox in
the right sidebar when the block is selected) to toggle visibility. Hidden
blocks appear greyed-out in the list and are omitted from both the preview
and the generated PDF.

### Remove a block

Click the **×** icon on the block row to permanently remove it. The change
is not committed until you click **Save**.

---

## Right sidebar — Properties

Click any block in the left panel to select it. The right sidebar switches to
the **Properties** tab and shows all configurable options for that block,
organised into collapsible accordion sections. Click a section header to
expand or collapse it.

### Common properties (most blocks)

**Typography**
- **Font Size** (pt) — text size for this block
- **Font** — font family (Default inherits from the global document font)
- **Text Color** — hex color or color picker
- **Align** — Left / Center / Right / Justify
- **Bold / Italic** — toggle switches

**Spacing**
- **Padding** (mm) — inner padding around the block content
- **Margin Below** (mm) — space added below the block

**Background & Border**
- **Background** — fill color behind the block
- **Border Width** (pt) / **Border Color** / **Border Style** — outline

---

## Footer Bar — properties in detail

The Footer Bar appears at the very bottom of every page in the generated PDF.
Select it to configure:

### Content section

**Text Source** — what text appears in the footer (left side):

| Option | Result |
|--------|--------|
| Custom Text | Shows the text you type in the **Custom Text** field |
| Website (from Report Settings) | Reads `report.website` from Settings → Technical → Report Settings |
| None | No text; only page numbers are shown (if enabled) |

**Custom Text** — the literal text to display when **Text Source = Custom**.
You can type a URL, tagline, or any short string.

### Page Numbers section

**Show Page Numbers** — when checked, the page number appears on the right
side of the footer.

**Format** — how the page number is displayed:

| Format | Example |
|--------|---------|
| Page X of Y | Page 2 of 5 |
| X / Y | 2 / 5 |
| Page X | Page 2 |
| X only | 2 |

### Separator Line section

**Show Line** — draws a thin horizontal rule above the footer text.

**Line Color** — color of the separator rule (default `#cccccc`, same grey
used by the table column separators).

**Line Weight** (pt) — thickness of the separator rule (default 0.5 pt).

### Typography, Spacing, Background & Border

Same as the common properties above — let you change the footer's font, color,
padding, and background independently of the rest of the document.

---

## Centre panel — Live Preview

The centre panel shows a live preview of the document rendered with dummy data.
The preview updates automatically whenever you:

- Change any block property
- Show or hide a block
- Reorder blocks
- Change the document type

**Preview Record ID** (top toolbar) — enter the database ID of a real record
to preview the document with actual data instead of dummy placeholders.

**Preview PDF** button — opens the PDF in a new browser tab (requires a valid
record ID and a saved template).

---

## Top toolbar

**Document type selector** — switch between Invoice, Sales Order, Purchase
Order, and Delivery Order. Each document type has its own independent block
layout.

**Save** button — commits all changes (block layout, properties, and footer
settings) to the database. A "Saved!" confirmation appears briefly.

**Tabs on the left panel**: **Blocks** (block list) and **HTML** (raw HTML
editor for advanced customisation).

**Tabs on the right sidebar**: **Properties** (block-level settings) and
**Objects** (document-wide settings — fonts, colors, margins, paper format).

---

## Objects tab — document-wide settings

The **Objects** tab in the right sidebar controls settings that apply to the
whole document (not a single block):

- **Paper Format** — A4, Letter, A3, Legal
- **Orientation** — Portrait / Landscape
- **Accent Color** — primary accent used in headings and table headers
- **Font Family** — global font (overridden per block in Typography)
- **Margins** — Top / Right / Bottom / Left (mm)
- **Font Size** / **Font Color** / **Line Height** — baseline body text
- **Decimal Places** — Qty / Price / Subtotal precision in the items table

---

## Typical workflow

1. Open **Settings → Technical → Document Template**.
2. Select the document type (Invoice, Sales Order, etc.).
3. Rearrange or remove blocks using the left panel.
4. Click a block to configure its properties in the right sidebar.
5. For the **Footer Bar**, choose the text source, enable page numbers, and
   adjust the separator line style.
6. Switch to the **Objects** tab to fine-tune margins and global typography.
7. Type a real record ID in the toolbar and click **Preview PDF** to verify
   the output.
8. Click **Save**.

---

## Tips

- Place a **Page Fill Spacer** block directly above the **Footer Bar** to
  push the footer to the bottom of the last page.
- Use **Footer Separator** instead of the built-in footer **Show Line** if
  you want the separator to appear as a distinct block that can be
  independently hidden.
- The **Website** text source reads from `report.website` in
  **Settings → Technical → Report Settings**. Set it there once and all
  document types will pick it up automatically.
- Changes to the **Footer Bar** properties are also synced to the
  Document Template settings (used by the PDF engine). Always click
  **Save** after adjusting footer settings.

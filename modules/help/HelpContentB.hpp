#pragma once
// =============================================================
// modules/help/HelpContentB.hpp — help for the remaining books (docs/102)
//
// Split from HelpContent.hpp purely for size. Same HelpSeed shape, same rules:
// a section is a row with an empty body, articles point at their section by
// slug, and slugs are stable public addresses.
//
// Everything here is written against the ACTUAL menu tree — the paths quoted in
// these articles were taken from ir_ui_menu, not from memory. Help that names a
// screen which does not exist is worse than no help, because it costs the
// reader a search before they conclude the documentation is wrong.
// =============================================================
#include "HelpContent.hpp"

namespace cerp::modules::help {

static const HelpSeed kHelpSeedsB[] = {

// ============================ PARTS ============================
{"parts","Parts","","parts-find","Finding parts",10,"",""},
{"parts","Parts","","parts-data","Part data",20,"",""},

{"parts","Parts","parts-find","parts-catalogue","The parts catalogue",10,
 "catalogue browse facets filter parametric attributes manufacturer package search",
R"MD(**Products → Parts Catalogue** is the electronics browser: a filter strip across
the top, results underneath.

## How filtering behaves

Three rules, and the third is the one that makes it usable:

- Ticking several values **within one attribute** widens the search — 0402 *or*
  0603.
- Values **across attributes** narrow it — 0402 *and* YAGEO.
- Each attribute's counts are worked out **ignoring its own ticks**, so after
  you tick 0402 the Package box still shows how many 0603 there are. Without
  that you could never add a second value to a selection.

## The filter strip

It scrolls **sideways**. There are more attributes than fit across a screen, and
the strip holds them all rather than pushing the results off the bottom of the
page. Each box scrolls its own values vertically.

**Filter layout: Stacked | Scrolling** switches between one scrolling row and a
wrapped block showing every attribute at once.

## Numeric attributes

Resistance, capacitance, power and the rest get **Min / Max boxes and a unit
picker** instead of a tick list. The span underneath tells you what the current
results actually cover — `0.015 – 820 kΩ`.

Type a bound and press **Enter**, or use **Apply**. Ranges are not applied as you
type, because a half-entered `1` would briefly narrow the world to nothing.

## Which attributes appear

They are **discovered from the results**, not configured. Filter to capacitors
and the strip becomes Capacitance, Voltage Rating, Dielectric; filter to
resistors and it becomes Resistance, Power, Tolerance, Type.

## The results table

Scrolls both ways with a sticky header. Click a column heading to sort. Columns
for the parameters most of the page shares are added automatically. Click any
row to open the product.
)MD"},

{"parts","Parts","parts-find","parts-units","Values, units and notation",20,
 "units si notation 4k7 resistance ohms farads conversion base value search range",
R"MD(A parts catalogue is only searchable if `4k7`, `4.7k` and `4700` are understood
to be the same resistance. They are.

## How a value is stored

Every parameter is stored twice: **as you typed it**, and converted to the base
unit of its quantity. A range search compares the converted number, so notation
never affects whether a part is found.

    4k7 Ω    ─┐
    4.7k Ω   ─┼──►  4700  (base: ohms)
    4700 Ω   ─┘

## Notation that is understood

| You type | It means |
|---|---|
| `4.7k` | 4 700 |
| `4k7` | 4 700 — the suffix stands in for the decimal point |
| `1R2` | 1.2 — `R` is the resistance stand-in |
| `3V3` | 3.3 — and `6V3` is 6.3, as marked on every electrolytic |
| `100n` | 0.000 000 1 |
| `10p` | 0.000 000 000 01 |
| `32k768` | 32 768 — the watch crystal |
| `125m` | 0.125 |

## `uF` is accepted as well as `µF`

The micro sign is not on a keyboard and does not survive most CSV exports, so
every datasheet and distributor listing writes `uF`, `uH`, `uA`. All of them are
read as the µ spelling and **stored** as it, so a search finds both. `ohm`,
`kohm` and `Mohm` are likewise read as `Ω`, `kΩ` and `MΩ`.

Only alternative spellings of the *same* unit are accepted. Nothing is guessed:
`C` stays Coulomb and never becomes Celsius, because a wrong unit is a wrong
number.

## Two things that are not quantities

**Package codes.** `0603` is an identifier, not the number 603 — it is kept
whole and given no numeric value. Better still, put the package in the part's
**footprint** rather than in a parameter, which is where searching for it
actually works.

**Ranges.** `-55 to 125` is kept as text rather than read as −55. If you want an
operating range to be searchable, enter it as two parameters — a
`temperature_min` and a `temperature_max`.

## Quantity kinds keep searches honest

Each unit belongs to a kind — resistance, capacitance, voltage, power,
frequency, time and so on. A search for "between 1 and 10 kΩ" only ever matches
**resistances**. It cannot accidentally return a 4.7 kHz part that happens to
share the number.

## The unit list

**Products → Configuration → Part Units** holds the vocabulary: Ω kΩ MΩ mΩ, F mF
µF nF pF, H mH µH nH, V mV kV, A mA µA nA, W mW kW, Hz kHz MHz GHz, and the rest.

Each has a **factor** converting it to its kind's base. Add a unit here before
using it on a part — a symbol the system does not know is rejected rather than
guessed at.

## Write the magnitude once

The prefix can live in the **value** or in the **unit**. Not both. The stored
number is the value multiplied by the unit's factor, so a `k` in each place is
applied twice and the part is filed a thousand times out.

| Value | Unit | Stored | Verdict |
|---|---|---|---|
| `4k7` | Ω | 4 700 Ω | correct — preferred |
| `4.7` | kΩ | 4 700 Ω | correct |
| `4k7` | kΩ | 4 700 000 Ω | **wrong** — 4.7 MΩ |

The first form is preferred because it is how the value appears on the
schematic, the BOM and the distributor listing.

This is the one notation mistake the system cannot catch by looking at a single
field: `4k7 kΩ` is a perfectly valid resistance, just not the one that was
meant. Typed by hand it stands; proposed by the agent it is corrected and
flagged — see *The AI agent*.
)MD"},

{"parts","Parts","parts-find","parts-parametric","Parametric search",30,
 "parametric search single parameter range min max",
R"MD(**Products → Parametric Search** answers one narrow question: which parts have a
given parameter inside a given range.

Pick the parameter, give a Min and/or a Max, choose the unit, search.

## When to use this instead of the catalogue

Use **Parts Catalogue** for browsing — several attributes at once, counts,
sorting, paging.

Use **Parametric Search** when you already know exactly what you want and only
care about one value. It takes a single parameter and gets out of the way.
)MD"},

{"parts","Parts","parts-data","parts-parameters","Parameters and footprints",10,
 "parameter attribute footprint package add edit product electronics",
R"MD(## Parameters

A parameter is one measured property of a part: Resistance 4k7 Ω, Voltage Rating
50 V, Dielectric X7R.

Parameters with a **unit** become numeric attributes you can range-search.
Parameters with **no unit** — Type, Dielectric, Mounting — become tick-list
attributes.

That is the only rule deciding how an attribute appears in the catalogue: does
it carry a unit.

## Footprints

**Products → Configuration → Footprints** is the package vocabulary — 0402, 0603,
0805, 1206, SOT-23, SOIC-8, TO-220, and so on. A product points at one footprint,
which is what the Package filter groups by.

Footprint names are unique. Two footprints called `0402` would be the same
footprint, so the system will not let you create the second one.

## Demo data

`./scripts/seed_demo_parts.sh` fills the catalogue with a realistic set of
resistors and capacitors so you can see faceting work before entering your own.
`--clean` removes them again. Everything it creates has an internal reference
beginning `DP-`.
)MD"},

{"parts","Parts","parts-data","parts-lookup","The part lookup queue",20,
 "lookup agent proposal review apply reject staging import datasheet distributor ask ai",
R"MD(**Products → Part Lookup** is a review queue for parts proposed by an agent —
something that reads distributor sites and datasheets and suggests catalogue
entries.

Proposals reach the queue two ways: the **Ask the agent** box at the top of the
screen, or a payload posted by an external tool. Both land in the same queue and
go through the same checks.

## Nothing arrives automatically

A proposal is **staged**, never applied. It sits in the queue until a person
approves it. An agent that browses the web will sometimes be confidently wrong,
and a wrong resistance that lands silently in the catalogue becomes a part
somebody solders.

## Ask the agent

Type a manufacturer part number — `RC0805FR-074K7L` — or a description —
`4.7k 0805 1% resistor` — and the agent **searches the web**, reads distributor
listings and datasheets, and comes back with what it found.

Set the provider up first in **Settings → AI Agent**; without one the box is
disabled.

### It answers with candidates, not an answer

A part number usually has one right answer. A description usually does not:
`0805 4.7k 1%` describes a part Yageo, Vishay, Walsin and a dozen others all
make. So you get **several candidates, best first**, each with the reason it is
on the list, and you choose.

Nothing is staged until you press **Stage for review** on one of them. Staging
two is fine if you want to compare them properly.

### What it shows you, and why

| | |
|---|---|
| **searched the web** / **from memory** | whether it actually looked anything up. A model answering from memory still produces a part number and a URL — this is the only thing that tells you which you are reading. |
| **Searched for** | the queries it actually typed. A bad search explains a bad result. |
| **Pages it read** | every source, cited ones first. Open them; that is the point of listing them. |
| **Notes** | the agent's own commentary — what it could not settle, what would narrow it down. |

### If your part number is incomplete

Give it what you have. An incomplete or slightly wrong part number is the case
it is most useful for: it searches, finds the near matches, and says in its
notes what was ambiguous and what would resolve it.

### When it says a magnitude was written twice

Occasionally a model answers `4k7` **and** `kΩ` — the multiplier in both fields,
which would store 4.7 MΩ. The value is taken as authoritative, the unit is
demoted to its base, and the candidate says so in amber.

Read that notice. The correction is a judgement, and it is the reviewer's job to
confirm it was the right one. See *Values, units and notation*.

## Editing a proposal

Every field on a staged proposal is editable — part number, manufacturer, name,
source, datasheet, and the whole parameter table. Add a parameter, remove one,
fix a unit. **Save changes** re-checks it exactly as if it had just been
submitted.

**Confidence is the exception.** It is shown beside the title as a coloured
badge and cannot be changed, here or through the API. It is the agent's
statement about its own certainty, and editing it would not make the part more
reliable — it would only destroy the one signal telling you how hard to check
the rest.

| Badge | Means |
|---|---|
| green | fairly sure — check it the way you would check anyone |
| amber | unsure — open the source before applying |
| red | guessing — verify every field against the datasheet |

Treat the bands as *review effort*, not accuracy. A model's 0.7 is not a
calibrated 70%.

That re-check matters: correcting one field is not a way to slip a bad one past
validation. Fixing whatever made a proposal *Needs fixing* returns it to
*Pending* and Apply becomes available again.

An **applied** proposal is frozen. It is the record of what was written to the
catalogue, and being able to edit it afterwards would leave no way to tell what
actually happened.

## The queue

Filter by state: **Pending**, **Needs fixing**, **Applied**, **Rejected**, or All.

Selecting a proposal shows what was found — query, manufacturer part number,
manufacturer, confidence, source link — then the parameters, and finally where
it will go.

## Issues come first

Problems are listed **above** the parameter table on purpose: you should know
what is doubted before you read the numbers, not after you have decided.

An issue is either a **warning** (low confidence, unknown category) or an
**error** (a unit the system does not know, a value it cannot parse). A proposal
carrying an error is marked *Needs fixing* and **cannot be applied** until it is
corrected — the Apply button says so rather than failing when pressed.

## The four states, and what each one allows

| State | Apply | Edit | Reject |
|---|---|---|---|
| **Pending** | yes | yes | yes |
| **Needs fixing** | **no** — fix the error first | yes | yes |
| **Rejected** | **no** — edit it to reopen it | yes | — |
| **Applied** | no — already done | **no** | **no** |

Only a *Pending* proposal can be applied. The other three are decisions that
have already been taken, and applying anyway would undo one of them quietly.

**Applied is frozen.** It is the record of what was written to the catalogue;
editing or rejecting it afterwards would leave the product in place and the
history claiming otherwise. If an applied part is wrong, correct the *product*.

Editing a *Rejected* proposal returns it to *Pending* — that is how you change
your mind without re-entering everything.

## Applying

Check the category — it is pre-selected from whatever the agent suggested — and
optionally point at an existing product to add the parameters to. Then **Apply to
catalogue**, or **Reject** with a reason.

## Paste a result

**Paste a result** takes a raw JSON payload and runs it through exactly the same
validation. It is how you try a payload while building an agent, without wiring
the agent up first.

The full request and response format is in `docs/LOOKUP_API.md`.
)MD"},

{"parts","Parts","parts-data","parts-labels","Printing labels and QR codes",30,
 "label print qr code barcode drawer sticker sheet a4 scan",
R"MD(Labels are generated as vector graphics and printed straight from the browser, so
they stay sharp at whatever resolution your printer runs at.

## One label

    /label/product/<id>

Options, added to the address:

| Option | Effect |
|---|---|
| `w`, `h` | Label size in millimetres. Default 50 × 25. |
| `payload=code` | The QR holds the internal reference or barcode. **Default.** |
| `payload=url` | The QR holds a link to the product — a phone camera opens it. |
| `text=0` | Do not print the payload as readable characters. |

## A sheet of labels

    /labels/sheet?ids=1,2,3&cols=4&copies=2

`cols` sets the grid, `copies` repeats each label, `gap` spaces them, `w` and `h`
size them.

**Print at 100%.** Do not use "fit to page" — it rescales the sheet and your
labels stop matching the stock.

## What is on a label

The QR, the internal reference in bold, the product name, and the package or
category. The payload is also printed as **readable text**: a scanner reads the
symbol, a person reads the text, and when a label has been scuffed in a parts
drawer the text is what saves it.

Long names are shortened with an ellipsis rather than shrunk indefinitely — text
too small to read is not a saving.

## Which payload to choose

`code` for existing warehouse scanners, which expect the part reference. `url`
for phones, which will open the record. Neither is right for everyone, so it is
a choice rather than a default you cannot change.
)MD"},

// ============================ PRODUCTS ============================
{"product","Products","","prod-basics","Products and categories",10,"",""},
{"product","Products","","prod-variants","Templates and variants",20,"",""},
{"product","Products","","prod-price","Pricing",30,"",""},

{"product","Products","prod-basics","prod-product","Creating a product",10,
 "product create new item reference barcode category uom cost price stock",
R"MD(**Products → Products → New.**

| Field | Notes |
|---|---|
| **Name** | Required. |
| **Internal Reference** | Your own code. Shown on labels and in the catalogue. |
| **Barcode** | Scanned in Inventory; also what a label's QR holds by default. |
| **Category** | Groups products for reporting and for the catalogue tree. |
| **Unit of Measure** | How you count it. |
| **Sales Price** | What you sell it for. |
| **Cost** | What it costs you. |
| **Type** | Storable products track stock; consumables do not. |

## Categories

**Products → Configuration → Categories** is a tree. A product belongs to one
category, and filtering by a parent finds everything underneath it — pick
*Resistors* and you also get *SMD Resistors*.

Keep the tree shallow enough to navigate and deep enough to be useful. Every
level you add is a level someone has to expand.

## Units of measure

**Products → Configuration → Units of Measure** is how you *buy and count*
things: Units, Dozens, Metres.

This is **not** where electronic values live. A resistor is counted in Units and
*measured* in ohms; the ohms are a part parameter. Mixing the two corrupts
purchasing — see the Parts book.
)MD"},

{"product","Products","prod-basics","prod-vendors","Vendors and purchase prices",20,
 "vendor supplier pricelist purchase price lead time minimum quantity supplierinfo",
R"MD(**Products → Configuration → Vendor Pricelists** records what each supplier
charges you for a product.

A line holds the vendor, the product, a **minimum quantity**, the **price** at
that quantity, and optionally a delivery lead time and the vendor's own product
code.

Several lines for one product describe a quantity break: 1+ at one price, 100+ at
another. Purchasing picks the line matching the quantity you are ordering.

This is the buying side. Selling prices live in **Pricelists** — see *How a price
is chosen*.
)MD"},

{"product","Products","prod-variants","prod-templates","Templates, attributes and variants",10,
 "template variant attribute value generate combination size colour matrix",
R"MD(A **product template** is the thing you sell; a **variant** is the specific one
you ship. "T-shirt" is a template; "T-shirt, Large, Blue" is a variant.

## Setting it up

1. **Products → Configuration → Attributes** — create the attribute (Size) and
   its values (S, M, L).
2. **Products → Product Templates** — open a template and add an
   **attribute line**: the attribute, and which of its values this template
   offers.
3. **Generate variants**. One variant is created for each combination.

Two attributes with three values each produce nine variants. That number grows
fast; add attributes deliberately.

## Removing a value later

Variants are **archived, not deleted**, when you remove an attribute value. Put
the value back and the original variants return — with their history, their
stock and their references intact.

Deleting them would lose all of that, and a variant that has ever been sold is
not safe to delete.

## Products that are not variants

Most products need none of this. A resistor is not a variant of anything; it is
its own product with its own parameters. Templates earn their keep when the same
item genuinely comes in a matrix of options.
)MD"},

{"product","Products","prod-price","prod-pricelists","How a price is chosen",10,
 "pricelist price rule discount formula quantity break date customer precedence",
R"MD(**Products → Configuration → Pricelists** hold your selling prices;
**Price Rules** are the individual lines.

## Rules and precedence

A rule can apply to one **product**, a whole **category**, or **everything**. When
more than one rule could apply, the most specific wins:

    product  >  category  >  everything

Within the same level, the lowest **sequence** wins, then the lowest id. That
ordering is fixed, so the same basket always prices the same way.

## What a rule can do

- A **fixed price**.
- A **percentage** off the list price.
- A **formula** — list price, a discount, then optional rounding and a margin
  floor.

## Quantity breaks and dates

A rule can carry a **minimum quantity**, so 100+ prices differently from 1+. It
can also carry a **start and end date**, which is how a promotion expires without
anybody remembering to remove it.

## Assigning a pricelist

Set one on a customer and their orders use it. An order can also carry its own
pricelist directly.

> Order lines do not yet re-price themselves automatically when you change the
> pricelist on an existing order.
)MD"},

// ============================ INVENTORY ============================
{"stock","Inventory","","inv-ops","Moving stock",10,"",""},
{"stock","Inventory","","inv-setup","Setting up your warehouse",20,"",""},
{"stock","Inventory","","inv-report","Knowing what you have",30,"",""},

{"stock","Inventory","inv-ops","inv-transfers","Receipts, deliveries and transfers",10,
 "transfer picking receipt delivery internal move validate reserve done draft",
R"MD(Everything that moves stock is a **transfer**. **Inventory → Operations** splits
them by direction:

| Menu | What it is |
|---|---|
| **Receipts** | Goods arriving from a vendor. |
| **Deliveries** | Goods going out to a customer. |
| **Internal Transfers** | Stock moving between your own locations. |
| **All Transfers** | Everything, unfiltered. |

## The life of a transfer

    Draft  →  Ready  →  Done

A transfer starts as a draft list of what should move. Confirming it reserves the
stock. **Validating** it is the moment the quantities actually change — before
that, nothing has moved.

If you only shipped part of an order, correct the done quantities before
validating. What you validate is what the system believes happened.

## Barcode

**Inventory → Operations → Barcode** is the scanning screen for picking without a
keyboard.
)MD"},

{"stock","Inventory","inv-ops","inv-landed","Landed costs",20,
 "landed cost freight duty customs valuation allocate",
R"MD(**Inventory → Operations → Landed Costs** adds the costs of getting goods to you
— freight, duty, handling — onto the value of the goods themselves.

Without it, a container of stock is valued at the invoice price and the shipping
disappears into expenses, so your margins look better than they are.

Create a landed cost, pick the receipt it applies to, enter the amounts, and
apply. The extra cost is spread across the received products and their stock
valuation goes up accordingly.
)MD"},

{"stock","Inventory","inv-setup","inv-locations","Warehouses, locations and putaway",10,
 "warehouse location putaway rule bin shelf structure",
R"MD(## Locations

**Inventory → Configuration → Locations** is a tree describing where stock can
physically be — a warehouse, an aisle, a shelf, a bin.

Stock always sits in a location. "In stock" is really "the total across every
internal location".

## Warehouses

**Inventory → Configuration → Warehouses** groups locations and defines the
operations that run there.

## Operation types

**Inventory → Configuration → Operation Types** define the kinds of transfer a
warehouse performs — its receipts, its deliveries, its internal moves — and which
locations each moves between.

## Putaway rules

**Inventory → Configuration → Putaway Rules** answer "where does this go when it
arrives". A rule sends a product, or a whole category, to a particular location
automatically, so incoming goods land somewhere sensible without anyone deciding
each time.
)MD"},

{"stock","Inventory","inv-setup","inv-reorder","Reordering rules",20,
 "reorder rule minimum maximum orderpoint replenish automatic stock level",
R"MD(**Inventory → Configuration → Reordering Rules** keep a product between a minimum
and a maximum.

When stock falls below the minimum, the rule proposes bringing it back up to the
maximum. Set the minimum to cover demand during your supplier's lead time —
that is the whole point of the number.

A product with no rule is never reordered automatically; somebody has to notice.
)MD"},

{"stock","Inventory","inv-setup","inv-lots","Lots, serial numbers and packages",30,
 "lot serial number traceability package pallet box tracking",
R"MD(## Lots and serial numbers

**Inventory → Products → Lots/Serial Numbers** tracks individual items or
batches. A serial number identifies one unit; a lot identifies a batch that
moved together.

Turn tracking on for a product only if you will actually record the numbers.
Half-recorded traceability is worse than none, because it looks complete.

## Packages

**Inventory → Products → Packages** groups physical units — a pallet, a box —
so they can be moved as one thing rather than item by item.
)MD"},

{"stock","Inventory","inv-report","inv-onhand","On hand, moves and valuation",10,
 "on hand quant stock level history moves valuation layer report cost",
R"MD(Three reports, answering three different questions.

| Report | Answers |
|---|---|
| **On Hand** | What do I have, and where is it? |
| **Moves History** | How did it get there? |
| **Inventory Valuation** | What is it worth? |

## On Hand

Current quantity per product per location. Group by location to see a site;
group by product to see everything of one kind.

## Moves History

Every stock movement, with its source and destination. This is where you go when
a quantity is wrong — the number is the result of a sequence of moves, and the
mistake is one of them.

## Inventory Valuation

Each layer of value as stock came in and went out. Stock value changes when goods
move, not when invoices are posted, so this is the report that reconciles against
your accounts.
)MD"},

// ============================ SALES ============================
{"sale","Sales","","sale-flow","Selling",10,"",""},

{"sale","Sales","sale-flow","sale-orders","Sales orders",10,
 "sale order quotation customer confirm line product quantity price delivery invoice",
R"MD(**Sales → Orders → Sales Orders.**

## Creating one

New, choose the **customer**, then add **order lines** — product, quantity, unit
price. The price starts from the product's sales price, adjusted by whichever
pricelist applies to that customer.

## Confirming

A confirmed order is a commitment: it becomes the basis for a delivery and, later,
an invoice. Until it is confirmed it is a quotation and changing it costs
nothing.

## Taxes and totals

Lines carry taxes; the order totals untaxed, tax and total. Amounts are held to
six decimal places internally and rounded for display, so a long order does not
drift by a cent.

## What follows an order

- **Inventory** ships it — see *Receipts, deliveries and transfers*.
- **Accounting → Customers → Invoices** bills it.
)MD"},

{"sale","Sales","sale-flow","sale-portal-quotes","Quote requests from the customer portal",20,
 "portal quote request customer draft order self-service website price confirm",
R"MD(Customers with portal access can ask for a quotation themselves. Their request
arrives as an ordinary **draft sales order** with the origin *Portal Quote
Request*, waiting for somebody to price and confirm it.

## What arrives, and what does not

A request creates **a draft order and nothing else**. It does not confirm
anything, does not reserve stock, does not create a delivery, and does not touch
the ledger.

**The customer cannot set prices.** Lines are priced from the product's own list
price; a price sent with the request is ignored. Nor can they order something
that is not marked sellable — those lines are dropped, and a request with no
usable line is refused outright rather than landing as an empty order.

## Handling one

1. **Sales → Orders → Sales Orders**, filter on the *Portal Quote Request*
   origin, or just look at the drafts.
2. Check the lines and set the real price — a pricelist, a discount, whatever
   the customer has actually been promised.
3. Confirm it, and the ordinary flow takes over from there.

## The rule behind the design

> **The portal proposes; staff dispose.**

A portal visitor authenticates with a customer password, not a staff login. So
every customer-facing action produces something a person reviews, never a posted
document. If you extend the portal, keep to that line: the moment a customer can
confirm an order or post to the ledger, a leaked customer password becomes a
leak of your books.
)MD"},

{"sale","Sales","sale-flow","sale-portal-statement","Statements and shared document links",30,
 "statement account balance ageing aging outstanding share link token portal customer send",
R"MD(Two things the customer portal can do that do not need anybody to log in and
hunt for a document.

## Statement of account

A customer signed into the portal can pull their own **statement** for any date
range, on screen, printed, or as a PDF.

It is built from the **receivable ledger**, not by adding up invoices and taking
off payments. That matters more than it sounds: a statement assembled the second
way is a *second opinion* about what the customer owes, and second opinions
drift from the books. This one is the ledger, so it agrees with your trial
balance by construction — and it automatically picks up credit notes,
write-offs and manual journal entries without anybody adding a rule for each.

It shows an opening balance, every movement in the window with a running
balance, the closing balance, and an **ageing** breakdown — current, 1–30,
31–60, 61–90 and 90+ days. Ageing uses what is still outstanding on each
invoice, so an invoice half paid ages by the half that is left.

Only **posted** documents appear. A draft invoice is not a debt.

## Sharing one document by link

Sometimes you need to send somebody a single invoice — an accounts department
that has no portal login, an insurer, a customer who cannot find the email.

**Generate a share link** for an invoice, sales order or delivery and you get a
URL that shows that **one document**, read-only, to whoever opens it. No
account, no password.

Because the link *is* the credential, it behaves accordingly:

* it opens **exactly one document** — it cannot be edited to show the next
  invoice, or any other customer's;
* it **expires** (30 days by default);
* **sharing again replaces the previous link.** If a link has gone somewhere it
  should not have, generate a new one and the old one stops working — you do not
  have to hunt for a revoke button first;
* you can **revoke** it outright at any time;
* it grants read access only, and creates no session.

Treat it like a password: anyone who has the link can see that document. For
anything ongoing, give the customer portal access instead — that way they see
their own documents and you can switch it off in one place.
)MD"},

// ============================ PURCHASE ============================
{"purchase","Purchase","","pur-flow","Buying",10,"",""},

{"purchase","Purchase","pur-flow","pur-orders","Purchase orders",10,
 "purchase order vendor supplier confirm receive bill line quantity price",
R"MD(**Purchase → Orders → Purchase Orders.**

## Creating one

New, choose the **vendor**, add lines. The price comes from the vendor pricelist
for that product and quantity if one exists — see *Vendors and purchase prices*.

## Confirming

Confirming turns the order into a commitment to buy and sets up the expected
**receipt** in Inventory.

## Then what

1. Goods arrive → validate the **receipt** in Inventory.
2. The vendor's invoice arrives → enter it under
   **Accounting → Vendors → Bills**.
3. Pay it → **Accounting → Vendors → Payments**.

Keeping those three separate is deliberate: ordering, receiving and being billed
happen at different times and can disagree. That disagreement is exactly what you
want to be able to see.
)MD"},

// ============================ ACCOUNTING ============================
{"account","Accounting","","acc-daily","Day to day",10,"",""},
{"account","Accounting","","acc-bank","Bank and payments",20,"",""},
{"account","Accounting","","acc-setup","Configuration",30,"",""},
{"account","Accounting","","acc-report","Reporting",40,"",""},

{"account","Accounting","acc-daily","acc-invoices","Customer invoices and credit notes",10,
 "invoice customer bill credit note refund post draft residual paid partial",
R"MD(**Accounting → Customers → Invoices.**

## The states

    Draft  →  Posted  →  Paid

A **draft** invoice can be edited freely and affects nothing. **Posting** creates
the journal entries and makes it real. A posted invoice is not edited — it is
corrected with a **credit note**.

That restriction is not bureaucracy: a posted invoice has already changed your
accounts, and silently editing it would leave the books disagreeing with the
documents you sent out.

## Residual and payment state

**Residual** is what is still owed. As payments are matched to the invoice the
residual falls and the payment state moves from *not paid* through *partial* to
*paid*.

## Credit notes

**Accounting → Customers → Credit Notes** reverses all or part of an invoice.
A credit note can also stand alone when you owe a customer something that does
not relate to one specific invoice.

## Vendor bills

**Accounting → Vendors → Bills** is the same machinery in the other direction —
what you owe rather than what you are owed.
)MD"},

{"account","Accounting","acc-daily","acc-entries","Journal entries",20,
 "journal entry debit credit balance double entry ledger misc",
R"MD(**Accounting → Journal Entries** shows every entry, however it was created.

Most entries are made for you — posting an invoice, validating a payment,
receiving stock. You write one by hand for things with no document behind them:
an accrual, a correction, an opening balance.

## The rule

Every entry balances. Total debits equal total credits, always. An entry that
does not balance cannot be posted, because a ledger that does not balance cannot
be trusted for anything else.

## Journals

Entries live in **journals** grouped by kind — Sales, Purchases, Bank and Cash,
Miscellaneous. **Accounting → Journals** shows each on its own.
)MD"},

{"account","Accounting","acc-daily","acc-expenses","Employee expenses",30,
 "expense report employee reimburse claim receipt sheet",
R"MD(**Employees → Employee Expenses** is where staff record what they spent;
**Accounting → Expense Reports** is where those are grouped into a claim,
approved and posted.

An expense becomes a payable to the employee once its report is posted, and is
settled like any other payment.
)MD"},

{"account","Accounting","acc-bank","acc-reconcile","Bank reconciliation",10,
 "bank reconcile statement line match invoice payment suggest clear",
R"MD(**Accounting → Bank Reconciliation** matches what your bank says happened against
what your books say happened.

## How it works

1. A **bank statement** brings in lines — date, description, amount.
2. Open a line. The system **suggests matches**: open invoices with the same
   amount, then the same partner, most recent first.
3. Pick the right one and **reconcile**.

Reconciling does three things at once: it clears the invoice, marks the statement
line done, and posts the bank journal entry. They happen together because a bank
line that is cleared without an entry, or an entry without a cleared line, is a
discrepancy someone has to chase later.

## When nothing is suggested

The amount may differ, the partner may be missing from the statement line, or the
invoice may not be posted yet. Suggestions are ranked by exact amount first, so a
line that does not match any invoice's amount will not offer much.

## Statements

**Accounting → Configuration → Bank Statements** holds the statements themselves;
**Accounting → Journals → Bank and Cash** is the day-to-day view.
)MD"},

{"account","Accounting","acc-bank","acc-payments","Payments",20,
 "payment customer vendor receive pay allocate partial outstanding",
R"MD(**Accounting → Customers → Payments** records money in;
**Accounting → Vendors → Payments** records money out.

A payment can be **allocated** against one or more invoices. Allocation is what
reduces an invoice's residual — an unallocated payment sits as an outstanding
balance for that partner until you assign it.

A payment larger than one invoice can be spread across several. A payment smaller
than an invoice leaves it partially paid.
)MD"},

{"account","Accounting","acc-setup","acc-chart","Chart of accounts and journals",10,
 "chart account type journal code setup configuration ledger",
R"MD(## Chart of accounts

**Accounting → Configuration → Chart of Accounts** is the list of accounts every
entry posts to. Each has a **code**, a **name** and a **type**.

The **type** is what makes reporting possible — it decides whether an account is
an asset, a liability, income or an expense, and therefore which report it lands
in. Get the type wrong and the account still works but the reports are wrong.

## Journals

**Accounting → Configuration → Journals** define where entries are recorded and
which accounts they default to. Sales, Purchases, Bank, Cash, Miscellaneous.

## Journal groups

**Journal Groups** bundle journals for reporting, so a report can cover "all bank
journals" without listing them.
)MD"},

{"account","Accounting","acc-setup","acc-tax","Taxes, currencies and fiscal positions",20,
 "tax rate currency exchange rate fiscal position incoterm sst",
R"MD(## Currencies

**Accounting → Configuration → Currencies** holds each currency and its rate.
Amounts are stored to six decimal places, so repeated conversion does not
accumulate rounding error.

## Fiscal positions

**Accounting → Configuration → Fiscal Positions** adapt taxes and accounts to a
customer's situation — a different country, an exemption. Set one on a partner
and their documents use the adapted taxes automatically, instead of somebody
remembering to override each line.

## Incoterms

**Accounting → Configuration → Incoterms** record who bears cost and risk at
which point of a shipment. They appear on trade documents.
)MD"},

{"account","Accounting","acc-setup","acc-assets","Assets and budgets",30,
 "asset depreciation budget position analytic forecast",
R"MD(## Assets

**Accounting → Assets** tracks things you own and write down over time.
An asset has a value, a type controlling how it depreciates, and a schedule of
depreciation lines that post as time passes.

**Asset Types** define the method and duration once, so every asset of that kind
behaves the same.

## Budgets

**Accounting → Budgets** sets expected amounts per **budgetary position** over a
period, and compares them with what actually happened.

**Budgetary Positions** group the accounts a budget line watches, so a budget is
written against "Travel" rather than against a list of account codes.

## Analytic accounts

**Configuration → Analytic Accounts** classify costs and revenue along a second
dimension — by department, by job, by site — independently of the chart of
accounts. **Analytic Items** are the individual postings.
)MD"},

{"account","Accounting","acc-report","acc-reports","Financial reports and the dashboard",10,
 "report profit loss balance sheet trial dashboard aged partner ledger",
R"MD(**Accounting → Dashboard** is the overview: what you are owed, what you owe, cash
position, recent activity.

**Accounting → Reporting → Financial Reports** produces the statements — profit
and loss, balance sheet, and the supporting ledgers.

## Reading them

A report is only as good as the **account types** behind it. If something appears
in the wrong section, the account's type is usually the reason rather than the
entry.

Reports are scoped to the company you are working in. If a figure looks too small
or too large, check which company is active before checking the entries.
)MD"},

// ============================ MANUFACTURING ============================
{"mrp","Manufacturing","","mrp-make","Making things",10,"",""},

{"mrp","Manufacturing","mrp-make","mrp-bom","Bills of materials",10,
 "bom bill of materials component quantity structure phantom subcontract",
R"MD(**Manufacturing → Products → Bills of Materials** describes what a product is
made of.

A BOM has a **product**, a **quantity** it produces, and **component lines** —
each a product and how many of it are consumed.

## BOM types

| Type | Meaning |
|---|---|
| **Normal** | Manufactured as its own step. |
| **Phantom** | Not made separately; its components are pulled into the parent. |
| **Subcontract** | Made by an outside supplier from components you provide. |

## Quantities are per BOM, not per unit

If the BOM produces 10 and consumes 25 of a component, one unit consumes 2.5.
Setting the produced quantity to something other than 1 is how you express a
recipe that only makes sense in batches.
)MD"},

{"mrp","Manufacturing","mrp-make","mrp-import","Importing a BOM from CSV",15,
 "bom import csv kicad altium eagle jlcpcb designator dnp header mapping draft assistant",
R"MD(**Manufacturing → BOM Editor** imports a bill of materials from a CSV export.
Paste it, or drop the file on the panel on the right.

## What it does with the file

Headers are matched automatically, then every row is resolved against your
catalogue — by manufacturer part number first because that is exact, then by
value and footprint.

Reference ranges are expanded: `R1-R4` becomes four designators, and the count
is checked against the quantity column. A designator used twice on one board is
an error, because it cannot be true.

**Nothing reaches the BOM until you press Import.** Every row is staged with a
status first:

| | |
|---|---|
| plain | resolved and consistent |
| **amber** | it will import, but look at it — several parts matched, or a line has no designators |
| **red** | it cannot import — no part chosen, quantity ≤ 0, designator count ≠ quantity, or a designator used twice |

Lines in error are not imported and cannot be. A half-imported BOM is worse
than none, because it looks complete.

## The tools it already knows

| Tool | Columns it writes |
|---|---|
| **KiCad** | Reference, Qty, Value, Footprint, Datasheet, Description, DNP |
| **Altium** | Designator, Comment, Description, Footprint, LibRef, Quantity, Manufacturer Part Number |
| **EAGLE** | Qty, Value, Device, Package, Parts — usually **semicolon**-separated |
| **JLCPCB** | Comment, Designator, Footprint, LCSC Part # |

Commas, semicolons and tabs are all accepted, so an EAGLE export needs no
editing first.

Two of those are worth knowing about:

- **`Comment` is the component value**, not a note — that is what Altium and
  JLCPCB put there.
- **`DNP` means *not* fitted.** A marked line is staged as do-not-populate. Read
  the other way round it would populate exactly the parts you left off.

## When the columns are not recognised

Every tool names them differently, and a wrong guess imports wrong data with no
error anywhere — so an unknown header row stops and asks rather than guessing.

**Ask the assistant** and it reads the header row and proposes which column is
which. You get a row of dropdowns to check and change before anything is
parsed, because it is a guess about your file: cheap to correct there, tedious
to unpick afterwards. **Map by hand** does the same without asking anyone.

Only the header and a couple of sample rows are sent — enough to work out the
shape, and it keeps your part list off a vendor's servers.

## Tidying rows to house conventions

Once rows are staged, **Tidy up with the assistant** rewrites them the way this
ERP expects them written:

| Written as | Becomes |
|---|---|
| `4.7K ohm` | `4k7` |
| `100nF` | `100n` — the unit belongs in its own field |
| `Capacitor_SMD:C_0603_1608Metric` | `0603` |
| `"C1, C2, C5"` | `C1,C2,C5` |
| `R-EU_R0603` in an MPN column | *(blank)* — that is a library reference, not a part number |
| `DEVICE=INDUCTOR`, `~` | *(blank)* — tool noise |
| a row whose description says DNP | marked **not fitted** |
| `R1-R4` with no quantity | quantity **4** |

It is offered where it pays: a row that did not resolve is usually a row
written in a way the ERP cannot read, not a part you do not stock.

**It proposes, and shows you a diff.** Every change is listed — old value struck
through, new value beside it — and nothing happens until you press **Apply and
re-resolve**. A bulk rewrite nobody reads is a bulk rewrite nobody can undo.

**It never chooses a part.** It rewrites text; the rows are then resolved
against your catalogue again exactly as on the first import. That is what keeps
the result reproducible — the same rows always resolve the same way.

If it hands back a different number of rows than it was given, the whole answer
is refused and nothing changes. A BOM quietly missing a line is far worse than
an untidy one: the board is short a part and nothing says so.

Up to 300 rows at a time. That is a limit on cost, and on how much of your part
list leaves the building.

## Cancel, and drafts

**Cancel** abandons the attempt — while it is parsing, too, which is when you
usually notice you dropped last month's export. It touches nothing staged.

A staged import **is** a draft. It survives leaving the screen and is only lost
when you import it or discard it, so you can come back to a half-reviewed BOM
tomorrow. The panel says when it was started, and warns you if it has been
sitting there long enough that the file has probably moved on.

**Discard** throws the draft away. The BOM itself is untouched.
)MD"},

{"mrp","Manufacturing","mrp-make","mrp-orders","Manufacturing and work orders",20,
 "manufacturing order production work order workcenter routing produce consume",
R"MD(## Manufacturing orders

**Manufacturing → Operations → Manufacturing Orders** is an instruction to make a
quantity of something. It pulls its components from the BOM, reserves them,
consumes them when produced, and creates the finished goods.

Stock only changes when the order is **done** — before that it is a plan.

## Work orders

**Manufacturing → Operations → Work Orders** breaks a manufacturing order into
the steps performed at each **work centre**. Use them when you need to know where
in the process something is, or how long each step actually took.

## Work centres

**Manufacturing → Configuration → Work Centers** define where work happens, with
a cost per hour and an efficiency factor used for scheduling.

## Master production schedule

**Manufacturing → Planning → Master Production Schedule** takes a longer view:
what is forecast, what is already committed, and what therefore needs starting.
)MD"},

// ============================ RENTAL ============================
{"rental","Rental","","rent-ops","Running rentals",10,"",""},

{"rental","Rental","rent-ops","rent-units","Units and contracts",10,
 "rental unit contract tenant lease occupancy available term",
R"MD(## Units

**Rental → Operations → Units** are the things you rent out. Each has a **unit
type**, a rate, and a state telling you whether it is available, occupied or
unavailable.

**Rental → Configuration → Unit Types** define the categories and their default
terms.

## Contracts

**Rental → Operations → Contracts** link a unit to a tenant for a period, at a
rate. The contract drives billing and occupancy.

Ending a contract frees the unit. Overlapping contracts on one unit are what the
unit state exists to prevent.

## Dashboard and events

**Dashboard** shows occupancy and income at a glance. **Events** is the timeline —
what happened to which unit and when.

## Expenses

**Rental → Operations → Expenses** records costs against units, categorised by
**Expense Categories**, so a unit's profitability is its income minus what it
actually cost to keep.
)MD"},

// ============================ EMPLOYEES ============================
{"hr","Employees","","hr-people","People",10,"",""},

{"hr","Employees","hr-people","hr-employees","Employees, departments and schedules",10,
 "employee department job position working schedule calendar manager contact",
R"MD(**Employees → Employees** holds one record per person: their job position,
department, manager, work contact details and their linked user account.

The **user account** link matters — it is what connects a person to the work
assigned to them and the hours they log in Project.

## Departments

**Employees → Departments** is a tree. It drives grouping in reports and the
approval path for expenses.

## Job positions

**Employees → Configuration → Job Positions** are the roles people hold,
independent of the individuals in them.

## Working schedules

**Employees → Configuration → Working Schedules** define working hours. They are
what "a working day" means for planning and for time off.
)MD"},

{"hr","Employees","hr-people","hr-attendance","Attendance — clocking in and out",20,
 "attendance clock in out punch hours worked timesheet kiosk shift check-in check-out",
R"MD(**Employees → Attendance** is the record of who was at work and for how long.
Each row is one shift: a check-in, a check-out, and the hours between them.

## Clocking

Anyone with access can clock a person in or out from the Attendance screen. Most
shops instead use the **kiosk** — see *The clock-in kiosk*.

## The hours are worked out for you

**Worked hours are calculated from the two timestamps, on the server.** You
cannot type them in: editing the field has no effect, and the number is
recalculated from the check-in and check-out every time the record is saved.
That is deliberate — hours that can be typed are hours that can be wrong, and
nobody would know which.

## What cannot happen

* **Nobody can be clocked in twice.** One person can have at most one open
  shift. Two taps on a kiosk half a second apart produce one record, not two —
  the database enforces it, so no screen or script can get around it.
* **A shift cannot end before it starts**, and a check-out cannot overlap a
  shift that is already closed.

## Reading the screen

A row with no check-out is somebody who is **still clocked in**. Filter on it to
see who is on site right now. At the end of a day, an open row usually means
somebody forgot to clock out — edit the check-out time and the hours correct
themselves.
)MD"},

{"hr","Employees","hr-people","hr-timeoff","Time off — leave, allocations and balances",30,
 "leave time off holiday annual sick unpaid emergency allocation balance approve refuse days entitlement",
R"MD(Time off is three things that work together: **types**, **allocations** and
**requests**.

| | Where | Is |
|---|---|---|
| Leave type | Configuration → Leave Types | The kind of leave: Annual, Sick, Unpaid, Emergency |
| Allocation | Employees → Allocations | How many days a person is entitled to |
| Request | Employees → Time Off | A person asking to be away on given dates |

## The balance

> **remaining = approved allocations − approved leave**

A type marked **Requires Allocation** is capped by that number: approving a
request for more days than remain is refused. **The balance can never go
negative.** Annual and Sick work this way; Unpaid and Emergency do not, because
they are granted on the merits rather than against an entitlement.

A **draft** allocation is not a balance. It has to be submitted and approved
before it counts — otherwise anyone could grant themselves days.

## Days are counted as WORKING days

The day count is worked out from the dates, on the server, and **excludes
Saturdays, Sundays and public holidays**. A request from Monday to Sunday is
five days, not seven. A request covering only a weekend is zero days and cannot
be approved.

Like attendance hours, the number is derived — typing a different figure has no
effect.

## Public holidays

**Employees → Configuration → Public Holidays** is **empty when the system is
installed, on purpose.** Public holidays differ by year and by state, and a
wrong date silently miscounts every leave request that spans it. Enter the dates
that apply to you before the first leave request of the year, and the day
counter will use them from then on.

## The workflow

    Draft → To Approve → Approved
                      ↘ Refused

* Only a **submitted** request can be approved — nothing skips the queue.
* An approved request cannot be approved again, and a refused one cannot be
  approved at all.
* **An approved request cannot be edited.** Reset it to draft first. Editing
  dates in place would move a balance somebody has already been told about.
* **Cancelling approved leave returns the days** to the balance immediately.
* One person cannot hold two approved leaves over the same dates. Two *pending*
  requests that overlap are fine — that is a decision for whoever approves, not
  an error.
)MD"},

{"hr","Employees","hr-people","hr-kiosk","The clock-in kiosk",40,
 "kiosk pin tablet clock punch badge door shared device attendance terminal",
R"MD(The kiosk is a page meant for a tablet by the door: a keypad, a PIN, and one
button. Open **/kiosk** in a browser on the device and leave it there.

## Setting somebody up

Open the employee, set a **PIN** (digits only, at least four), and they can
punch. **Employees → Employees → *the person* → Set PIN.**

The PIN is stored the same way a password is — hashed. Nobody can read it back,
including you: if somebody forgets theirs, set a new one. Clearing a PIN stops
that person using the kiosk immediately.

## What one tap does

Enter the PIN and press Enter. The kiosk clocks the person **in** if they were
out, and **out** if they were in, then shows their name, which way they went,
and their hours so far today. The message clears after a few seconds so the next
person does not see it.

A USB badge reader that types digits and presses Enter works with no extra
setup.

## Why the kiosk is safe to leave in a public place

This is the whole design, and it is worth understanding before you deploy one:

* **The kiosk is not a login.** The page carries no session and a successful
  punch does not create one. A PIN authorises exactly one action — clock that
  one person in or out — and nothing else.
* **It cannot list your staff.** There is no way to ask the kiosk who works
  here; it only ever answers about the person whose PIN was just entered.
* **Guessing is throttled.** Repeated wrong PINs from the same device are
  blocked for a few minutes, which is what makes a four-digit PIN defensible.

So a stolen or misused tablet is worth **one person's attendance record**, not
access to the company's data. That is the trade being made, and it is why the
kiosk must never be given a real login instead.
)MD"},

{"settings","Settings","","set-website","Website",60,"",""},

{"settings","Settings","set-website","set-website-pages","Building your public website",10,
 "website cms page publish draft slug menu seo sitemap robots block content public site",
R"MD(The system includes a small CMS for your public website: pages, a menu, and
search-engine metadata.

## Where it lives

Public pages are served under **`/site`** — your home page is `/site`, and a
page with the slug `about` is at `/site/about`.

They are not at `/` because `/` is the ERP itself. To put the website on your
real domain, point it at `/site` in nginx; nothing inside the system has to
change.

## Pages

**Settings → Website Pages.** A page has a **slug** (its URL), a **title**, and
its **content blocks**.

The slug may contain only lowercase letters, digits, hyphens and slashes —
`our-services`, `about/team`. It is the public address of the page, so anything
else is refused when you save.

## Content is blocks, not HTML

A page's content is a list of typed **blocks**:

| Block | Holds |
|---|---|
| `heading` | a heading, level 1–3 |
| `text` | paragraphs — a blank line starts a new one, a single newline is a line break |
| `image` | an image with optional caption |
| `button` | a labelled link |
| `columns` | two or more short titled sections side by side |
| `divider` | a horizontal rule |
| `html` | raw HTML, for an embed you cannot express any other way |

**You write text; the system writes the HTML.** That is deliberate: a website
page is content typed by one person and run in another person's browser, so
anything that let an author's text become markup would be a way to attack your
visitors. Because the blocks hold text, there is nothing to attack with.

The `html` block is the exception, for the occasional map or payment badge. It
is filtered against a list of permitted elements and attributes — scripts,
frames, event handlers and `javascript:` links are removed. Anything the filter
does not recognise is dropped rather than passed through.

## Publishing

A page is a **draft** until you tick **Published**.

A draft is not merely unlinked — to the public it returns the same "page not
found" as an address that has never existed, so nobody can probe your site to
discover what is coming. **While you are signed in you can open a draft to
preview it**, and a banner tells you the public cannot see it.

Unticking Published takes a page down again immediately.

## Menu

**Settings → Website Menu.** Each entry points at either one of your pages or
an external URL, and `Sequence` sets the order.

An entry pointing at an **unpublished page is hidden**, so your menu never
contains a link to a dead page.

## Search engines

Each page has a **meta title**, **description** and **keywords**, used for the
page's own tags and for the preview cards that appear when somebody shares the
link. Untick **Indexed** and the page tells search engines to ignore it and
drops out of your sitemap, while staying reachable to anyone with the address.

`/robots.txt` and `/sitemap.xml` are generated for you. The sitemap lists only
pages that are both published *and* indexed. `robots.txt` allows the public site
and **disallows the ERP, the customer portal and the kiosk** — none of those
should ever appear in a search result.

## Site-wide settings

Three configuration parameters set the frame around every page: the site name
shown in the header, the accent colour, and the footer line.
)MD"},

{"settings","Settings","set-website","set-website-edit","Editing the site by clicking on it",15,
 "edit inline wysiwyg website page block toolbar save publish permission designer",
R"MD(You can edit a page while looking at it, rather than through a form.

Open any page under **/site** while signed in. If you have permission, a bar
appears at the bottom with **Edit page**. Click it and the page becomes
editable in place:

* click any heading or paragraph and type;
* hover a block for its controls — **move up**, **move down**, **delete**;
* use **Add a block** at the foot of the page for a new heading, paragraph,
  image, button, columns, references block, map or divider;
* **Save** writes it and reloads the real page; **Cancel** discards.

## Who can edit

| | Sees the bar | Can save |
|---|---|---|
| A visitor | no | no |
| A customer on the portal | no | no |
| Staff **without** Settings / Configuration | no | no |
| Staff **with** Settings / Configuration | yes | yes |
| An administrator | yes | yes, **and only they can add a raw HTML block** |

Grant editing by adding a user to the **Settings / Configuration** group.

Being logged in is not enough on its own — editing the public site is a
configuration act, not an everyday one. And the permission is checked **on the
server when you save**, not just when the bar is drawn: hiding a button has
never stopped anybody, so the button and the check are separate things.

## Why you type text, not HTML

The editor changes the page's **blocks**, not its markup. Whatever you type is
stored as text and the system builds the HTML around it.

That is deliberate. A public page is written by one person and runs in another
person's browser, so anything that let typed text become markup would be a way
to attack your visitors. Because you are editing text, there is nothing to
attack with — and that stays true whether a page is edited here or through the
form in Settings.

The one block that does carry markup is **HTML**, which is why only an
administrator can add one, and why it is filtered even then.

## Every save is kept

You are editing a live public page, so a save is not final. **The previous
content is kept every time**, with who changed it and when, and you can put any
of them back.

* The last **20 versions** of each page are held.
* Saving without changing anything adds nothing to the list.
* **Restoring is itself a save** — the version you are replacing is kept too,
  so you can undo an undo.

Reading or restoring a version needs the same permission as editing, because
versions contain unpublished content.

## Checking the site for problems

`GET /site/api/health` returns everything currently wrong with the public site
— the same permission as editing, because the report names unpublished pages.

It looks for:

| Reported | Why |
|---|---|
| A published page with **no meta description** | Search engines invent the snippet instead |
| A **title over 60 characters** | Only about 60 are shown in a result |
| A published page in **no menu** | Reachable only by its address — often deliberate, so this is information, not a fault |
| A menu entry pointing at a **draft** | The entry is hidden from visitors, so the menu is shorter than you think |
| An image with **no alt text** | A screen reader and an image search both read it |
| A button linking to a **page that is not published** | A dead link on a live page |
| A **post with no excerpt** | The news index shows only its title |
| A **form with no fields** | Nothing can be submitted |

Each is graded *error*, *warning* or *info*, with a count to match.

## What it does not do yet

No image uploading (give an image block a URL) and no drag-and-drop (use the
move buttons). The menu, the theme and a page's SEO fields are still edited
under **Settings → Website Pages**.
)MD"},

{"settings","Settings","set-website","set-website-forms","Forms, news and page blocks",20,
 "form contact enquiry submission field honeypot spam blog news post publish reference map",
R"MD(Three things you can put on a public page beyond text and images.

## Forms

**Settings → Website Forms.** Build a form, add its fields, then drop it on any
page with a `form` block naming its slug.

Each field has a **name** (the key the answer is stored under — lowercase
letters, digits and underscores), a **label**, a **type** (text, email, tel,
number, textarea, choice, checkbox) and a **required** flag.

Submissions arrive under **Settings → Form Submissions**.

### What the form will not do

A public form is the only place on your website where a stranger can write to
your database, so it is deliberately narrow:

* **Only the fields you declared are read.** Anything else in the submission is
  discarded — a form can never be tricked into writing to a field you did not
  put on it.
* Required fields, value lengths and number formats are checked **on the
  server**, not just in the browser.
* Every form carries a hidden **honeypot** field. A person never sees it; a bot
  fills it, and that submission is silently dropped — the sender still gets a
  "thank you", so a bot learns nothing from the difference.
* Submissions are **rate-limited per visitor**. Note that failed attempts count
  too, and everyone in one office shares an address, so the limit is set high
  enough not to catch a real group of people.
* An **inactive** form answers exactly as a form that does not exist, so
  nobody can probe your site for form names.

### Sending a submission somewhere

Set **Also create** on the form to `project.task` and each submission also
opens a task, with the answers in its description. That is the only destination
available on purpose — a public form must never be able to choose what it
writes to.

> There is no email notification yet, because the system cannot send email at
> all. Until it can, check the Submissions list.

## News

Set a page's **Kind** to *Post* and it becomes a news item: it gains a date, an
author and an excerpt, appears on **/site/blog**, and links back to that index.

* A **draft** post is invisible, like any draft page.
* A post with a **future date** is scheduled: published, but held off the index
  and out of your sitemap until the date arrives.

## Reference and map blocks

**References** lists customers or partners — a name, an optional note, an
optional logo.

**Map** shows a location. You supply a place name or address, not an embed
code: the system builds the map frame itself, sandboxed, against a fixed
provider. That is why there is no "paste your embed here" box — an embed field
would be a hole straight through the protections on every other block.
)MD"},

// ============================ REPORTING ============================
{"report","Reporting","","rep-use","Reading your data",10,"",""},

{"report","Reporting","rep-use","rep-views","Group, pivot and graph",10,
 "group by pivot graph chart aggregate sum count list view analysis kanban calendar",
R"MD(Most lists in the system can be reshaped without anyone building a report.

## Group by

Grouping collapses a list into totals. Group tasks by stage to see workload;
group timesheet entries by project to see where hours went; group invoices by
customer to see who owes what.

Numeric columns are summed per group, so the totals are computed by the database
rather than by eye.

## Pivot

A pivot puts one field down the side and another across the top, with a measure
in the cells — hours by person by month, sales by category by quarter. It answers
"how does A vary with B" in a way a grouped list cannot.

## Graph

Bar, line and pie over the same grouped data. Use bar to compare, line for change
over time, pie only when the parts genuinely make a whole.

## Kanban and calendar

**Kanban** shows records as cards in columns — good for anything with a status.
**Calendar** places records with dates onto a month grid. Days with many entries
show the first few and a "+N more", so a busy day stays readable.
)MD"},

{"report","Reporting","rep-use","rep-documents","Printed documents",20,
 "document template report print pdf letterhead layout invoice quotation",
R"MD(**Settings → Technical → Document Templates** control what printed documents look
like — invoices, quotations, orders.

Each company can carry its own letterhead, so documents printed while working in
one company do not go out with another's branding.

Documents print from the browser. Print at 100% scale; "fit to page" rescales the
layout and can push content off the paper.
)MD"},

// ============================ CONTACTS ============================
{"base","Contacts","","con-use","Contacts",10,"",""},

{"base","Contacts","con-use","con-partners","Customers, vendors and companies",10,
 "contact partner customer vendor company address email phone",
R"MD(**Contacts → Contacts** is one list for everyone you deal with — customers,
vendors, and often both at once.

A contact is not permanently a "customer" or a "vendor". The same company can buy
from you and sell to you, and forcing a choice would mean maintaining them twice.

## What a contact carries

Name, addresses, email, phone, and the settings that affect documents: the
**pricelist** used when selling to them, and the **fiscal position** adapting
their taxes.

## The same list, filtered

**Accounting → Customers → Customers** and **Accounting → Vendors → Vendors**
show this same data filtered to the relevant side. Editing one edits the contact.

## Portal users

**Settings → Portal Users** gives a contact login access to their own documents
without giving them access to the system.
)MD"},

// ============================ SETTINGS ============================
{"settings","Settings","","set-admin","Administration",10,"",""},
{"settings","Settings","","set-data","Data and technical",20,"",""},

{"settings","Settings","set-admin","set-users","Users, groups and companies",10,
 "user group access rights company multi company switch permission login",
R"MD(## Users

**Settings → Users** manages logins. A user belongs to a company and to groups.

## Groups

**Settings → Technical → Groups** are what actually grant access. Rights come
from group membership, not from the user record, so the way to change what
someone can do is to change their groups.

## Companies

**Settings → Companies** defines each company: name, address, currency, and the
letterhead its documents carry.

## Working in more than one company

Records belong to a company, and you only see the companies you are allowed to.
This is enforced when reading, writing **and** deleting — not just in what the
list happens to show you.

Some records are shared deliberately and belong to no company; those are visible
to everyone. Products are commonly shared this way.

If a figure looks wrong, check which company is active before checking the data.
)MD"},

{"settings","Settings","set-admin","set-erp","ERP settings",20,
 "settings configuration parameter erp options preferences",
R"MD(**Settings → ERP Settings** holds system-wide options — the values that change how
the whole installation behaves rather than one record.

Change them deliberately. Unlike a record, there is no per-document history
showing what a setting used to be.
)MD"},

{"settings","Settings","set-admin","set-ai","The AI agent",30,
 "ai agent claude anthropic grok xai api key provider model token cap lookup",
R"MD(**Settings → AI Agent** configures the assistant used by **Products → Part
Lookup**. It is off until you configure it, and admin-only to read or change.

## Choosing a provider

| Provider | What it needs |
|---|---|
| **Anthropic** (Claude) | an `sk-ant-…` key from console.anthropic.com |
| **xAI** (Grok) | an `xai-…` key from console.x.ai |
| **Mock** | nothing — answers locally, never touches the network |

Each provider keeps **its own key and model**, so you can switch between them
without re-entering anything. Save the key, then **Test connection** — it makes
one minimal call and reports what it learned.

Choose the model to match the job: a fast small model is enough for a common
resistor; a larger one earns its cost on an obscure part.

## The key is stored in the database, and nowhere else

This is deliberate. The database is then the complete migration unit: move the
dump and the new machine has the configuration, the credentials and the data.
There is nothing to set up out of band.

The trade is real and worth stating plainly: **a database backup now carries a
live credential.** Anyone who can restore a dump can use the key. Treat backups
as secrets.

The key is **write-only** from the screen's point of view. Once saved it is
never sent back to the browser — you see only whether one is configured and its
last four characters. One exception exists, **Reveal for setup**, for when you
genuinely have to paste the key into a systemd unit or a Docker line. It is
admin-only and every use is written to the audit log.

The shared template dumps (`default-clean`, `default-demo`) have the key blanked
before capture, so they can be handed to somebody else safely.

## Anthropic workspace id

An identity-linked Anthropic key belongs to a workspace, and calls fail without
it. If **Test connection** reports a workspace problem, paste the id from the
Anthropic console and save.

## Web search

**Search the web** is on by default, and it is what makes a part lookup worth
running: with it off, the agent answers from training data and will still give
you a part number, a specification and a source URL it has never opened.

Searching costs more and takes longer — a lookup runs several searches and can
take the better part of a minute. Turn it off only if you understand that you
are then reading recollection rather than research.

**Candidates** sets how many answers a lookup may return (1–8, default 3). More
is useful for vague queries and wasteful for exact part numbers.

Not every provider searches the same way, and the vendors change it: xAI's
original search API was retired and now answers *"Live search is deprecated"*.
The endpoint and tool name are therefore stored as configuration, so a change
like that is a settings fix rather than a new build.

## Cost ceilings

**Max output tokens** bounds one answer. **Daily call cap** bounds the day, and
is checked *before* the call is made — a cap that noticed after the money was
spent would not be a cap.

Every feature shares the one cap: part lookups and help questions come out of
the same budget.

## Prompts — what the agent is actually told

**Settings → AI Agent → Prompts** shows the instructions sent for each job:
the part lookup, the help assistant, and the BOM column mapping.

They ship as files under `prompts/` — `part_lookup.md`, `help_assistant.md`,
`bom_headers.md`. They are **git-tracked**, so a deployment team changes how
the agent behaves the same way it changes anything else: edit, review, commit.
No rebuild.

### `{{placeholders}}`

`{{query}}`, `{{units}}`, `{{articles}}` and the rest are filled in when the
prompt is sent. The screen lists which ones each task has, because the *code*
supplies them — you cannot invent a new one by typing it.

A **required** placeholder cannot be removed. A part-lookup prompt with no
`{{query}}` in it asks the model about nothing at all, and the result looks
like a bad model rather than a bad edit, so saving is refused instead.

### Three sources, and the screen says which

| Badge | Means |
|---|---|
| **file** | the normal case — live from `prompts/` |
| **override** | somebody edited it here; the file is **not** in effect |
| **compiled** | the file is missing. A short built-in copy is keeping the feature alive — restore the file |

Editing here stores an **override in the database**; it does not write back to
the file. A process that rewrites its own git-tracked sources is a process
fighting whoever deployed it. Use an override to try something; move it into
the file to keep it. **Reset to file** drops the override.

### What a prompt cannot do

These are instructions to a model whose answers reach your catalogue, so
loosening a rule here loosens it for real data.

But the safeguards are **not** in these files and cannot be edited away: the
double-multiplier guard, the unit allowlist, the staging queue, and the rule
that the agent proposes parts rather than choosing them all live in the code.
That is a floor, not a licence.

## The help assistant

The same agent answers questions in **Help → Help Centre**, in the Assistant
panel on the right. It is **retrieval-augmented**: the manual's own articles are
retrieved and sent as the context, and the answer cites the articles it used, as
buttons that open them.

It deliberately does **not** search the web — the manual is the authority on how
this ERP works, and letting it browse would invite a confident answer about
somebody else's ERP.

Anyone signed in can ask; only an administrator can configure it. Asking the
manual a question is everybody's business, and the daily cap is what bounds the
spend.

## What the agent is allowed to do

**It cannot write to the catalogue.** It proposes; a person applies. Everything
it returns lands in the Part Lookup queue for review.

That containment is the point rather than a precaution. The agent reads vendor
pages and datasheets — untrusted text that can carry instructions aimed at the
model — so the staging queue is what stands between a hijacked answer and a
part somebody solders.

Answers are also checked before they are staged: a value whose magnitude was
written twice (`4k7` **and** `kΩ`) is corrected to the sensible reading and the
change is reported on screen. See *Values, units and notation*.
)MD"},

{"settings","Settings","set-data","set-dbtools","Database tools",10,
 "database browser sql query schema table inspect read only diagram",
R"MD(**Settings → Database Tools** is a read-only window onto the database, for when a
question cannot be answered from a screen.

Three tabs:

- **Browse** — tables, their columns and their rows.
- **SQL** — run a query and see the result.
- **Schema** — how the tables relate.

## It cannot change anything

The console runs inside a **read-only transaction**. That, not a list of banned
words, is what makes it safe: an `UPDATE`, a `DELETE` or a sequence call is
refused by the database itself, so there is no clever phrasing that gets around
it.

Queries are also time-limited, so a mistake cannot lock up the system.

## Hidden columns

Passwords, tokens and similar columns are masked. The tool is for understanding
your data, not for extracting credentials.

Admin only.
)MD"},

{"settings","Settings","set-data","set-backups","Backups and demo data",20,
 "backup restore database dump demo data seed sample",
R"MD(## Backups

**Settings → Database & Backups** creates and lists database backups.

A backup you have never restored is a hope, not a backup. Test a restore
somewhere harmless before you need one.

## Demo data

**Settings → Technical → Demo Data** loads sample records so screens can be
explored with something in them.

For the parts catalogue there is a separate seeder:

    ./scripts/seed_demo_parts.sh            # load
    ./scripts/seed_demo_parts.sh --clean    # remove

Everything it creates carries the internal reference prefix `DP-`, so demo parts
are always recognisable and always removable.
)MD"},

};

static constexpr int kHelpSeedCountB = sizeof(kHelpSeedsB) / sizeof(kHelpSeedsB[0]);

} // namespace cerp::modules::help

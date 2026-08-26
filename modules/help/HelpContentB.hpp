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

namespace odoo::modules::help {

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
| `1R2` | 1.2 |
| `100n` | 0.000 000 1 |
| `125m` | 0.125 |

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
 "lookup agent proposal review apply reject staging import datasheet distributor",
R"MD(**Products → Part Lookup** is a review queue for parts proposed by an external
agent — something that reads distributor sites and datasheets and suggests
catalogue entries.

## Nothing arrives automatically

A proposal is **staged**, never applied. It sits in the queue until a person
approves it. An agent that browses the web will sometimes be confidently wrong,
and a wrong resistance that lands silently in the catalogue becomes a part
somebody solders.

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
corrected.

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

} // namespace odoo::modules::help

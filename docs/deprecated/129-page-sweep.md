> **See also `docs/128`.** This document exists because of a bug introduced
> there, and records what was missing that let it through.

# 129 — A smoke test that opens every screen

---

## 1. The miss

A one-line change to `altViews` — the getter every generic list depends on —
read the view's field **map** as a list:

```js
const fields = this.state.listView?.fields || [];   // it is {name: {type}}
const has = (types) => fields.some(f => types.includes(f.type));
```

`{}.some` is `undefined`. Calling it threw, OWL swallowed the exception, and
**every list view in the ERP rendered blank** — no rows, no switcher, no
console error to say why.

The whole suite passed. That is the part worth sitting with: over a hundred
suites, thousands of assertions, and not one of them opened a screen. They
drive the HTTP API, and the API was perfectly healthy. The damage lived
entirely in the browser.

It was found by a person clicking around and saying "all the other pages don't
seem to have anything".

Two things made it worse than an ordinary bug:

* **It was silent.** No stack trace reached the console, so the only signal was
  a white rectangle.
* **My first two diagnostics misread it.** I clicked an app tile, saw the app's
  own "Select a menu item to get started" landing page, and concluded first
  that everything was broken and then — after fixing it — that the fix had not
  worked. Neither reading was right. Only walking to an actual leaf settled it.

## 2. The test

`tests/integration/core/page-sweep/` walks the menu the way a person does:
every app tile, every entry on every app's bar, every child of every dropdown.
**115 screens.** For each it asserts the screen put something on the page and
logged nothing to the console.

It is **deliberately shallow**. It does not check *what* a screen shows, only
that it shows something, because the failure it guards against is a blank
screen. A deep assertion per screen would be a second copy of the rest of the
suite and would rot within a month.

Three signals, because they are different failures:

| | |
|---|---|
| `apps ≥ 8` | the app grid itself rendered |
| `visited > 40` | the sweep actually walked — a sweep that visits nothing is a broken sweep, not a clean product |
| `blank == []` | a render that produced nothing |
| `errored == []` | a render that threw on the way |

That second one matters. Without it, a sweep that failed to find the menu would
report zero blank screens and pass.

## 3. It was verified against the bug it exists for

A test that has never failed is a test nobody knows the meaning of. The
regression was reintroduced deliberately and the sweep was re-run:

```
FAIL  no screen rendered blank
      47 screens: Accounting → Journal Entries, Contacts → Contacts,
      Products → Products, Inventory → Deliveries, …
FAIL  no screen logged a console or page error
      4 screens: Settings → Companies, Rental → Contracts, …
```

Then the fix was restored and it went green again: **115 screens, 0 blank, 0
errors.**

The fix itself accepts either shape, because the cost is two lines and the
failure mode was a silent white screen:

```js
const raw  = this.state.listView?.fields;
const defs = !raw ? [] : (Array.isArray(raw) ? raw : Object.values(raw));
```

## 4. Where it runs

Ordinary member of the standard suite — `tests/integration/core/page-sweep`,
order `0095`, `scenario=baseline`. `./tests/run.sh` picks it up like any other.
It skips cleanly where Chrome or puppeteer is absent, so a machine without a
browser still runs the rest.

Roughly four minutes, which is why it sits near the end rather than in the fast
path.

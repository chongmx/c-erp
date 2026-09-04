# 101 — The Help Centre

Status: **done**. `./scripts/run_tests.sh` → 74 passed, 0 failed
(`verify_help.sh`, 43 checks).

The Project app shipped in docs/100 without anywhere explaining how to use it.
This is that place — built so every other module can move in later, and so an AI
assistant can answer from it.

---

## 1. Help is data, not pages

One row per article: `book`, `slug`, `title`, markdown `body`, `keywords`,
`parent_id`, `sequence`. A **section** is a row with an empty body and
`is_section = true`; articles point at their section.

That shape is chosen for the assistant, not for the renderer. To answer a
question and **cite** where the answer came from, the content has to be
addressable in pieces — retrievable, rankable, quotable. A folder of HTML pages
could be displayed but never searched or cited.

**The slug is the public address.** It is what a deep link points at and what an
assistant will hand back ("see *Filling in your timesheet*"). A unique index
enforces it; the tests assert uniqueness, URL-safety and that nothing is
orphaned.

Shipped content **upserts on slug** at every start, so a corrected article
reaches installs that already have the old text. Anything an operator writes has
a slug that is never seeded, so it is left alone.

## 2. The screen

```
[ tabs — one per ERP module, scrolls sideways ]
┌──────────┬────────────────────────────┬──────────────┐
│ contents │ the article                │ assistant    │
│ (tree)   │                            │ (right rail) │
└──────────┴────────────────────────────┴──────────────┘
```

**Tabs are modules and the bar scrolls horizontally.** Measured on the live page:
`scrollWidth 1449` in a `clientWidth 1318` viewport — already 131px hidden with
14 tabs, and that list only grows. Wrapping onto a second row would push the
article down every time the system gains a module. `‹ ›` buttons are the
affordance, and they disable at each end.

Every module gets a tab **whether or not its help is written**. An undocumented
module shows a dash and an empty state; a module silently missing from the bar
would read as a bug rather than as information about what is missing.

**Both rails resize and collapse.** Drag the inner edge (a 7px hit area over a
hairline); widths and collapsed state persist in `localStorage`. Collapsed is a
34px strip, not `display: none` — the reopen button has to stay reachable.

**Deep links.** The article is written into the hash as `&help=<slug>` with
`history.replaceState`, which updates the address bar without firing
`hashchange` and sending the app's router somewhere else.

## 3. Markdown is rendered, not stored

Bodies stay plain markdown so the assistant can quote them. `HelpCenter.markdown`
handles headings, tables, fenced and indented code, blockquotes, lists, inline
code, bold/italic and links.

**Everything is HTML-escaped first and formatted afterwards**, so stored content
can never inject markup — bodies are data, and data never becomes tags. Only
`http(s)` links are linkified, so a `javascript:` URL cannot reach an `href`.

## 4. The assistant rail

Not connected. The panel is present and the retrieval step behind it already
works: `related` returns the articles nearest to whatever you are reading, which
is the same lookup an assistant would run before answering. Wiring a model in
means replacing the disabled textarea, not rebuilding the screen.

## 5. Bugs found

1. **The OWL template would not compile.** My section separators were
   `<!-- ------- left ------- -->`, and `--` is illegal inside an XML comment.
   OWL parses templates as XML, so the whole component died at mount. Same
   family as the NUL byte in docs/093 and the control character in docs/095;
   `verify_help.sh` now asserts no template comment contains `--`, no control
   characters are present, and the template parses as XML.
2. **`related` returned nothing** for articles with distinctive keywords —
   "walkthrough", "scenario" share no words with their neighbours. An empty
   panel reads as broken, not as "nothing related". It now falls back to the
   article's section, then its book. The test walks **every** article and fails
   if any has an empty panel.
3. **A bullet's wrapped line broke out of its list** into a stray paragraph.
   Indented continuation lines now fold into the item above, kept to 1–3 spaces
   so a 4-space indent is still a code block.
4. **An unknown slug returned "An internal error occurred."** It was thrown as
   `std::runtime_error`, which SEC-28 correctly masks. A missing article is an
   ordinary not-found and a slug is public, so it is a `ValidationError` now —
   masking is right for a leaked SQL message, useless for a dead link.

## 6. An unrelated failure this surfaced

`verify_bank_recon.sh` began failing, and it was **not** caused by this work.

The database has accumulated **43 posted invoices with no move lines**, residual
exactly 100, against the first partner, dated today — leaked since 2026-08-15 by
other scripts. `suggest_matches` orders by
`(amount_residual = $1) DESC, (partner_id = $2) DESC, date LIMIT 20`, so with
enough identical invoices **every tie-break ties**, the order falls back to
arbitrary, and the fixture's own invoice — the newest — fell off the end of the
20.

Fixed by giving the fixture a distinctive amount (`1373`) so it wins the
exact-amount match outright. The assertion now depends on reconciliation rather
than on what else is in the database.

> **The leak itself is not fixed.** Something in
> `verify_fx_settlement`, `verify_money_roundtrip`, `verify_multicompany_*` or
> `verify_payment_allocation` creates bare `account_move` rows and does not
> remove them. Worth a sweep — a suite that slowly poisons its own database will
> keep producing failures that look like feature bugs.

## 7. Not done

- Only **Project** and **Using Help** have content. Twelve tabs are empty.
- No editing from the Help Centre itself; articles are edited through
  **Help → Help Articles** or by adding rows to `HelpContent.hpp`.
- Search is `ILIKE` with a title/keyword/body ranking, not full-text indexed. Fine
  at this size, wrong at ten times it.
- No screenshots or images in articles.

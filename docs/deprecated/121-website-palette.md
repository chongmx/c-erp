# 121 — The site palette

---

## 1. What was wrong

The public site had **one** configurable colour. The entire stylesheet hung off
a single `:root` block:

```css
:root{--a:<accent>;--ink:#16202a;--mut:#5d6f7e;--line:#e2e8ee;--bg:#fff}
```

Five tokens, four of them literals. `website.accent` in `ir_config_parameter`
was the only thing an owner could move, and there was no UI for it — you
changed it with SQL. So every site built on this was black on white, and the
accent was doing all of the visible work by itself.

That is a highlight colour, not a palette.

Two smaller things were wrong in the same block:

* **`line-height:1.２`** — a fullwidth Unicode `２` (U+FF12), not an ASCII `2`.
  Invalid CSS, so the declaration was dropped and every `.w-h` heading
  inherited the body's `1.65` instead of `1.2`.
* **`color:#fff` hardcoded** on buttons, the pricing badge and the step
  markers. White text on the accent is an assumption, and it fails silently
  the moment somebody picks a pale one.

---

## 2. What it is now

A palette is a **preset** plus **overrides**.

A preset carries a complete, deliberately-paired set of tokens for *both*
schemes — a light site still has to answer a visitor whose OS is in dark mode,
and picking those tokens by inverting the light ones gives muddy text on a
washed ground. Any single token can then be overridden without restating the
other nine.

| Preset | Light ground | Character |
|---|---|---|
| `paper` | `#ffffff` | the original look; the fallback |
| `slate` | `#f6f8fa` | grey ground, white cards — depth without colour |
| `sand` | `#faf7f3` | warm neutral |
| `midnight` | `#0e151d` | **committed dark** — its light scheme *is* its dark scheme, so it stays dark for every visitor |
| `contrast` | `#ffffff` | pure black on white, heavy rules |

### Tokens

**Base:** `--bg` ground · `--surface` cards, panels, form fields · `--ink` body ·
`--mut` secondary · `--line` rules · `--a` accent · `--on-a` text on the accent

**Derived, per scheme:** `--a-tint` (~8% accent over the surface — a card wash)
· `--a-tint2` (~16% — a band, a hovered row) · `--a-soft` (~12% over the
*ground* — the hero) · `--a-deep` (accent darkened — gradient end, hover) ·
`--a-rule` (accent mixed into the border colour) · `--a-text` (the accent
adjusted until it is **readable** on the ground).

`--surface` is new. Cards used to be transparent, so they could only ever be
the same colour as the page. That is why `paper` looks flat and `slate` does
not.

The derived tones are computed **per scheme**, not written once. An 8% wash
over `#ffffff` is nearly white; the same wash over `#0e151d` is nearly black.
Sharing them would give a dark site a light site's tints.

`--a-text` deserves its own note. A brand accent is picked to sit *behind*
white text on a button; that does not make it readable *as* text on the page.
`#e94560` on white is **3.83:1** — below AA. `readableOn()` nudges it toward
the ground's opposite in 5% steps and stops at the first one that clears 4.5:1,
keeping as much of the brand hue as the ratio allows. On a dark ground it
lightens instead. An accent that already passes is returned untouched.

### Configuration keys

| Key | Meaning |
|---|---|
| `website.theme` | preset key; unknown → `paper` |
| `website.accent` | hex; empty → the preset's own accent |
| `website.on_accent` | hex; empty → **computed** (below) |
| `website.dark_mode` | `auto` (follow the visitor) / `off` / `on` |
| `website.color.{bg,surface,ink,muted,line}` | light-scheme overrides |
| `website.color.dark.{…}` | the same five for dark |
| `website.login_link` | `off` removes the top-bar Sign in link |

All of it lives in `ir_config_parameter`, so it travels with a `pg_dump` like
everything else.

### `--on-a` is computed, not assumed

`onColor()` picks near-black or white by whichever gives the better WCAG
contrast ratio on the accent. On Easy Locker Space's `#e94560` that is
**near-black at 4.85:1**, against **white at 3.83:1** — so the old hardcoded
white was the *less* readable of the two choices, and below AA.

This changes how existing sites look, which is why `website.on_accent` exists:
an owner whose buttons have always been white-on-pink can pin it and own the
consequence, rather than being argued with by a stylesheet. The editor exposes
it as **Text on accent: Automatic / White / Dark**.

---

## 3. Where the colour actually goes

Tokens on their own change nothing — the old stylesheet would have used two of
them. These are the places the design now spends colour:

* **Top bar** — sticky, on `--a-tint`, closed with a 3px `--a` → `--a-deep`
  gradient rule. The brand gets a small gradient mark; nav links grow an accent
  underline on hover.
* **Hero** — a `--a-soft` → `--bg` wash in a rounded card that bleeds past the
  text column, with a rule-and-caps eyebrow.
* **Section headings** — a short accent bar above each `h2`.
* **Featured pricing card** — a `--a-tint2` → `--a-tint` → `--surface`
  gradient, an accent cap along the top edge, and the price itself in
  `--a-text`. Cards lift on hover.
* **Steps** — the numbered discs are joined by an `--a-rule` trail, so they
  read as a sequence rather than as bullets that happen to have numbers.
* **FAQ** — one bordered card; the open row takes the tint and its heading
  `--a-text`; the marker is an accent disc.
* **Footer** — a tinted band closed with a 3px accent rule.

### The badge that got clipped

The featured card's cap was first done with `overflow:hidden` and a `:before`.
That clipped the **MOST POPULAR** badge, which overhangs the top edge on
purpose. The cap is now clipped to the card's own radius instead
(`border-radius:12px 12px 0 0`, inset by `-1px`) and the badge carries
`z-index:2`. Caught by looking at the screenshot, which is the only thing that
catches it.

---

## 4. The staff sign-in link

The top bar now carries a **Sign in** link to `/`.

docs/120 argued for leaving it off: advertising the admin door to every visitor
and crawler buys nothing, and a link makes credential-stuffing easy to aim.
The owner asked for it, which is their call — so it ships, `rel="nofollow"` so
it is not an invitation to index, and `website.login_link=off` removes it again
without a rebuild.

What actually defends that door is unchanged and does not depend on the link
being hidden: the login endpoint's rate limiting, and the uniform "Invalid
credentials" that refuses to say whether a login exists (docs/120 §1). The
tests assert that showing the link grants an unauthenticated caller nothing.

---

## 5. The UI

A **Theme** button in the editor bar opens a panel: preset swatches, an accent
colour input, the on-accent choice, and dark mode. Apply reloads, because the
palette is in the page's `<style>` block.

It has exactly the standing the rest of `website-editor.js` has — a
convenience, not a control. `POST /site/api/theme` re-checks the session's
groups and re-validates every colour, so forcing the panel open in a browser
that is not allowed to use it produces a 403 and nothing else. Same gate as
editing a page: **Settings / Configuration**, not merely a staff login.

The ten per-token overrides are deliberately **not** in the panel. A form with
ten hex fields is a way to build an unreadable site quickly; the presets exist
so nobody has to. The API accepts them for the case that actually needs one.

---

## 6. Security

Every value here ends up inside a `<style>` block on a page anyone can reach,
so the rules are the same as for the CMS sanitiser.

* **Colours are validated as hex literals**, `#rgb` or `#rrggbb`, on the way in
  *and* re-validated on the way out. `red;}body{display:none` is not a colour.
* **The preset key is allowlisted** against the preset table — it is never
  interpolated anywhere, but the same discipline as S-49 applies: a name
  reaching a lookup is checked against what exists, not against a charset.
* **`dark_mode` is a closed set** of three strings.
* **The `colors` object is allowlisted to ten token names.** `ir_config_parameter`
  holds settings for the whole ERP; an unfiltered `{key: value}` write here
  would have been a write primitive for every one of them. The test proves
  `web.base.url` cannot be reached through it.
* **Refusals are atomic.** Writes are collected and validated in full before
  any of them is applied, so a request with one good field and one bad one
  leaves nothing behind. Tested directly.
* **An empty token is treated as an injection would be.** `--bg:;` paints the
  site out as effectively as anything hostile, so the tests assert no empty
  declaration can be produced.

---

## 7. Tests

**Unit — `tests/unit/website/test_palette.cpp`, 173 assertions.**

Two jobs that fail in different directions. The first is the validator, tested
the way the sanitiser is: the whole catalogue, including `#fff;}body{...}`,
`rgb()`, `var(--x)`, an embedded newline, and fullwidth digits — the last
because that is exactly the bug found in §1.

The second is **readability, which no HTTP test can check.** A preset whose
muted text sits at 3:1 on its own ground renders perfectly and returns 200.
Contrast is arithmetic, so it is asserted rather than eyeballed: body text
clears **AAA (7:1)** on both its ground *and* its surface, secondary text
clears **AA (4.5:1)** on both, in **both schemes**, for **every preset**.

**Integration — `tests/integration/website/theme/`, 53 checks.**
Config → resolved tokens → the bytes a visitor receives. Presets reach the
stylesheet; the accent override survives; `auto` emits exactly one
`prefers-color-scheme` block and `on`/`off` emit none; eight bad payloads are
refused and the palette is byte-identical afterwards; an ordinary employee gets
403 and the site is unchanged after their attempt.

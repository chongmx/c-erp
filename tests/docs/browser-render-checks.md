# Browser render checks

How to put a screen in front of a real browser, what works today, and the four
facts about this app that cost an afternoon to rediscover.

Read this before writing any test that claims a screen "works".

---

## 1. Why this exists

**A passing API test tells you nothing about whether the screen renders.**

The frontend is OWL, and its templates are parsed as XML at load time in the
browser. A malformed template — a stray control character, a `--` inside an XML
comment, a typo in a `t-att` expression — throws in the client and renders
*nothing*. The server logs nothing, every RPC still returns 200, and the whole
integration suite stays green while the user looks at a blank panel.

This has happened here more than once. It is the single largest gap between
"the tests pass" and "the feature works".

So: an API test proves the data is right. Only a browser proves the screen is.

---

## 2. What is installed — and how to use it

| | |
|---|---|
| **Google Chrome** | `/usr/bin/google-chrome` (also `/opt/google/chrome/chrome`) |
| **puppeteer-core** | installed as a dev dependency, drives the Chrome above |
| **node** | WSL's `/usr/bin/node` (v18). Note `npm` resolves to the **Windows** npm — installs still land in `./node_modules` and work |

**Just use the harness:**

```bash
node tests/lib/render.mjs Products Configuration Categories .ct-shell
```

Arguments are the **menu path to click**, then the selector that proves the
screen arrived. It prints a JSON report — rows rendered, sidebar width, whether
the detail panel filled in, and any browser console errors — writes a
screenshot to `$SHOT` (default `/tmp/render.png`), and **exits non-zero if the
selector never appeared or the console reported anything**.

A worked example is `tests/integration/product/category-tree/test.sh` §9, which
skips with a NOTE rather than failing when puppeteer-core or Chrome is absent.

> **puppeteer-core is ESM-only.** `require('puppeteer-core')` throws
> `ERR_REQUIRE_ESM`. Use `await import('puppeteer-core')` from a `.mjs` file.

---

## 3. The four facts that cost time

Every one of these produced a confusing wrong answer before it was understood.
None is guessable.

### 3.1 The app is served at `/`, and `/web` is a 404

`http://127.0.0.1:8069/web` returns a **Drogon 404 page**, not the app. The
first render attempt screenshotted that 404 and looked like a rendering bug.

The SPA is served at `/`, `/index.html`, and `/web/login` — all three return the
same `index.html`.

### 3.2 Static files are registered at boot

A file added under `web/static/` **404s until the server restarts.** Adding a
component and immediately loading it will fail for a reason that has nothing to
do with the component.

```bash
pkill -x c-erp; sleep 2
(setsid ./build/c-erp > /tmp/cerp_run.log 2>&1 < /dev/null &)
```

### 3.3 There is no hash router — screens are reached by CLICKING

From [app.js](../../web/static/src/app.js), in `MainApp.setup`:

> *"There is no hash router in this app — screens are reached by clicking menus
> — so `location.hash = '#action=...'` did nothing at all. Three screens used it
> to open a record and silently failed."*

So `http://127.0.0.1:8069/#model=product.category` **lands on the home tiles**,
not on Categories. This is the reason a `--dump-dom` check cannot currently
reach most screens: it has no way to click Products → Configuration →
Categories.

The one programmatic hook is registered on the window:

```js
window.ErpNav.openRecord(model, recordId)   // opens a record's FORM view
```

That opens a form — it will **not** reach a custom component registered against
a model in the `CUSTOM_VIEWS` map, because those replace the *list* view.

### 3.4 The session is cookie-only, and Chrome's CLI cannot set a cookie

`JsonRpcDispatcher::resolveSessionId_` accepts a session from exactly two
places: the `session_id` **cookie**, or `kwargs.context.session_id` on an RPC
call. There is no query-parameter fallback, and no HTTP basic auth.

Chrome has no `--cookie` flag. The way around it is a **same-origin bootstrap
page** that logs itself in with `fetch`, plus a persistent profile:

- the page must be served from the app's own origin, or the fetch is cross-site
  and the `SameSite=Lax` cookie is refused;
- it must live under a **real static route** — `web/static/src/` is served at
  `/src/...`. A file at `web/static/foo.html` is *not* reachable: unknown paths
  fall through to `index.html`, so you get the SPA back and your page never
  runs;
- the session cookie carries `Max-Age=3600`, so it is **persistent** and
  survives to disk in `--user-data-dir` — which is what lets a second Chrome
  invocation start already logged in.

---

## 4. The recipe that works today

Two Chrome runs sharing one profile. One run does not work: the in-page
navigation is still settling when `--virtual-time-budget` expires, and the dump
comes back as a near-empty document.

**Step 1 — the bootstrap page**, written to `web/static/src/_render_probe.html`
(and deleted afterwards; restart the server after creating it):

```html
<!doctype html>
<meta charset="utf-8">
<body>
<div id="probe-status">booting</div>
<script>
(async () => {
    const st = document.getElementById('probe-status');
    const r = await fetch('/web/session/authenticate', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        credentials: 'same-origin',
        body: JSON.stringify({ jsonrpc: '2.0', method: 'call', params: {
            db: 'odoo', login: 'admin', password: 'admin' } }),
    });
    const j = await r.json();
    // The id is nested under `result`. Checking only j.session_id reports a
    // login failure on a login that worked.
    const sid = j && (j.session_id || (j.result && j.result.session_id));
    st.textContent = sid ? 'probe-login-ok' : 'probe-login-failed';
})();
</script>
</body>
```

**Step 2 — drive it:**

```bash
PROF=/tmp/cprof; rm -rf "$PROF"; mkdir -p "$PROF"
CHROME="google-chrome --headless=new --no-sandbox --disable-gpu \
        --window-size=1400,900 --user-data-dir=$PROF"

# 1. log in — this writes the cookie into the profile
timeout 90 $CHROME --dump-dom --virtual-time-budget=8000 \
    'http://127.0.0.1:8069/src/_render_probe.html' > /tmp/probe.html
grep -q probe-login-ok /tmp/probe.html || echo "login failed"

# 2. load the app, already authenticated
timeout 120 $CHROME --dump-dom --virtual-time-budget=25000 \
    'http://127.0.0.1:8069/' > /tmp/dom.html
timeout 120 $CHROME --screenshot=/tmp/shot.png --virtual-time-budget=25000 \
    'http://127.0.0.1:8069/'
```

This is verified to work: the screenshot shows the app logged in as `admin`.

**What it can do:** confirm the app boots, is authenticated, and that the home
screen renders; screenshot any screen reachable without a click.

**What it cannot do:** reach a screen behind a menu (§3.3), read the browser
console, wait on a selector, or interact. That is the whole gap.

---

## 5. The driver — what `tests/lib/render.mjs` does

Installed with `npm i -D puppeteer-core`. Use **`puppeteer-core`**, not
`puppeteer` — it skips the ~150 MB Chromium download and drives the Chrome
already installed:

```js
const puppeteer = require('puppeteer-core');
const browser = await puppeteer.launch({
    executablePath: '/usr/bin/google-chrome',
    args: ['--no-sandbox', '--disable-setuid-sandbox'],
});
```

With a driver the login needs no bootstrap page — set the cookie directly:

```js
const sid = /* from a normal /web/session/authenticate call */;
await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
```

…and the menu click becomes possible, which is what actually unlocks the
screens:

```js
await page.goto('http://127.0.0.1:8069/', { waitUntil: 'networkidle2' });
await page.click('text/Products');          // then Configuration -> Categories
await page.waitForSelector('.ct-shell', { timeout: 15000 });
```

**Poll, never sleep.** OWL flushes renders on `requestAnimationFrame`; a fixed
`sleep` races it and fails intermittently, which is worse than failing always.
`waitForSelector` is the correct tool.

Collect errors, or the check is half-blind:

```js
page.on('pageerror', e => errors.push('pageerror: ' + e.message));
page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
```

A render test should assert `errors.length === 0` — an OWL template failure
shows up there and nowhere else.

### What it found the first time it ran

Worth recording, because it is the whole argument for this file. The Categories
screen passed 59 API and wiring checks. Then the first screenshot showed the
tree was **invisible**: the stylesheet used `var(--bg-soft, #fafafa)` and
`var(--accent-soft, #e6f0fb)`, and **neither token exists**. Every fallback
painted a light surface while the text kept inheriting the dark theme's
near-white `--text` — white labels on a white sidebar, and two icon buttons
rendered as empty boxes.

Nothing failed. No error, no console warning, every assertion green. It was
only visible in a picture.

The real tokens are `--bg`, `--surface`, `--border`, `--accent`, `--text`,
`--muted` (defined at the top of `app.css`). **Check a token exists before
relying on its fallback** — a CSS fallback is silent by design.

---

## 6. The static checks — useful, but not proof

These are cheap and worth keeping alongside a render check. They catch the
common wiring failures early. They do **not** prove anything renders — the
Categories screen passed every one of them while being invisible.

From `tests/integration/product/category-tree/test.sh` §8:

| Check | Catches |
|---|---|
| `http_code /src/components/X.js` is 200 | the file is not served (usually: server not restarted) |
| `index.html` contains the `<script src=…>` | the component is never loaded |
| `app.js` contains the `'model': Component` mapping | loaded but never reached |
| `app.css` contains the screen's root class | unstyled screen |
| `node --check` on the component and `app.js` | a JavaScript syntax error, which takes the whole frontend down |
| no `<!-- … -- … -->` in the file | `--` inside an XML comment stops OWL parsing the template |
| every CSS class appears in both template and stylesheet | a rule that never applies, or an element with no styling |

The last one is cheap and surprisingly effective: it catches renames that
update one file and not the other.

---

## 7. Rules

- **Every screen test gets a render check.** `node tests/lib/render.mjs <menu
  path…> <selector>`. If you skip it, say in the test output that rendering was
  not verified.
- **Look at the screenshot at least once.** Assertions confirm elements exist;
  only a picture shows white-on-white, a collapsed column, or a control that
  renders as an empty box.
- **"It exists" is not "it is visible." Hit-test it.** An element clipped away
  by an ancestor's `overflow: hidden` still reports a real `offsetHeight`, real
  child rows and a real bounding box, and passes every DOM assertion you can
  write about it — while the user sees nothing at all. That is exactly what
  `.o2m-table { overflow: hidden }` did to every dropdown opened in a line
  table. Take the point where the content is drawn and ask the page what is
  actually there:

  ```js
  const r = opt.getBoundingClientRect();
  const hit = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
  const visible = !!(hit && (hit === opt || opt.contains(hit)));
  ```

  Check the rect is inside the viewport too — an element can be painted
  perfectly and still be off screen.
- **Screenshot BEFORE you interrogate the page.** Every CDP round trip is a
  chance for headless Chrome to blur the page, and a blur closes anything that
  hides on blur. An "is the dropdown open?" `evaluate()` placed before the
  capture is what closes the dropdown, and the picture comes back empty while
  the assertion insists it was open.
- **Check a CSS token exists before relying on its fallback.** `var(--nope,
  #fff)` is silent and will happily fight the theme.
- **Restart the server after adding any file under `web/static/`.**
- **Put render scaffolding under `web/static/src/`, and delete it afterwards.**
  A probe file left behind is served to real users.
- **Poll for the thing you expect; never sleep a fixed amount.**
- **Assert the browser console is clean.** A screen that renders with an
  exception is not a screen that works.

# 126 — The website is the front door

---

## 1. The change

| | before | after |
|---|---|---|
| `/` | the ERP login form | **the website homepage** |
| `/login` | — | **the ERP application** |
| `/site`, `/site/<slug>` | the website | unchanged |

`/site` keeps working. Every stored menu row, sitemap entry, share link and
bookmark already points there, and breaking them to reclaim a path would be a
poor trade.

The application shell's path is configuration, not a constant:
`HttpConfig::appPath`, defaulting to `/login`. `index.html` references its
assets with absolute paths (`/lib`, `/src`), so it loads correctly from any
path this is set to.

### An ERP-only installation still works

Not every deployment has a website. If nothing is published as the homepage,
`/` issues a **302 to `/login`** rather than answering the site's own domain
with a 404. Asserted in the theme suite.

---

## 2. The palette now matches the application

A new preset, **Backend** (`console`), taken from `web/static/src/app.css`
`:root` rather than approximated:

| token | value |
|---|---|
| `--bg` | `#1a1a2e` |
| `--surface` | `#16213e` |
| `--line` | `#0f3460` |
| `--accent` | `#e94560` |
| `--ink` | `#eaeaea` |
| `--mut` | `#8899aa` |

Committed dark, like the backend — its light scheme *is* its dark scheme. It
clears the same contrast floors as every other preset: body text at AAA,
secondary at AA, on both ground and surface, and every derived accent tint
along with them (358 unit assertions).

---

## 3. What this broke, and what it did not

### nginx — no change required

`deploy/nginx/c-erp.conf` ends in a catch-all `location /` that proxies to the
app, so `/` and `/login` both reach it with no new block.

The part worth being explicit about: **login rate limiting is unaffected.** It
is attached to `location = /web/session/authenticate` — the *API the form
posts to*, not the path that serves the form. Moving the page does not move
the endpoint, so the 10/min cap still applies.

### Two things that would have broken on the live domain

**`robots.txt` ended `Disallow: /`.** That was correct when `/` was the login
page and everything public lived under `/site`. With `/` as the homepage, that
single line would have excluded the site's front page from every search
engine — a regression nobody notices until the traffic goes. The blanket is
replaced by named prefixes: `/login`, `/web`, `/portal`, `/kiosk`, `/rental`,
`/site/api`, each of which is a route that actually exists.

**The homepage declared `/site` canonical.** Both paths serve it, so one has to
be named the real one — and pointing it at `/site` would tell search engines to
index `easylockerspace.com/site` as the homepage and treat the bare domain, the
URL on the business cards, as a duplicate. The homepage's canonical is now `/`,
and `sitemap.xml` was changed to match: a sitemap offering `/site` while the
page declares `/` is a contradiction crawlers resolve by ignoring one of them.

### What did not change

* Nothing became publicly readable that was not already. `/` served a static
  `index.html` before and a rendered public page now; neither carries data.
  Every API still refuses an unauthenticated caller — verified: `401` on
  `/site/api/theme`, `/site/api/media` and `/site/api/page/N/blocks`.
* A visitor at `/` receives no editor script, no `__WSITE_EDIT`, no toolbar
  markup and no session material.
* Session cookies are path-`/`, so they work at both paths without change.
* The portal, kiosk and rental routes are untouched.

### One test caught it

`integration/core/read-group` and `integration/core/db-tools` both did
`curl "$BASE/" | grep -q 'RecordViews.js'` — asserting the app shell answers at
the root. That is exactly the assumption this change breaks, and it failed
loudly on the first run. Both now fetch `/login`.

---

## 4. Before deploying to easylockerspace.com

**Set `web.base.url` to `https://easylockerspace.com`.** It defaults to
`http://localhost:8069` and feeds the canonical tag, every `<loc>` in
`sitemap.xml`, the `Sitemap:` line in `robots.txt`, and admin-issued password
reset links. Left at the default, a live site publishes canonicals and a
sitemap pointing at localhost.

```sql
INSERT INTO ir_config_parameter (key, value)
VALUES ('web.base.url', 'https://easylockerspace.com')
ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value;
```

Then check, in order: `/` renders the site · `/login` renders the sign-in card
· `curl -s https://easylockerspace.com/ | grep canonical` names the bare domain
· `robots.txt` does not disallow `/` · `sitemap.xml` lists the domain root.

#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# The site palette (docs/121).
#
# Before this, the public site had one configurable colour and four hardcoded
# ones, so every site was black on white. The feature is a preset plus
# overrides; the risk is that all of it lands inside a <style> block on a page
# anyone can reach.
#
# So this file asserts three different things:
#
#   §2–§4  it WORKS: a preset actually reaches the stylesheet, an accent
#          override survives, and dark mode means what it says.
#   §5     bad values are refused AND refused atomically — a request with one
#          good field and one bad one must not leave half a palette behind.
#   §6–§7  the same gate as page editing, and the config table does not become
#          a general write primitive via the colours object.
#
# The unit tier (tests/unit/website/test_palette.cpp) owns contrast and hex
# validation, where the whole catalogue costs microseconds. This tier owns the
# wiring: config → resolved tokens → the bytes a visitor receives.
# =============================================================
auth_or_die
ADMIN_SID="$SID"

acode() { # acode <sid> <method> <path> [body]
    if [ "$2" = "GET" ]; then
        curl -s -o /dev/null -w '%{http_code}' -H "Cookie: session_id=${1:-}" "$BASE$3"
    else
        curl -s -o /dev/null -w '%{http_code}' -X POST -H "Cookie: session_id=${1:-}" \
             -H 'Content-Type: application/json' --data "${4:-{\}}" "$BASE$3"
    fi
}
theme() { # theme <json> -> body
    curl -s -X POST -H "Cookie: session_id=$ADMIN_SID" -H 'Content-Type: application/json' \
         --data "$1" "$BASE/site/api/theme"
}
# Every assertion reads the stylesheet off a page THIS test published. A clean
# baseline has no published home page, so reading /site would 404 — and would
# only have worked on a working database that happened to have a site in it.
PAGE_PATH="/site/th-palette"
root() {  # the :root declaration a visitor actually receives
    curl -s "$BASE$PAGE_PATH" | grep -o ':root{[^}]*}' | head -1
}
darkroot() {
    curl -s "$BASE$PAGE_PATH" | grep -o ':root{[^}]*}' | sed -n '2p'
}
scheme_blocks() {
    curl -s "$BASE$PAGE_PATH" | grep -c 'prefers-color-scheme'
}

# The theme is global configuration, so put it back exactly as found.
SAVED=$(pg "SELECT string_agg(key || '=' || value, '|' ORDER BY key)
              FROM ir_config_parameter
             WHERE key LIKE 'website.theme' OR key LIKE 'website.accent'
                OR key LIKE 'website.on_accent' OR key LIKE 'website.dark_mode'
                OR key LIKE 'website.color.%'")
cleanup() {
    pg "DELETE FROM ir_config_parameter
         WHERE key IN ('website.theme','website.accent','website.on_accent','website.dark_mode')
            OR key LIKE 'website.color.%'" >/dev/null
    if [ -n "$SAVED" ]; then
        echo "$SAVED" | tr '|' '\n' | while IFS='=' read -r k v; do
            [ -z "$k" ] && continue
            pg "INSERT INTO ir_config_parameter (key,value) VALUES ('$k','$v')
                ON CONFLICT (key) DO UPDATE SET value=EXCLUDED.value" >/dev/null
        done
    fi
    pg "DELETE FROM website_page_revision WHERE page_id IN
          (SELECT id FROM website_page WHERE slug LIKE 'th-%')" >/dev/null
    pg "DELETE FROM website_page WHERE slug LIKE 'th-%'" >/dev/null
    pg "DELETE FROM res_groups_users_rel WHERE uid IN
          (SELECT id FROM res_users WHERE login LIKE 'th_%')" >/dev/null
    pg "DELETE FROM res_users   WHERE login LIKE 'th_%'" >/dev/null
    pg "DELETE FROM res_partner WHERE name LIKE 'TH %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

# ------------------------------------------------------------------
sec "0. a published page to read the stylesheet off"
# ------------------------------------------------------------------
BLK='[{"type":"heading","level":"1","text":"Palette"},{"type":"text","text":"Body copy."}]'
PID=$(call website.page create "[{\"slug\":\"th-palette\",\"title\":\"Palette\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$BLK"),\"is_published\":true}]" | rid)
t_nonempty "$PID" "a published page exists"
[ -z "$PID" ] && { # ------------------------------------------------------------------
sec "10. the front door — / is the website, /login is the ERP (docs/126)"
# ------------------------------------------------------------------
# Make this page the homepage so "/" has something to serve.
#
# is_homepage is GLOBAL state — exactly one row carries it — so the row that
# had it is remembered and handed back at the end. Without that, running this
# file against a working database (--no-baseline) would silently leave the real
# site with no homepage, and "/" would redirect to /login for everyone.
PREV_HOME=$(pg "SELECT id FROM website_page WHERE is_homepage=TRUE LIMIT 1" | tr -dc '0-9')
restore_home() {
    pg "UPDATE website_page SET is_homepage=FALSE WHERE is_homepage=TRUE" >/dev/null
    [ -n "$PREV_HOME" ] && \
        pg "UPDATE website_page SET is_homepage=TRUE WHERE id=$PREV_HOME" >/dev/null
}
trap 'restore_home; cleanup' EXIT
pg "UPDATE website_page SET is_homepage=FALSE WHERE is_homepage=TRUE" >/dev/null
pg "UPDATE website_page SET is_homepage=TRUE, is_published=TRUE, is_indexed=TRUE WHERE id=$PID" >/dev/null

ROOT=$(curl -s "$BASE/")
t_contains "$ROOT" 'w-brand'      "the bare domain serves the website"
t_lacks    "$ROOT" 'login-card'   "…not the login form"
t_contains "$(curl -s "$BASE/login")" 'id="app"' \
     "the application answers at /login"
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/site")" \
     "/site keeps working — every stored menu and link points there"

# The homepage is reachable by two paths, so exactly one of them has to be
# declared canonical, and it must be the one on the business cards.
t_contains "$ROOT" 'rel="canonical" href="'"$(pg "SELECT value FROM ir_config_parameter WHERE key='web.base.url'")"'/"' \
     "the homepage's canonical is the BARE DOMAIN, not /site"
t_contains "$(curl -s "$BASE/sitemap.xml")" "<loc>$(pg "SELECT value FROM ir_config_parameter WHERE key='web.base.url'")/</loc>" \
     "…and the sitemap agrees with it"

# robots.txt used to end "Disallow: /", which was right when / was the login
# page and would now hide the homepage from every search engine.
ROB=$(curl -s "$BASE/robots.txt")
t_lacks    "$ROB" 'Disallow: /
' "robots.txt does not blanket-disallow the site root"
t_contains "$ROB" 'Disallow: /login'  "the application is excluded by name"
t_contains "$ROB" 'Disallow: /web'    "so is its API"
t_contains "$ROB" 'Disallow: /portal' "and the customer portal"
t_contains "$ROB" 'Allow: /'          "while the site itself is crawlable"

# An ERP-only install must not answer its own domain with a 404.
pg "UPDATE website_page SET is_homepage=FALSE WHERE id=$PID" >/dev/null
t_eq "302" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/")" \
     "with no homepage published, / hands the front door to the application"
t_contains "$(curl -s -D - -o /dev/null "$BASE/")" '/login' "…by redirecting there"
pg "UPDATE website_page SET is_homepage=TRUE WHERE id=$PID" >/dev/null

verdict; exit 1; }
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE$PAGE_PATH")" "and a visitor can read it"

# ------------------------------------------------------------------
sec "1. the palette is readable through the API"
# ------------------------------------------------------------------
GOT=$(curl -s -H "Cookie: session_id=$ADMIN_SID" "$BASE/site/api/theme")
t_contains "$GOT" '"presets"'   "the API offers a list of presets"
t_contains "$GOT" '"paper"'     "including the original look"
t_contains "$GOT" '"midnight"'  "and a dark one"
t_contains "$GOT" '"dark_mode"' "and how dark mode is reached"
t_contains "$GOT" '"on_accent"' "and the ink computed for the accent"

# ------------------------------------------------------------------
sec "2. a preset reaches the stylesheet"
# ------------------------------------------------------------------
# The whole point of the change: --bg used to be the literal #fff and could
# not be moved. If this passes, it can.
theme '{"theme":"paper","accent":"","on_accent":"","dark_mode":"auto"}' >/dev/null
PAPER=$(root)
t_contains "$PAPER" '--bg:#ffffff'  "paper grounds the page in white"
t_contains "$PAPER" '--a:#0a6f7d'   "and uses its own accent when none is set"

theme '{"theme":"slate"}' >/dev/null
SLATE=$(root)
t_contains "$SLATE" '--bg:#f6f8fa'      "slate moves the ground off white"
t_contains "$SLATE" '--surface:#ffffff' "and lifts cards above it"
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE$PAGE_PATH")" "the page still renders"

theme '{"theme":"sand"}' >/dev/null
t_contains "$(root)" '--bg:#faf7f3' "sand warms it"

# ------------------------------------------------------------------
sec "3. the accent, and the ink computed for it"
# ------------------------------------------------------------------
theme '{"theme":"paper","accent":"#e94560","on_accent":""}' >/dev/null
A=$(root)
t_contains "$A" '--a:#e94560'    "an explicit accent overrides the preset's"
# The old CSS hardcoded white here. On this pink, near-black is the higher
# contrast choice (4.85:1 against 3.83:1), so that is what it must pick.
t_contains "$A" '--on-a:#111318' "the ink on the accent is COMPUTED, not assumed white"

theme '{"accent":"#0a6f7d"}' >/dev/null
t_contains "$(root)" '--on-a:#ffffff' "…and flips to white on a dark accent"

# A brand that has always been white-on-pink must be able to say so.
theme '{"accent":"#e94560","on_accent":"#ffffff"}' >/dev/null
t_contains "$(root)" '--on-a:#ffffff' "an explicit on_accent wins over the computation"
theme '{"on_accent":""}' >/dev/null
t_contains "$(root)" '--on-a:#111318' "clearing it goes back to computed"

# Shorthand is a colour too.
theme '{"accent":"#0af"}' >/dev/null
t_contains "$(root)" '--a:#00aaff' "#rgb shorthand is expanded, not rejected"

# ------------------------------------------------------------------
sec "4. dark mode means what it says"
# ------------------------------------------------------------------
theme '{"theme":"paper","dark_mode":"auto"}' >/dev/null
t_eq "1" "$(scheme_blocks)" \
     "auto asks the visitor's OS, once"
t_contains "$(darkroot)" '--bg:#101820' "and the dark block carries dark tokens"

theme '{"dark_mode":"off"}' >/dev/null
t_eq "0" "$(scheme_blocks)" \
     "off never asks — a dark-mode visitor gets the light site"

theme '{"theme":"midnight","dark_mode":"on"}' >/dev/null
t_eq "0" "$(scheme_blocks)" \
     "on does not ask either"
t_contains "$(root)" '--bg:#0e151d' "…because the dark tokens ARE the tokens"

# ------------------------------------------------------------------
sec "5. bad values are refused, and refused ATOMICALLY"
# ------------------------------------------------------------------
theme '{"theme":"paper","accent":"#123456","dark_mode":"auto"}' >/dev/null
BEFORE=$(root)

t_eq "400" "$(acode "$ADMIN_SID" POST /site/api/theme '{"theme":"nope"}')" \
     "an unknown preset is refused"
t_eq "400" "$(acode "$ADMIN_SID" POST /site/api/theme '{"theme":"../../etc/passwd"}')" \
     "so is a traversal-shaped one"
t_eq "400" "$(acode "$ADMIN_SID" POST /site/api/theme '{"accent":"red"}')" \
     "a named colour is not a colour here"
t_eq "400" "$(acode "$ADMIN_SID" POST /site/api/theme '{"accent":"#fff;}body{display:none}"}')" \
     "nor is a CSS injection"
t_eq "400" "$(acode "$ADMIN_SID" POST /site/api/theme '{"dark_mode":"maybe"}')" \
     "dark_mode is a closed set"
t_eq "400" "$(acode "$ADMIN_SID" POST /site/api/theme '{"accent":123}')" \
     "a non-string is refused rather than coerced"
t_eq "400" "$(acode "$ADMIN_SID" POST /site/api/theme '{}')" \
     "an empty request changes nothing"
t_eq "400" "$(acode "$ADMIN_SID" POST /site/api/theme 'not json')" \
     "so does a body that is not JSON"

t_eq "$BEFORE" "$(root)" "after eight refusals the palette is untouched"

# The atomicity claim: one good field, one bad one, in the same request.
t_eq "400" "$(acode "$ADMIN_SID" POST /site/api/theme \
             '{"theme":"midnight","accent":"not-a-colour"}')" \
     "a request that is partly valid is still refused"
t_eq "$BEFORE" "$(root)" \
     "…and the VALID half of it was not applied either"

# The stylesheet must never carry a token with no value — that paints the
# site out just as effectively as an injection.
t_lacks "$(root)" '--bg:;'      "no empty background declaration"
t_lacks "$(root)" '--a:;'       "no empty accent declaration"
t_lacks "$(root)" '--on-a:;'    "no empty ink declaration"

# ------------------------------------------------------------------
sec "6. the same gate as editing a page"
# ------------------------------------------------------------------
t_eq "401" "$(acode "" GET  /site/api/theme)"                       "reading needs a session"
t_eq "401" "$(acode "" POST /site/api/theme '{"theme":"midnight"}')" "writing needs a session"
t_eq "401" "$(acode "deadbeefdeadbeefdeadbeef" POST /site/api/theme '{"theme":"midnight"}')" \
     "a forged cookie is not a session"

# The case that matters: a real employee, legitimately signed in, with no
# website permission. Changing the company's colours is a configuration act.
PART=$(call res.partner create '[{"name":"TH Plain","email":"th_plain@t.test"}]' | rid)
U=$(call res.users create "[{\"login\":\"th_plain@t.test\",\"password\":\"Plain-Pass-1\",\"partner_id\":$PART,\"active\":true}]" | rid)
t_nonempty "$U" "an ordinary staff user exists"
PLAIN=$(login 'th_plain@t.test' 'Plain-Pass-1')
t_nonempty "$PLAIN" "they can sign in"
t_eq "0" "$(pg "SELECT count(*) FROM res_groups_users_rel WHERE uid=$U AND gid=4")" \
     "they are NOT in the configuration group"
t_eq "403" "$(acode "$PLAIN" GET  /site/api/theme)"  "they cannot read the theme"
t_eq "403" "$(acode "$PLAIN" POST /site/api/theme '{"theme":"midnight"}')" \
     "and cannot repaint the company website"
t_eq "$BEFORE" "$(root)" "the site is unchanged after their attempt"

# ------------------------------------------------------------------
sec "7. the colours object is not a write primitive for the config table"
# ------------------------------------------------------------------
# ir_config_parameter holds settings for the whole ERP. An unfiltered
# {key: value} write here would reach every one of them.
t_eq "400" "$(acode "$ADMIN_SID" POST /site/api/theme \
             '{"colors":{"web.base.url":"http://evil.test"}}')" \
     "an arbitrary config key is refused"
t_eq "400" "$(acode "$ADMIN_SID" POST /site/api/theme '{"colors":{"password":"#ffffff"}}')" \
     "so is a suggestive one"
t_eq "400" "$(acode "$ADMIN_SID" POST /site/api/theme '{"colors":"#fff"}')" \
     "colors must be an object"
t_eq "http://localhost:8069" "$(pg "SELECT value FROM ir_config_parameter WHERE key='web.base.url'")" \
     "web.base.url is untouched"

# The ten real token keys DO work, so the allowlist is an allowlist and not a
# wall — an owner can move a single colour without restating the other nine.
t_eq "200" "$(acode "$ADMIN_SID" POST /site/api/theme \
             '{"colors":{"bg":"#fdf6e3","dark.bg":"#0b1015"}}')" \
     "a real token override is accepted"
t_contains "$(root)"     '--bg:#fdf6e3' "and reaches the light scheme"
t_contains "$(darkroot)" '--bg:#0b1015' "and the dark one"

t_eq "200" "$(acode "$ADMIN_SID" POST /site/api/theme '{"colors":{"bg":""}}')" \
     "an override can be cleared"
t_contains "$(root)" '--bg:#ffffff' "…returning the preset's own ground"

# ------------------------------------------------------------------
sec "8. the derived accent tones reach the page, in both schemes"
# ------------------------------------------------------------------
# A design that uses colour needs the accent at several strengths against THIS
# scheme's ground. If these are missing the stylesheet still parses and the
# page renders untinted, so the failure is silent — hence an explicit check.
theme '{"theme":"paper","accent":"#e94560","dark_mode":"auto"}' >/dev/null
LIGHT=$(root); DARK=$(darkroot)
for tok in '--a-tint:' '--a-tint2:' '--a-soft:' '--a-deep:' '--a-rule:' '--a-text:'; do
    t_contains "$LIGHT" "$tok" "light scheme carries $tok"
    t_contains "$DARK"  "$tok" "dark scheme carries $tok"
done
# They must be DERIVED per scheme, not written once and shared: an 8% wash over
# white and the same wash over #101820 are not the same colour.
LT=$(printf '%s' "$LIGHT" | grep -o -- '--a-tint:#[0-9a-f]*')
DT=$(printf '%s' "$DARK"  | grep -o -- '--a-tint:#[0-9a-f]*')
t_nonempty "$LT" "the light tint has a value"
t_nonempty "$DT" "so does the dark one"
t_ne "$LT" "$DT" "the dark scheme derives its OWN tint rather than sharing the light one"
# The accent as TEXT is darkened until readable; #e94560 on white is 3.83:1.
t_lacks "$LIGHT" '--a-text:#e94560' "accent-as-text is adjusted, not the raw accent"

# ------------------------------------------------------------------
sec "9. the staff sign-in link"
# ------------------------------------------------------------------
pg "DELETE FROM ir_config_parameter WHERE key='website.login_link'" >/dev/null
PUB=$(curl -s "$BASE$PAGE_PATH")
t_contains "$PUB" 'class="w-login"' "the top bar offers a way in by default"
t_contains "$PUB" 'rel="nofollow"'  "…but does not invite crawlers to index it"

# It is a link, not a bypass: the page it points at is the ordinary login, and
# nothing about showing it changes what an unauthenticated caller may do.
t_eq "401" "$(acode "" POST /site/api/theme '{"theme":"midnight"}')" \
     "showing the link grants a visitor nothing"

pg "INSERT INTO ir_config_parameter (key,value) VALUES ('website.login_link','off')
    ON CONFLICT (key) DO UPDATE SET value='off'" >/dev/null
t_lacks "$(curl -s "$BASE$PAGE_PATH")" 'class="w-login"' \
     "an owner who would rather not advertise it can turn it off"
pg "DELETE FROM ir_config_parameter WHERE key='website.login_link'" >/dev/null

verdict

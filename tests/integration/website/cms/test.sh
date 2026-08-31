#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# The CMS (docs/115).
#
# A CMS is content written by one person and executed in another person's
# browser, on a page anyone can reach. So the assertions that matter are:
#
#   §4  STORED XSS — every ordinary block escapes its author's text, and the
#       one raw-HTML block is sanitised against an allowlist. This is the
#       section that would fail if the block model were ever swapped back for
#       free-form markup.
#   §5  DRAFT GATING — an unpublished page answers 404 to the public, the same
#       answer as a page that does not exist, so unreleased URLs cannot be
#       probed.
#   §7  robots.txt and sitemap.xml never expose the ERP, the portal, or a page
#       that is not published AND indexed.
# =============================================================
auth_or_die

anon()  { curl -s "$BASE$1"; }
acode() { curl -s -o /dev/null -w '%{http_code}' "$BASE$1"; }

cleanup() {
    pg "DELETE FROM website_menu WHERE name LIKE 'CMS %'" >/dev/null
    pg "DELETE FROM website_page WHERE slug LIKE 'cms-%'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

# ------------------------------------------------------------------
sec "1. schema, seed and menus"
# ------------------------------------------------------------------
t_eq "1" "$(pg "SELECT count(*) FROM pg_tables WHERE tablename='website_page'")" "website_page exists"
t_eq "1" "$(pg "SELECT count(*) FROM pg_tables WHERE tablename='website_menu'")" "website_menu exists"
t_eq "1" "$(pg "SELECT count(*) FROM pg_indexes WHERE indexname='website_page_homepage_uniq'")" \
     "only one page can be the homepage"
t_eq "website.page" "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Website Pages' LIMIT 1")" \
     "Settings -> Website Pages is wired"
# The shipped PLACEHOLDER must not be live: deploying should not put "This is
# your website's home page" on a public site.
#
# Keyed on the placeholder's own title rather than on the slug 'home' — once a
# real site is seeded over it (scripts/seed_easylocker_site.py) that slug is
# legitimately published, and an assertion on the slug would then be failing
# for the right reason at the wrong time.
t_eq "0" "$(pg "SELECT count(*) FROM website_page
                 WHERE title='Welcome' AND blocks_json LIKE '%your website''s home page%'
                   AND is_published")" \
     "the shipped placeholder page is never published"

# ------------------------------------------------------------------
sec "2. creating and publishing a page"
# ------------------------------------------------------------------
BLOCKS='[{"type":"heading","level":"1","text":"Our Widgets"},{"type":"text","text":"Line one.\nLine two.\n\nA second paragraph."},{"type":"divider"},{"type":"button","text":"Contact us","href":"/site/cms-contact"}]'
P=$(call website.page create "[{\"slug\":\"cms-widgets\",\"title\":\"Our Widgets\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$BLOCKS"),\"is_published\":true}]" | rid)
t_nonempty "$P" "a page was created"
[ -z "$P" ] && { verdict; exit 1; }

BODY=$(anon /site/cms-widgets)
t_eq "200" "$(acode /site/cms-widgets)" "the published page is served"
t_contains "$BODY" '<h1 class="w-h">Our Widgets</h1>' "the heading block renders"
t_contains "$BODY" 'Line one.<br/>Line two.' "newlines become line breaks"
t_contains "$BODY" '<p class="w-p">A second paragraph.</p>' "a blank line starts a new paragraph"
t_contains "$BODY" '<hr class="w-hr"/>' "the divider renders"
t_contains "$BODY" 'class="w-btn"' "the button renders"

# ------------------------------------------------------------------
sec "3. slugs are constrained"
# ------------------------------------------------------------------
for bad in '../etc/passwd' '/leading' 'Upper' 'has space' 'semi;colon' 'a//b'; do
    RES=$(call website.page create "[{\"slug\":\"$bad\",\"title\":\"x\"}]")
    has_error "$RES" && ok "slug '$bad' is refused" || no "slug '$bad' was accepted"
done
t_eq "404" "$(acode '/site/../../etc/passwd')" "a traversal path finds no page"

# ------------------------------------------------------------------
sec "4. STORED XSS — the section that matters"
# ------------------------------------------------------------------
# Author text goes out ESCAPED, by construction: the server builds the markup
# and the author only ever supplies strings.
XSS='<script>alert(1)</script>'
XB="[{\"type\":\"heading\",\"level\":\"1\",\"text\":\"$XSS\"},{\"type\":\"text\",\"text\":\"$XSS\"}]"
PX=$(call website.page create "[{\"slug\":\"cms-xss\",\"title\":\"$XSS\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$XB"),\"is_published\":true}]" | rid)
t_nonempty "$PX" "a page with hostile text was accepted (it is only text)"
XBODY=$(anon /site/cms-xss)
t_lacks "$XBODY" '<script>alert(1)</script>' "the script tag is NOT emitted"
t_contains "$XBODY" '&lt;script&gt;' "it is escaped instead"
# ...including in the <title>, which is a separate escaping path.
t_lacks "$XBODY" '<title><script>' "the page title is escaped too"

# The raw-HTML block is the one that carries markup. It is sanitised.
mkraw() { # mkraw <slug> <raw html>
    local blocks
    blocks=$(python3 -c 'import json,sys;print(json.dumps(json.dumps([{"type":"html","html":sys.argv[1]}])))' "$2")
    call website.page create "[{\"slug\":\"$1\",\"title\":\"raw\",\"blocks_json\":$blocks,\"is_published\":true}]" | rid
}
RID=$(mkraw cms-raw '<p class="x">keep me</p><script>alert(1)</script><img src=x onerror=alert(1)><a href="javascript:alert(1)">bad</a><iframe src="//evil"></iframe><a href="https://ok.example">good</a>')
t_nonempty "$RID" "a raw-html block page was created"
RB=$(anon /site/cms-raw)
t_contains "$RB" 'keep me'            "allowed markup survives"
# '<script>' with the bracket, not '<script' — every page now legitimately
# carries <script type="application/ld+json"> (docs/118 E2), and a bare prefix
# grep would fail on that rather than on anything the sanitiser let through.
t_lacks    "$RB" '<script>'           "script elements are dropped"
t_lacks    "$RB" 'alert(1)'           "and their payload with them"
t_lacks    "$RB" 'onerror'            "event handlers are dropped"
t_lacks    "$RB" 'javascript:'        "javascript: URLs are dropped"
t_lacks    "$RB" '<iframe'            "iframes are dropped"
t_contains "$RB" 'https://ok.example' "a safe link survives"
t_contains "$RB" 'rel="noopener'      "and is given rel=noopener"

# Case and whitespace tricks must not get past the allowlist.
RID2=$(mkraw cms-raw2 '<SCRIPT>alert(1)</SCRIPT><IMG SRC="JaVaScRiPt:alert(1)"><div OnClick="x()">t</div><!-- <script>x</script> -->')
RB2=$(anon /site/cms-raw2)
t_lacks "$RB2" 'alert(1)'  "an upper-case SCRIPT tag is dropped"
t_lacks "$RB2" 'JaVaScRiPt' "a mixed-case javascript: URL is dropped"
t_lacks "$RB2" 'OnClick'   "a mixed-case event handler is dropped"
t_lacks "$RB2" 'onclick'   "...in any case"

# An unknown block type renders nothing rather than something unvouched-for.
RID3=$(call website.page create "[{\"slug\":\"cms-unknown\",\"title\":\"u\",\"blocks_json\":\"[{\\\"type\\\":\\\"evil\\\",\\\"html\\\":\\\"<script>x</script>\\\"}]\",\"is_published\":true}]" | rid)
UB=$(anon /site/cms-unknown)
t_lacks "$UB" '<script>' "an unknown block type renders nothing"
t_lacks "$UB" 'x</script>' "and its payload is not emitted either"

# ------------------------------------------------------------------
sec "5. DRAFT GATING — unpublished is invisible, not just unlinked"
# ------------------------------------------------------------------
PD=$(call website.page create '[{"slug":"cms-secret","title":"Unreleased Product","is_published":false}]' | rid)
t_nonempty "$PD" "a draft page exists"
t_eq "404" "$(acode /site/cms-secret)" "the public gets 404 for a draft"
DB=$(anon /site/cms-secret)
t_lacks "$DB" 'Unreleased Product' "and the draft's title does not leak"
# The same answer as a page that never existed — so the URL space cannot be
# probed to find out what is coming.
t_eq "$(acode /site/cms-secret)" "$(acode /site/cms-does-not-exist)" \
     "a draft and a non-existent page answer identically"

# Staff may preview it.
SBODY=$(curl -s -H "Cookie: session_id=$SID" "$BASE/site/cms-secret")
t_contains "$SBODY" 'Unreleased Product' "a signed-in staff user can preview the draft"
t_contains "$SBODY" 'Draft' "and is told it is a draft"

# Publishing makes it public; unpublishing takes it away again.
call website.page write "[[$PD],{\"is_published\":true}]" >/dev/null
t_eq "200" "$(acode /site/cms-secret)" "publishing makes it public"
call website.page write "[[$PD],{\"is_published\":false}]" >/dev/null
t_eq "404" "$(acode /site/cms-secret)" "unpublishing takes it away again"

# ------------------------------------------------------------------
sec "6. the menu"
# ------------------------------------------------------------------
M1=$(call website.menu create "[{\"name\":\"CMS Widgets\",\"page_id\":$P,\"sequence\":10}]" | rid)
M2=$(call website.menu create "[{\"name\":\"CMS Draft\",\"page_id\":$PD,\"sequence\":20}]" | rid)
t_nonempty "$M1" "a menu entry was created"
NB=$(anon /site/cms-widgets)
t_contains "$NB" 'CMS Widgets' "the menu shows the published page"
t_lacks    "$NB" 'CMS Draft'   "a menu entry for a DRAFT page is hidden, not a link to a 404"

# A menu URL is an href, so hostile schemes are refused on the way in.
BADM=$(call website.menu create '[{"name":"CMS Bad","url":"javascript:alert(1)"}]')
has_error "$BADM" && ok "a javascript: menu URL is refused" || no "a javascript: menu URL was stored"

# ------------------------------------------------------------------
sec "7. SEO — and what must NOT be in it"
# ------------------------------------------------------------------
call website.page write "[[$P],{\"meta_description\":\"Widgets for every need\",\"meta_keywords\":\"widgets,parts\"}]" >/dev/null
SB=$(anon /site/cms-widgets)
t_contains "$SB" 'name="description" content="Widgets for every need"' "the meta description is emitted"
t_contains "$SB" 'property="og:title"' "OpenGraph tags are emitted"
t_contains "$SB" 'name="twitter:card"' "Twitter card tags are emitted"
t_contains "$SB" 'rel="canonical"' "a canonical URL is emitted"

RT=$(anon /robots.txt)
t_eq "200" "$(acode /robots.txt)" "robots.txt is served"
t_contains "$RT" 'Allow: /site'      "the public site is crawlable"
t_contains "$RT" 'Disallow: /web'    "the ERP is not"
t_contains "$RT" 'Disallow: /portal' "nor the customer portal"
t_contains "$RT" 'Disallow: /kiosk'  "nor the kiosk"
t_contains "$RT" 'Sitemap:'          "and it points at the sitemap"

SM=$(anon /sitemap.xml)
t_eq "200" "$(acode /sitemap.xml)" "sitemap.xml is served"
t_contains "$SM" '/site/cms-widgets' "a published, indexed page is listed"
t_lacks    "$SM" 'cms-secret'        "an unpublished page is NOT listed"

# noindex is honoured on both the page and the sitemap.
call website.page write "[[$P],{\"is_indexed\":false}]" >/dev/null
t_contains "$(anon /site/cms-widgets)" 'name="robots" content="noindex' "a non-indexed page says noindex"
t_lacks "$(anon /sitemap.xml)" '/site/cms-widgets' "and drops out of the sitemap"
call website.page write "[[$P],{\"is_indexed\":true}]" >/dev/null

# ------------------------------------------------------------------
sec "7b. A3 — reference and map blocks"
# ------------------------------------------------------------------
REFB='[{"type":"references","items":[{"name":"Acme Bhd","note":"Since 2019"},{"name":"<script>alert(1)</script>","note":"x"}]},{"type":"map","query":"Kuala Lumpur"}]'
PR=$(call website.page create "[{\"slug\":\"cms-refs\",\"title\":\"References\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$REFB"),\"is_published\":true}]" | rid)
t_nonempty "$PR" "a page with reference and map blocks"
RB3=$(anon /site/cms-refs)
t_contains "$RB3" 'Acme Bhd'      "a customer reference renders"
t_contains "$RB3" 'Since 2019'    "with its note"
t_lacks    "$RB3" '<script>alert' "a hostile reference name is escaped"
t_contains "$RB3" 'openstreetmap' "the map block renders against a fixed provider"
t_contains "$RB3" 'Kuala+Lumpur'  "with the query percent-encoded"
# A place NAME alone renders a link, not an iframe: an OpenStreetMap embed
# needs a bounding box, and without one it is an empty grey rectangle — worse
# than no map at all.
t_lacks    "$RB3" '<iframe'       "a map with no coordinates renders a link, not a blank frame"
# The map is an iframe the SERVER builds — an author supplies a query, never
# markup, so it cannot become an arbitrary frame.
# With coordinates it DOES embed — and that frame must be sandboxed.
COORDB='[{"type":"map","lat":"3.139","lon":"101.6869","query":"KL","label":"Find us"}]'
PC=$(call website.page create "[{\"slug\":\"cms-mapc\",\"title\":\"MapC\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$COORDB"),\"is_published\":true}]" | rid)
MC=$(anon /site/cms-mapc)
t_contains "$MC" '<iframe'   "coordinates produce a real embedded map"
t_contains "$MC" 'bbox='     "with a bounding box"
t_contains "$MC" 'marker='   "and a marker"
t_contains "$MC" 'sandbox='  "and the frame is sandboxed"
# A coordinate is a number or it is nothing — it lands inside an attribute.
BADC='[{"type":"map","lat":"0\" onload=alert(1)","lon":"1","query":"Q"}]'
PBC=$(call website.page create "[{\"slug\":\"cms-mapbad\",\"title\":\"MapBad\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$BADC"),\"is_published\":true}]" | rid)
t_lacks "$(anon /site/cms-mapbad)" 'onload' "a non-numeric coordinate cannot inject an attribute"
MAPX='[{"type":"map","query":"\"></iframe><script>alert(1)</script>"}]'
PX2=$(call website.page create "[{\"slug\":\"cms-mapx\",\"title\":\"MapX\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$MAPX"),\"is_published\":true}]" | rid)
MB=$(anon /site/cms-mapx)
t_lacks "$MB" '<script>alert(1)</script>' "a map query cannot break out of the iframe src"

# ------------------------------------------------------------------
sec "7b2. hero / pricing / steps / faq blocks"
# ------------------------------------------------------------------
# The blocks a marketing site is built from. Each takes author text, so each
# is an escaping question as well as a layout one.
SITEB='[{"type":"hero","eyebrow":"Eyebrow","headline":"Big headline","subheadline":"A sentence.","cta_text":"Go","cta_href":"/site/cms-widgets"},{"type":"pricing","items":[{"name":"The 50","size":"50 sq ft","price":"RM 190","period":"/month","badge":"Popular","featured":true,"features":["Dry","Alarmed"],"cta_text":"Enquire","cta_href":"/site/cms-widgets"}]},{"type":"steps","items":[{"title":"First","text":"Do this."}]},{"type":"faq","items":[{"q":"A question?","a":"An answer."}]}]'
PS=$(call website.page create "[{\"slug\":\"cms-site\",\"title\":\"Site Blocks\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$SITEB"),\"is_published\":true}]" | rid)
SB2=$(anon /site/cms-site)
t_contains "$SB2" 'w-hero'       "the hero renders"
t_contains "$SB2" 'Big headline' "with its headline"
t_contains "$SB2" 'w-plan'       "a pricing card renders"
t_contains "$SB2" 'RM 190'       "with its price"
t_contains "$SB2" 'is-feat'      "and the featured card is marked"
t_contains "$SB2" 'w-steps'      "steps render"
t_contains "$SB2" '<details'     "the FAQ uses <details>, so it works with JS off"
t_contains "$SB2" 'An answer.'   "with its answer"

# Every one of those fields is author text.
XB2='[{"type":"hero","headline":"<script>alert(1)</script>"},{"type":"pricing","items":[{"name":"<script>alert(1)</script>","features":["<img src=x onerror=alert(1)>"]}]},{"type":"faq","items":[{"q":"<script>alert(1)</script>","a":"x"}]}]'
PX3=$(call website.page create "[{\"slug\":\"cms-sitex\",\"title\":\"SiteX\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$XB2"),\"is_published\":true}]" | rid)
SX=$(anon /site/cms-sitex)
t_lacks    "$SX" '<script>alert(1)</script>' "hostile text in these blocks is not emitted as markup"
t_lacks    "$SX" '<img src=x'                "nor in a feature line"
t_contains "$SX" '&lt;script&gt;'            "it is escaped"

# ------------------------------------------------------------------
sec "7c. A4 — the blog"
# ------------------------------------------------------------------
t_eq "200" "$(acode /site/blog)" "the blog index is served"
POST1='[{"type":"text","text":"We have moved to a bigger unit."}]'
B1=$(call website.page create "[{\"slug\":\"cms-post-one\",\"title\":\"We have moved\",\"page_kind\":\"post\",\"author\":\"Ops\",\"excerpt\":\"A short note.\",\"publish_date\":\"2026-06-01\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$POST1"),\"is_published\":true}]" | rid)
t_nonempty "$B1" "a published post"
IDX=$(anon /site/blog)
t_contains "$IDX" 'We have moved' "the post is listed on the index"
t_contains "$IDX" 'A short note.' "with its excerpt"
t_contains "$IDX" 'Ops'           "and its author"

PB1=$(anon /site/cms-post-one)
t_eq "200" "$(acode /site/cms-post-one)" "the post itself is served"
t_contains "$PB1" 'bigger unit'   "its content renders"
t_contains "$PB1" 'All news'      "and it links back to the index"
t_contains "$PB1" '2026-06-01'    "with its date"

# A DRAFT post is invisible, exactly like a draft page.
B2=$(call website.page create '[{"slug":"cms-post-draft","title":"Unannounced","page_kind":"post","is_published":false}]' | rid)
t_eq "404" "$(acode /site/cms-post-draft)" "a draft post is 404"
t_lacks "$(anon /site/blog)" 'Unannounced' "and is not on the index"

# A post dated in the FUTURE is scheduled, not published: held off the index
# and out of the sitemap until its date.
B3=$(call website.page create '[{"slug":"cms-post-future","title":"Next Quarter","page_kind":"post","publish_date":"2099-01-01","is_published":true}]' | rid)
t_lacks "$(anon /site/blog)"    'Next Quarter' "a future-dated post is not on the index"
t_lacks "$(anon /sitemap.xml)"  'cms-post-future' "nor in the sitemap"
t_contains "$(anon /sitemap.xml)" '/site/blog'  "the blog index is in the sitemap"
t_contains "$(anon /sitemap.xml)" 'cms-post-one' "and so is a live post"

# page_kind selects a rendering path, so it is constrained in the DATABASE,
# not only in the model: BaseModel::write() writes registered fields straight
# from the payload and walks past a model-only check.
RESK=$(call website.page write "[[$B1],{\"page_kind\":\"evil\"}]")
t_ne "evil" "$(pg "SELECT page_kind FROM website_page WHERE id=$B1")" \
     "an unknown page_kind is not stored"
# Negative control: the constraint, not the handler, is what refuses it.
DIRECT=$(pgv "UPDATE website_page SET page_kind='evil' WHERE id=$B1" 2>&1)
case "$DIRECT" in
    *website_page_kind_chk*|*violates*) ok "a direct UPDATE is rejected by the CHECK constraint" ;;
    *) no "the database accepted page_kind='evil'" ;;
esac

# ------------------------------------------------------------------
sec "7d. E3 — the site health check (docs/118)"
# ------------------------------------------------------------------
# the reference ERP has no built-in site audit. Each issue kind is provoked deliberately
# and then asserted, because a health check that reports nothing looks
# identical to one that is broken.
HEALTH=/site/api/health
t_eq "401" "$(acode $HEALTH)" "the report needs a session"
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' -H "Cookie: session_id=$SID" "$BASE$HEALTH")" \
     "an administrator can read it"

hjq() { curl -s -H "Cookie: session_id=$SID" "$BASE$HEALTH" \
        | python3 -c "import sys,json;d=json.loads(sys.stdin.read());print(sum(1 for i in d['issues'] if i['kind']=='$1' and i['subject']=='$2'))" 2>/dev/null; }

# A published page with no meta description.
call website.page create '[{"slug":"cms-nometa","title":"No Meta","is_published":true}]' >/dev/null
t_eq "1" "$(hjq meta_description cms-nometa)" "a missing meta description is reported"

# A menu entry pointing at a draft.
MD=$(call website.page create '[{"slug":"cms-hidden","title":"Hidden","is_published":false}]' | rid)
call website.menu create "[{\"name\":\"CMS ToDraft\",\"page_id\":$MD}]" >/dev/null
t_eq "1" "$(hjq menu_to_draft 'CMS ToDraft')" "a menu entry pointing at a draft is reported"

# An image with no alt text, and a button pointing at a page that is not there.
BADB='[{"type":"image","src":"https://x/y.png","alt":""},{"type":"button","text":"Go","href":"/site/cms-nowhere"}]'
call website.page create "[{\"slug\":\"cms-issues\",\"title\":\"Issues Page\",\"meta_description\":\"d\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$BADB"),\"is_published\":true}]" >/dev/null
t_eq "1" "$(hjq image_no_alt cms-issues)"  "an image with no alt text is reported"
t_eq "1" "$(hjq broken_link  cms-issues)"  "a button pointing at a missing page is reported"

# A form with no fields cannot be submitted.
call website.form create '[{"slug":"cms-emptyform","title":"Empty"}]' >/dev/null
t_eq "1" "$(hjq form_no_fields cms-emptyform)" "a form with no fields is reported"

# A published page in no menu — information, not a fault: a landing page is
# often deliberately unlinked.
t_eq "1" "$(hjq orphan_page cms-nometa)" "a page in no menu is reported as information"
SEV=$(curl -s -H "Cookie: session_id=$SID" "$BASE$HEALTH" \
      | python3 -c "import sys,json;d=json.loads(sys.stdin.read());print([i['severity'] for i in d['issues'] if i['kind']=='orphan_page' and i['subject']=='cms-nometa'][0])" 2>/dev/null)
t_eq "info" "$SEV" "...and graded as info, not an error"

# The counts add up to the list — a summary that disagrees with its detail is
# worse than no summary.
SUMOK=$(curl -s -H "Cookie: session_id=$SID" "$BASE$HEALTH" \
        | python3 -c "import sys,json;d=json.loads(sys.stdin.read());c=d['counts'];print('yes' if c['total']==len(d['issues']) and c['error']+c['warning']+c['info']==c['total'] else 'no')" 2>/dev/null)
t_eq "yes" "$SUMOK" "the counts agree with the issue list"

# Fixing an issue makes it go away — otherwise the report is just noise.
call website.page write "[[$(pg "SELECT id FROM website_page WHERE slug='cms-nometa'")],{\"meta_description\":\"Now described\"}]" >/dev/null
t_eq "0" "$(hjq meta_description cms-nometa)" "fixing the page clears the issue"

pg "DELETE FROM website_menu WHERE name='CMS ToDraft'" >/dev/null
pg "DELETE FROM website_form WHERE slug='cms-emptyform'" >/dev/null

# ------------------------------------------------------------------
sec "8. the public site is not a session surface"
# ------------------------------------------------------------------
HDRS=$(curl -s -D - -o /dev/null "$BASE/site/cms-widgets" | tr 'A-Z' 'a-z')
t_lacks "$HDRS" 'set-cookie' "serving a public page sets no cookie"
t_contains "$HDRS" 'nosniff' "and carries X-Content-Type-Options"
# The ERP is untouched by any of this.
t_eq "200" "$(acode /healthz)" "the ERP is still healthy"

verdict

#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# In-place website editing (docs/117).
#
# The feature is "let an admin edit the site by clicking on it". The risk is
# letting anyone ELSE do that, so almost every assertion here is a refusal.
#
# §3 is the one that matters most: a STAFF user WITHOUT the configuration
# group. Anonymous and portal callers are the easy cases — the interesting
# failure is an ordinary employee who is legitimately logged in, because that
# is what a test written only against "logged out vs admin" would miss.
#
# §6 protects the property docs/115 is built on: editing must not become a
# second way for markup to reach a public page.
# =============================================================
auth_or_die

ADMIN_SID="$SID"

api() {  # api <sid> <method> <path> [body]
    if [ "$2" = "GET" ]; then
        curl -s -H "Cookie: session_id=${1:-}" "$BASE$3"
    else
        curl -s -X POST -H "Cookie: session_id=${1:-}" \
             -H 'Content-Type: application/json' --data "${4:-{\}}" "$BASE$3"
    fi
}
acode() { # acode <sid> <method> <path> [body]
    if [ "$2" = "GET" ]; then
        curl -s -o /dev/null -w '%{http_code}' -H "Cookie: session_id=${1:-}" "$BASE$3"
    else
        curl -s -o /dev/null -w '%{http_code}' -X POST -H "Cookie: session_id=${1:-}" \
             -H 'Content-Type: application/json' --data "${4:-{\}}" "$BASE$3"
    fi
}

cleanup() {
    pg "DELETE FROM website_page_revision WHERE page_id IN (SELECT id FROM website_page WHERE slug LIKE 'ed-%')" >/dev/null
    pg "DELETE FROM website_page WHERE slug LIKE 'ed-%'" >/dev/null
    pg "DELETE FROM res_groups_users_rel WHERE uid IN
          (SELECT id FROM res_users WHERE login LIKE 'ed_%')" >/dev/null
    pg "DELETE FROM res_users   WHERE login LIKE 'ed_%'" >/dev/null
    pg "DELETE FROM res_partner WHERE name LIKE 'ED %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

# ------------------------------------------------------------------
sec "1. a page to edit"
# ------------------------------------------------------------------
BLK='[{"type":"heading","level":"1","text":"Before"},{"type":"text","text":"Original copy."}]'
P=$(call website.page create "[{\"slug\":\"ed-page\",\"title\":\"Editable\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$BLK"),\"is_published\":true}]" | rid)
t_nonempty "$P" "a published page exists"
[ -z "$P" ] && { verdict; exit 1; }
API="/site/api/page/$P/blocks"

# ------------------------------------------------------------------
sec "2. an anonymous visitor sees no editor and cannot save"
# ------------------------------------------------------------------
PUB=$(curl -s "$BASE/site/ed-page")
t_contains "$PUB" 'Before'            "the page renders for the public"
t_lacks    "$PUB" 'website-editor.js' "the editor script is NOT in a visitor's HTML"
t_lacks    "$PUB" '__WSITE_EDIT'      "nor the editor's configuration"
t_lacks    "$PUB" 'wse-bar'           "nor the toolbar"
t_eq "401" "$(acode "" GET  "$API")"  "reading the blocks needs a session"
t_eq "401" "$(acode "" POST "$API" '{"blocks":[]}')" "saving needs a session"
# A forged cookie is not a session.
t_eq "401" "$(acode "deadbeefdeadbeefdeadbeef" POST "$API" '{"blocks":[]}')" \
     "a forged session cookie cannot save"

# ------------------------------------------------------------------
sec "3. THE CASE THAT MATTERS — staff WITHOUT the group"
# ------------------------------------------------------------------
# An ordinary employee is legitimately logged in. They must still not be able
# to change the public website. A test that only compared "logged out" with
# "admin" would pass while this was wide open.
PART=$(call res.partner create '[{"name":"ED Plain","email":"ed_plain@t.test"}]' | rid)
U=$(call res.users create "[{\"login\":\"ed_plain@t.test\",\"password\":\"Plain-Pass-1\",\"partner_id\":$PART,\"active\":true}]" | rid)
t_nonempty "$U" "an ordinary staff user exists"
PLAIN=$(login 'ed_plain@t.test' 'Plain-Pass-1')
t_nonempty "$PLAIN" "they can sign in"

t_eq "0" "$(pg "SELECT count(*) FROM res_groups_users_rel WHERE uid=$U AND gid=4")" \
     "they are NOT in the configuration group"

PPAGE=$(curl -s -H "Cookie: session_id=$PLAIN" "$BASE/site/ed-page")
t_lacks "$PPAGE" 'website-editor.js' "no editor script for staff without the group"
t_lacks "$PPAGE" '__WSITE_EDIT'      "nor its configuration"

t_eq "403" "$(acode "$PLAIN" POST "$API" '{"blocks":[{"type":"text","text":"hacked"}]}')" \
     "they are REFUSED when they save anyway (403, not 401 — they are known)"
t_eq "403" "$(acode "$PLAIN" GET "$API")" \
     "and cannot even read the blocks"
t_lacks "$(pg "SELECT blocks_json FROM website_page WHERE id=$P")" 'hacked' \
     "the page was not changed"

# A PORTAL customer's cookie is a different cookie entirely and buys nothing.
t_eq "401" "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Cookie: portal_sid=$PLAIN" \
              -H 'Content-Type: application/json' --data '{"blocks":[]}' "$BASE$API")" \
     "a portal cookie cannot edit the website"

# ------------------------------------------------------------------
sec "4. staff WITH the group can edit"
# ------------------------------------------------------------------
call res.users write "[[$U],{\"groups_id\":[[4,4]]}]" >/dev/null
t_eq "1" "$(pg "SELECT count(*) FROM res_groups_users_rel WHERE uid=$U AND gid=4")" \
     "the configuration group was granted"
EDITOR=$(login 'ed_plain@t.test' 'Plain-Pass-1')
t_nonempty "$EDITOR" "they sign in again"

EPAGE=$(curl -s -H "Cookie: session_id=$EDITOR" "$BASE/site/ed-page")
t_contains "$EPAGE" 'website-editor.js' "now the editor script IS served"
t_contains "$EPAGE" "page_id:$P"        "with this page's id"
t_contains "$EPAGE" 'admin:false'       "and marked as not an administrator"

t_eq "200" "$(acode "$EDITOR" GET "$API")" "they can read the blocks"
t_contains "$(api "$EDITOR" GET "$API")" 'Before' "and get the current content"

SAVE=$(api "$EDITOR" POST "$API" '{"blocks":[{"type":"heading","level":"1","text":"After"},{"type":"text","text":"Rewritten."}]}')
t_contains "$SAVE" '"ok":true' "they can save"
t_contains "$(curl -s "$BASE/site/ed-page")" 'After' "and the public page changed"
t_lacks    "$(curl -s "$BASE/site/ed-page")" 'Before' "the old content is gone"

# ------------------------------------------------------------------
sec "5. the raw-HTML block is administrator-only"
# ------------------------------------------------------------------
# It is the one block that carries markup, so an editor who is not an admin
# must not be able to introduce one — and must be TOLD, not silently ignored.
RAW='{"blocks":[{"type":"html","html":"<b>hi</b>"}]}'
t_eq "403" "$(acode "$EDITOR" POST "$API" "$RAW")" "a non-admin cannot add an html block"
RESP=$(api "$EDITOR" POST "$API" "$RAW")
t_contains "$RESP" 'administrator' "the refusal says why"
t_lacks "$(pg "SELECT blocks_json FROM website_page WHERE id=$P")" '<b>hi</b>' \
     "and nothing was stored"

t_eq "200" "$(acode "$ADMIN_SID" POST "$API" "$RAW")" "an administrator may"
APAGE=$(curl -s -H "Cookie: session_id=$ADMIN_SID" "$BASE/site/ed-page")
t_contains "$APAGE" 'admin:true' "and the page tells the editor they are one"

# ------------------------------------------------------------------
sec "6. editing is not a second way for markup to reach the page"
# ------------------------------------------------------------------
# The whole block model (docs/115) rests on author text never being markup.
# Saving through the editor must go through exactly the same escaping.
XSS='{"blocks":[{"type":"heading","level":"1","text":"<script>alert(1)</script>"},{"type":"text","text":"<img src=x onerror=alert(1)>"}]}'
t_eq "200" "$(acode "$ADMIN_SID" POST "$API" "$XSS")" "hostile TEXT is accepted (it is only text)"
XB=$(curl -s "$BASE/site/ed-page")
t_lacks    "$XB" '<script>alert(1)</script>' "but it is not emitted as markup"
t_contains "$XB" '&lt;script&gt;'            "it is escaped"
t_lacks    "$XB" '<img src=x'                "nor is the image tag"

# An unknown block type is refused rather than stored — stored, it would
# render as nothing and read as data loss.
t_eq "400" "$(acode "$ADMIN_SID" POST "$API" '{"blocks":[{"type":"evil","html":"<script>x</script>"}]}')" \
     "an unknown block type is refused"
t_eq "400" "$(acode "$ADMIN_SID" POST "$API" '{"blocks":[{"noType":1}]}')" "a block with no type is refused"
t_eq "400" "$(acode "$ADMIN_SID" POST "$API" '{"blocks":"not-a-list"}')"   "blocks must be a list"
t_eq "400" "$(acode "$ADMIN_SID" POST "$API" '{}')"                        "blocks is required"
t_eq "400" "$(acode "$ADMIN_SID" POST "$API" 'not json')"                  "a malformed body is refused"

# Caps: a page is content, not a payload.
BIG=$(python3 -c 'import json;print(json.dumps({"blocks":[{"type":"text","text":"x"} for _ in range(400)]}))')
t_eq "400" "$(acode "$ADMIN_SID" POST "$API" "$BIG")" "more than 200 blocks is refused"

# ------------------------------------------------------------------
sec "7. the editor cannot reach another page, or a missing one"
# ------------------------------------------------------------------
t_eq "404" "$(acode "$ADMIN_SID" POST "/site/api/page/999999/blocks" '{"blocks":[]}')" \
     "saving a page that does not exist is 404"
t_eq "404" "$(acode "$ADMIN_SID" GET  "/site/api/page/999999/blocks")" "as is reading one"

# The editor script itself is a static asset; serving it to anyone is harmless
# because it is inert without a session, but the endpoint is what is guarded.
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/website-editor.js")" \
     "the editor script is a static file"
JS=$(curl -s "$BASE/website-editor.js")
# The editor's own safety rests on two habits, so assert those rather than
# grepping for innerHTML — it DOES assign innerHTML, from a string it built
# itself out of escaped values, which is fine. What would not be fine is
# reading user input back as markup, or interpolating a value unescaped.
t_contains "$JS" 'el.textContent'       "it harvests textContent, never innerHTML"
t_lacks    "$JS" 'el.innerHTML'         "it never reads an edited element as markup"
t_contains "$JS" 'esc(b.text)'          "block text is escaped when drawn"
# The heading level becomes a TAG NAME, which escaping cannot make safe — it
# has to be constrained to a known set instead.
t_contains "$JS" '/^[123]$/'            "the heading level is constrained, not escaped"

# ------------------------------------------------------------------
sec "8. E1 — revision history (docs/118)"
# ------------------------------------------------------------------
# the reference ERP keeps exactly ONE previous version (ir.ui.view.arch_prev,
# overwritten on every write). Since docs/117 put a click-to-edit button on a
# LIVE PUBLIC PAGE, one step of undo is not enough.
REV="/site/api/page/$P/revisions"
t_eq "1" "$(pg "SELECT count(*) FROM pg_tables WHERE tablename='website_page_revision'")" \
     "the revision table exists"

pg "DELETE FROM website_page_revision WHERE page_id=$P" >/dev/null
api "$ADMIN_SID" POST "$API" '{"blocks":[{"type":"text","text":"Version one"}]}' >/dev/null
api "$ADMIN_SID" POST "$API" '{"blocks":[{"type":"text","text":"Version two"}]}' >/dev/null
api "$ADMIN_SID" POST "$API" '{"blocks":[{"type":"text","text":"Version three"}]}' >/dev/null

LIST=$(api "$ADMIN_SID" GET "$REV")
t_contains "$LIST" '"revisions"' "the history lists"
N=$(printf '%s' "$LIST" | python3 -c 'import sys,json;print(len(json.loads(sys.stdin.read()).get("revisions",[])))')
t_ge "$N" "3" "several versions were kept, not just one"
t_contains "$LIST" '"author"' "each records who made the change"
t_contains "$LIST" '"at"'     "and when"

# Saving identical content twice must not manufacture a version to scroll past.
BEFORE=$(pg "SELECT count(*) FROM website_page_revision WHERE page_id=$P")
api "$ADMIN_SID" POST "$API" '{"blocks":[{"type":"text","text":"Version three"}]}' >/dev/null
t_eq "$BEFORE" "$(pg "SELECT count(*) FROM website_page_revision WHERE page_id=$P")" \
     "re-saving identical content adds no revision"

# Restore an old one. The current page says "Version three"; go back to one.
RID=$(pg "SELECT id FROM website_page_revision WHERE page_id=$P AND blocks_json LIKE '%Version one%' ORDER BY id LIMIT 1")
t_nonempty "$RID" "the first version is in the history"
t_eq "200" "$(acode "$ADMIN_SID" POST "$REV/$RID")" "it can be restored"
t_contains "$(curl -s "$BASE/site/ed-page")" 'Version one' "the page went back"

# Restoring is itself a change, so undoing the undo has to work.
t_contains "$(api "$ADMIN_SID" GET "$REV")" 'before restore' \
     "the pre-restore state was snapshotted too"

# A revision id belonging to ANOTHER page must not restore onto this one.
OTHER=$(call website.page create '[{"slug":"ed-other","title":"Other","blocks_json":"[]"}]' | rid)
api "$ADMIN_SID" POST "/site/api/page/$OTHER/blocks" '{"blocks":[{"type":"text","text":"Other page"}]}' >/dev/null
api "$ADMIN_SID" POST "/site/api/page/$OTHER/blocks" '{"blocks":[{"type":"text","text":"Other v2"}]}' >/dev/null
ORID=$(pg "SELECT id FROM website_page_revision WHERE page_id=$OTHER ORDER BY id DESC LIMIT 1")
if [ -n "$ORID" ]; then
    t_eq "404" "$(acode "$ADMIN_SID" POST "$REV/$ORID")" \
         "a revision from another page cannot be restored here"
    t_lacks "$(curl -s "$BASE/site/ed-page")" 'Other page' "and nothing leaked across"
fi

# The history is behind the same group as editing — it holds draft content.
t_eq "401" "$(acode "" GET "$REV")"          "anonymous cannot read the history"
t_eq "403" "$(acode "$PLAIN" GET "$REV")"    "nor staff without the group"
t_eq "403" "$(acode "$PLAIN" POST "$REV/1")" "nor restore from it"

# It is bounded: the last 20, so the table cannot grow without limit.
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
    api "$ADMIN_SID" POST "$API" "{\"blocks\":[{\"type\":\"text\",\"text\":\"Bulk $i\"}]}" >/dev/null
done
KEPT=$(pg "SELECT count(*) FROM website_page_revision WHERE page_id=$P")
t_eq "1" "$(pg "SELECT ($KEPT <= 20)::int")" "the history is capped at 20 (kept $KEPT)"

# ------------------------------------------------------------------
sec "9. E2 — JSON-LD structured data (docs/118)"
# ------------------------------------------------------------------
# the reference ERP emits none: grep -rl "application/ld+json" website/ finds nothing.
LD=$(curl -s "$BASE/site/ed-page")
t_contains "$LD" 'application/ld+json' "the page carries JSON-LD"
t_contains "$LD" 'schema.org'          "against the schema.org context"
t_contains "$LD" '"@type":"Organization"'  "an Organization node"
t_contains "$LD" '"@type":"WebSite"'       "a WebSite node"
t_contains "$LD" '"@type":"BreadcrumbList"' "and a breadcrumb trail"

# A post also gets an Article node with its date and author.
POSTB='[{"type":"text","text":"Body."}]'
PP=$(call website.page create "[{\"slug\":\"ed-post\",\"title\":\"A Post\",\"page_kind\":\"post\",\"author\":\"Ops\",\"publish_date\":\"2026-06-01\",\"blocks_json\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$POSTB"),\"is_published\":true}]" | rid)
PLD=$(curl -s "$BASE/site/ed-post")
t_contains "$PLD" '"@type":"Article"'  "a post gets an Article node"
t_contains "$PLD" '"datePublished"'    "with its publication date"
t_contains "$PLD" '"@type":"Person"'   "and its author"

# THE TRAP hand-rolled JSON-LD always falls into: a title containing a quote
# or a literal closing tag must not break out of the script block.
HOSTILE='</script><script>alert(1)</script>'
PH=$(call website.page create "[{\"slug\":\"ed-ld\",\"title\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$HOSTILE"),\"is_published\":true}]" | rid)
HLD=$(curl -s "$BASE/site/ed-ld")
t_lacks "$HLD" '</script><script>alert(1)</script>' "a hostile title cannot break out of the JSON-LD block"
# It is serialised, so the closing tag survives only as escaped JSON text.
t_contains "$HLD" 'ld+json' "the JSON-LD block is still emitted"
# And the ordinary escaping still holds in the visible page.
t_lacks "$HLD" '<script>alert(1)</script>' "nor appear as markup anywhere on the page"

# ------------------------------------------------------------------
sec "10. THE EDITOR, DRIVEN THROUGH REAL CHROME"
# ------------------------------------------------------------------
# Everything above talks to the HTTP API. That proves the server refuses the
# right callers and says NOTHING about whether the editor works:
# website-editor.js could throw on its first line and every assertion so far
# would still pass. This is the only check that runs it — and the only one
# that can reach the contenteditable paste path, because that path is the DOM.
DRIVE="$R/tests/integration/website/editor/drive.mjs"
# A SEPARATE ungrouped user. ed_plain was granted the group back in §4, so
# handing that login to the browser check asserted the opposite of what it
# said — and passed for the wrong reason until Chrome disagreed.
NP=$(call res.partner create '[{"name":"ED NoEdit","email":"ed_noedit@t.test"}]' | rid)
call res.users create "[{\"login\":\"ed_noedit@t.test\",\"password\":\"NoEdit-Pass-1\",\"partner_id\":$NP,\"active\":true}]" >/dev/null
if command -v node >/dev/null 2>&1 && [ -f "$DRIVE" ]; then
    REP=$(EDIT_SLUG=ed-page PLAIN_LOGIN='ed_noedit@t.test' PLAIN_PASS='NoEdit-Pass-1' \
          SHOT=/tmp/website-editor.png node "$DRIVE" 2>/tmp/ed_drive.err | tail -1)
    SKIP=$(printf '%s' "$REP" | python3 -c 'import sys,json;print(json.loads(sys.stdin.read() or "{}").get("skipped",""))' 2>/dev/null)
    if [ -n "$SKIP" ]; then
        echo "    SKIP  browser editor check: $SKIP"
    else
        g(){ printf '%s' "$REP" | python3 -c "import sys,json;d=json.loads(sys.stdin.read() or '{}');print(d.get('steps',{}).get('$1',''))" 2>/dev/null; }
        t_eq "True" "$(g anonNoBar)"        "no toolbar in a visitor's DOM (not merely hidden)"
        t_eq "True" "$(g anonNoScript)"     "and no editor script or config"
        t_eq "True" "$(g plainNoBar)"       "no toolbar for staff without the group"
        t_eq "True" "$(g barLoads)"         "the toolbar loads for an administrator"
        t_eq "True" "$(g editButton)"       "with an Edit button"
        t_eq "True" "$(g editModeEntered)"  "clicking Edit enters edit mode (the script runs)"
        t_eq "True" "$(g addPaletteShown)"  "and the block palette appears"
        t_eq "True" "$(g savedAndReloaded)" "typing and saving round-trips"
        t_eq "True" "$(g persisted)"        "and the change really persisted"
        t_eq "True" "$(g blockAdded)"       "a block can be added"
        t_eq "True" "$(g blockDeleted)"     "and deleted"
        # The security assertions only a browser can make.
        t_eq "True" "$(g xssNotLiveTag)"    "TYPING a script tag stores it as text, not markup"
        t_eq "True" "$(g xssEscaped)"       "and it comes back escaped"
        t_eq "True" "$(g noAlertFired)"     "no script ran in the browser"
        t_eq "True" "$(g pastedMarkupFlattened)" \
             "PASTED markup is flattened to text by textContent"
        ERRS=$(printf '%s' "$REP" | python3 -c 'import sys,json;print(len(json.loads(sys.stdin.read() or "{}").get("errors",[])))' 2>/dev/null)
        # --- the sidebar (docs/122) ---
        t_eq "True" "$(g sidebarShown)"    "the sidebar is present in edit mode"
        t_eq "True" "$(g topbarShown)"     "so is the editing top bar"
        t_eq "True" "$(g viewBarHidden)"   "and the viewing bar gets out of the way"
        t_eq "True" "$(g outlineListsBlocks)" "the outline lists the page's blocks"
        # The hero bleeds past the text column; when the wrapper did not, the
        # selection outline cut across it and lost its vertical edges.
        t_eq "True" "$(g noBlockOverflowsWrapper)" \
             "no block escapes its wrapper, so the selection outline surrounds it"
        t_eq "True" "$(g clickSelects)"    "clicking a block on the page selects it"
        t_eq "True" "$(g customizeOpened)" "…and opens Customize on it"
        t_eq "True" "$(g customizeHasControls)" "which offers that block's fields"
        t_eq "True" "$(g addSelectsNewBlock)" \
             "adding a block selects it and opens its settings"
        # The reason the sidebar exists: in-place editing can only ever reach
        # what is rendered, so a heading's LEVEL — a tag name, not text — had
        # no editable surface anywhere before this.
        t_eq "True" "$(g offPageFieldEditable)" \
             "a field with NO inline representation is editable"
        t_eq "True" "$(g offPageFieldApplied)" \
             "…and changing it re-renders the page"

        t_eq "0" "$ERRS" "no console or page errors throughout"
        [ "$ERRS" != "0" ] && echo "    $REP"
    fi
else
    echo "    SKIP  browser editor check: node or drive.mjs unavailable"
fi

# ------------------------------------------------------------------
sec "11. the ERP and the draft rules are untouched"
# ------------------------------------------------------------------
D=$(call website.page create '[{"slug":"ed-draft","title":"ED Draft","is_published":false}]' | rid)
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/site/ed-draft")" \
     "a draft is still 404 to the public"
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' -H "Cookie: session_id=$ADMIN_SID" "$BASE/site/ed-draft")" \
     "and still previewable by staff"
t_eq "200" "$(http_code /healthz)" "the ERP is healthy"

verdict

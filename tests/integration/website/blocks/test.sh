#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# The content blocks (docs/125) — video, gallery, quote, numbers, call to
# action, table, spacer — and the DAILY USE of the editor.
#
# Two halves:
#
#   §1–§8  every block type, through the real save endpoint and out of the
#          public renderer. Each block gets its happy path AND its hostile
#          one, because a block is a place where author text becomes markup.
#   §9     journey.mjs — a full editing session in Chrome: add blocks, type
#          into them, upload a picture, reorder, delete, save, then read the
#          page back as a VISITOR. That is what somebody actually does, and
#          none of it is reachable from curl.
# =============================================================
auth_or_die
ADMIN_SID="$SID"

cleanup() {
    pg "DELETE FROM website_page_revision WHERE page_id IN (SELECT id FROM website_page WHERE slug LIKE 'bk-%')" >/dev/null
    pg "DELETE FROM website_page   WHERE slug LIKE 'bk-%'" >/dev/null
    pg "DELETE FROM ir_attachment  WHERE res_model='website'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

P=$(call website.page create '[{"slug":"bk-page","title":"Blocks","is_published":true}]' | rid)
t_nonempty "$P" "a page to put blocks on"
[ -z "$P" ] && { verdict; exit 1; }
API="/site/api/page/$P/blocks"

# save <json-array>  -> http code
save() {
    curl -s -o /dev/null -w '%{http_code}' -X POST -H "Cookie: session_id=$ADMIN_SID" \
         -H 'Content-Type: application/json' --data "{\"blocks\":$1}" "$BASE$API"
}
# render <json-array> -> the public HTML
render() { save "$1" >/dev/null; curl -s "$BASE/site/bk-page"; }

# ------------------------------------------------------------------
sec "1. video — a provider link becomes an embed we built"
# ------------------------------------------------------------------
H=$(render '[{"type":"video","src":"https://www.youtube.com/watch?v=dQw4w9WgXcQ","caption":"A tour"}]')
t_contains "$H" 'youtube-nocookie.com/embed/dQw4w9WgXcQ' \
     "a YouTube link renders on the no-cookie embed host"
t_lacks    "$H" 'youtube.com/watch'  "the author's URL is not echoed anywhere"
t_contains "$H" 'allowfullscreen'    "the frame can go fullscreen"
t_contains "$H" 'sandbox='           "and is sandboxed"
t_contains "$H" 'A tour'             "the caption renders"

H=$(render '[{"type":"video","src":"https://vimeo.com/123456789"}]')
t_contains "$H" 'player.vimeo.com/video/123456789' "a Vimeo link too"

# ------------------------------------------------------------------
sec "2. video — a URL that is not a video we trust renders NOTHING"
# ------------------------------------------------------------------
for bad in \
  'https://youtube.com.evil.example/watch?v=abc' \
  'https://notyoutube.com/watch?v=abc' \
  'javascript:alert(1)' ; do
    H=$(render "[{\"type\":\"video\",\"src\":\"$bad\"}]")
    t_lacks "$H" 'youtube-nocookie' "refused: $bad"
    t_lacks "$H" 'evil.example'     "…and its host never reaches the page"
done
H=$(render '[{"type":"video","src":"javascript:alert(1)"}]')
t_lacks "$H" 'javascript:' "a javascript: URL is not emitted as a video source either"

# A file we host is played inline rather than framed.
H=$(render '[{"type":"video","src":"/site/media/1","caption":"Hosted"}]')
t_contains "$H" '<video'   "a hosted file is played with a video element"
t_contains "$H" 'controls' "with controls"
t_lacks    "$H" '<iframe'  "and no frame"

# ------------------------------------------------------------------
sec "3. gallery"
# ------------------------------------------------------------------
H=$(render '[{"type":"gallery","items":[
  {"src":"/site/media/1","alt":"A unit","caption":"Inside"},
  {"src":"/site/media/2","alt":"Corridor"}]}]')
t_contains "$H" 'w-gal'      "a gallery renders"
t_contains "$H" 'A unit'     "with its alt text"
t_contains "$H" 'Inside'     "and its caption"
# Count the real TAGS, not the class name: the stylesheet mentions .w-gal-i
# four times, so grepping the bare class scores the CSS as well as the page.
t_eq "2" "$(printf '%s' "$H" | grep -o '<figure class="w-gal-i"' | wc -l)" \
     "one figure per picture"

# A picture with an unsafe source is DROPPED, not rendered broken.
H=$(render '[{"type":"gallery","items":[
  {"src":"javascript:alert(1)","alt":"x"},{"src":"/site/media/1","alt":"ok"}]}]')
t_lacks    "$H" 'javascript:' "an unsafe source is dropped"
t_contains "$H" 'ok'          "…without taking the rest of the gallery with it"

# ------------------------------------------------------------------
sec "4. quote, numbers, call to action"
# ------------------------------------------------------------------
H=$(render '[{"type":"quote","text":"They had a unit free the same day.","author":"Aisha R.","role":"Moved in June"}]')
t_contains "$H" 'w-quote'                   "a quote renders"
t_contains "$H" 'the same day'              "with its text"
t_contains "$H" 'Aisha R.'                  "and attribution"
t_contains "$H" 'Moved in June'             "and a role"

H=$(render '[{"type":"stats","items":[{"value":"250","label":"Units"},{"value":"7 days","label":"Access"}]}]')
t_contains "$H" 'w-stats'  "numbers render"
t_contains "$H" '250'      "with the value"
t_contains "$H" 'Units'    "and the label"

H=$(render '[{"type":"cta","headline":"Need one this week?","text":"We answer same day.","cta_text":"Ask","cta_href":"/site/contact"}]')
t_contains "$H" 'w-cta'                "a call to action renders"
t_contains "$H" 'href="/site/contact"' "with a working link"
# An unsafe link means no button, not a dangerous one.
H=$(render '[{"type":"cta","headline":"X","cta_text":"Go","cta_href":"javascript:alert(1)"}]')
t_lacks    "$H" 'javascript:' "an unsafe button link is dropped"
t_contains "$H" 'w-cta'       "…and the rest of the block still renders"

# ------------------------------------------------------------------
sec "5. table"
# ------------------------------------------------------------------
H=$(render '[{"type":"table","header":true,"items":[
  {"cells":["Size","Price","Free"]},
  {"cells":["50 sq ft","RM 190","Yes"]},
  {"cells":["90 sq ft","RM 310","No"]}]}]')
t_contains "$H" '<thead>'    "the first row becomes a header"
t_contains "$H" '<th>Size'   "…as header cells"
t_contains "$H" '<td>50 sq ft' "and the rest as data cells"
t_contains "$H" 'w-table-wrap' "wrapped so a wide table scrolls inside itself"
# Three <tr> in total: the header row plus two body rows.
t_eq "3" "$(printf '%s' "$H" | grep -o '<tr>' | wc -l)" "one header row and two body rows"
t_eq "6" "$(printf '%s' "$H" | grep -o '<td>' | wc -l)" "six data cells"

H=$(render '[{"type":"table","header":false,"items":[{"cells":["a","b"]}]}]')
t_lacks "$H" '<thead>' "header:false means no header row"

# ------------------------------------------------------------------
sec "6. spacer"
# ------------------------------------------------------------------
for size in small medium large; do
    H=$(render "[{\"type\":\"spacer\",\"size\":\"$size\"}]")
    t_contains "$H" 'w-sp-' "a $size spacer renders"
done
H=$(render '[{"type":"spacer","size":"enormous"}]')
t_contains "$H" 'w-sp-m' "an unknown size falls back to medium rather than emitting nothing"

# ------------------------------------------------------------------
sec "7. EVERY new block escapes author text"
# ------------------------------------------------------------------
# The blocks are new; the rule they have to obey is not. Author text becomes
# TEXT, never markup, in every field of every one of them.
X='<script>alert(1)</script>'
H=$(render "[
 {\"type\":\"quote\",\"text\":\"$X\",\"author\":\"$X\",\"role\":\"$X\"},
 {\"type\":\"stats\",\"items\":[{\"value\":\"$X\",\"label\":\"$X\"}]},
 {\"type\":\"cta\",\"headline\":\"$X\",\"text\":\"$X\",\"cta_text\":\"$X\",\"cta_href\":\"/site\"},
 {\"type\":\"table\",\"items\":[{\"cells\":[\"$X\"]}]},
 {\"type\":\"gallery\",\"items\":[{\"src\":\"/site/media/1\",\"alt\":\"$X\",\"caption\":\"$X\"}]},
 {\"type\":\"video\",\"src\":\"https://youtu.be/dQw4w9WgXcQ\",\"caption\":\"$X\"}]")
t_lacks    "$H" '<script>alert(1)</script>' "no live script tag anywhere"
t_contains "$H" '&lt;script&gt;'            "…because it was escaped to text"
# The caption is used as an iframe title attribute, so it must not break out.
t_lacks "$H" 'title="<'                     "a caption cannot break out of the frame title"

# ------------------------------------------------------------------
sec "8. the server still refuses a block type it does not know"
# ------------------------------------------------------------------
t_eq "400" "$(save '[{"type":"marquee","text":"hi"}]')" "an unknown type is refused on save"
t_eq "400" "$(save '[{"type":"iframe","src":"https://evil.example"}]')" \
     "and so is one that sounds like a way in"

# ------------------------------------------------------------------
sec "9. A DAY'S EDITING, in a real browser"
# ------------------------------------------------------------------
# Start from an empty page. The sections above left blocks pointing at
# /site/media/1, which does not exist — the browser would report those 404s as
# console errors and the journey's own "no errors" assertion would be scoring
# this file's fixtures rather than the editor.
save '[]' >/dev/null
if command -v node >/dev/null 2>&1 && [ -f tests/integration/website/blocks/journey.mjs ]; then
    REP=$(BASE="$BASE" DBN="${DBN:-odoo}" EDIT_SLUG="bk-page" \
          node tests/integration/website/blocks/journey.mjs 2>&1 | tail -1)
    SKIP=$(printf '%s' "$REP" | python3 -c "import sys,json;print(json.loads(sys.stdin.read() or '{}').get('skipped',''))" 2>/dev/null)
    if [ -n "$SKIP" ]; then
        echo "    SKIP  browser journey: $SKIP"
    else
        j(){ printf '%s' "$REP" | python3 -c "import sys,json;d=json.loads(sys.stdin.read() or '{}');print(d.get('steps',{}).get('$1',''))" 2>/dev/null; }
        t_eq "True" "$(j enteredEdit)"       "the editor opens"
        t_eq "True" "$(j addedHeading)"      "a heading can be added and typed into"
        t_eq "True" "$(j addedQuote)"        "a quote can be added and typed into"
        t_eq "True" "$(j addedStats)"        "numbers can be added"
        t_eq "True" "$(j addedTable)"        "a table can be added and its cells typed into"
        t_eq "True" "$(j addedVideo)"        "a video link can be set from the sidebar"
        t_eq "True" "$(j uploadedIntoGallery)" "a picture can be uploaded into a gallery"
        t_eq "True" "$(j reordered)"         "a block can be moved"
        t_eq "True" "$(j deleted)"           "a block can be deleted"
        t_eq "True" "$(j saved)"             "the page saves"
        # Everything above is worthless if the visitor does not get it.
        t_eq "True" "$(j visitorSeesHeading)" "a VISITOR sees the edited heading"
        t_eq "True" "$(j visitorSeesQuote)"   "…the quote"
        t_eq "True" "$(j visitorSeesTable)"   "…the table cells that were typed"
        t_eq "True" "$(j visitorSeesVideo)"   "…the video, as a no-cookie embed"
        t_eq "True" "$(j visitorSeesImage)"   "…and the uploaded picture"
        t_eq "True" "$(j deletedGone)"        "and does NOT see the deleted block"
        t_eq "0" "$(j errorCount)"            "no console or page errors all session"
        [ "$(j errorCount)" != "0" ] && echo "    $REP"
    fi
else
    echo "    SKIP  browser journey: node or journey.mjs unavailable"
fi

verdict

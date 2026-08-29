#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# Product -> Configuration -> Categories: the tree screen.
#
# Two endpoints back it, and each answers a question the old flat list could
# not:
#
#   tree    the whole hierarchy in one call, every node carrying BOTH counts —
#           `direct` (filed exactly here) and `total` (everything beneath).
#   detail  everything the right-hand panel shows for one category, in one
#           call: the path up to the root, the four counts, the children, a
#           page of products, and the inventory accounts.
#
# The counts are where this earns its keep. A rolled-up total that disagrees
# with the sum of its parts is invisible on screen and destroys trust in the
# figure, so the arithmetic is checked against the database directly rather
# than against the endpoint's own other fields.
#
# Everything is prefixed CT- / 'CT ' and removed on the way out.
# =============================================================
auth_or_die

py() { python3 -c "$1" 2>/dev/null; }

cleanup() {
    pg "DELETE FROM product_product  WHERE default_code LIKE 'CT-%'" >/dev/null
    pg "DELETE FROM product_template WHERE default_code LIKE 'CT-%'" >/dev/null
    # Children first: the tree is deleted from the leaves up.
    pg "DELETE FROM product_category WHERE name LIKE 'CT %' AND parent_id IS NOT NULL" >/dev/null
    pg "DELETE FROM product_category WHERE name LIKE 'CT %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

# ------------------------------------------------------------------
sec "1. a small hierarchy to measure"
# ------------------------------------------------------------------
#   CT Root
#     CT Branch
#       CT Leaf      <- 2 products
#     CT Empty       <- none
ROOT=$(call product.category create '[{"name":"CT Root"}]' | rid)
t_nonempty "$ROOT" "a root category was created"
[ -z "$ROOT" ] && { verdict; exit 1; }
BRANCH=$(call product.category create "[{\"name\":\"CT Branch\",\"parent_id\":$ROOT}]"   | rid)
LEAF=$(call product.category   create "[{\"name\":\"CT Leaf\",\"parent_id\":$BRANCH}]"   | rid)
EMPTY=$(call product.category  create "[{\"name\":\"CT Empty\",\"parent_id\":$ROOT}]"    | rid)
t_nonempty "$BRANCH" "a child category was created"
t_nonempty "$LEAF"   "a grandchild category was created"
t_nonempty "$EMPTY"  "a second, empty child was created"
t_eq "$ROOT"   "$(pg "SELECT parent_id FROM product_category WHERE id=$BRANCH")" "the child points at its parent"
t_eq "$BRANCH" "$(pg "SELECT parent_id FROM product_category WHERE id=$LEAF")"   "and the grandchild at the child"

UOM=$(pg "SELECT id FROM uom_uom ORDER BY id LIMIT 1")
for n in 1 2; do
    T=$(pgid "INSERT INTO product_template (name, default_code, type, categ_id, uom_id, uom_po_id,
              list_price, standard_price, active, sale_ok, purchase_ok, company_id)
              VALUES ('CT Part $n','CT-P$n','product',$LEAF,$UOM,$UOM,1500000,0,true,true,true,1) RETURNING id")
    pg "INSERT INTO product_product (name, default_code, type, categ_id, uom_id, uom_po_id,
        list_price, standard_price, qty_available, active, sale_ok, purchase_ok, company_id, product_tmpl_id)
        VALUES ('CT Part $n','CT-P$n','product',$LEAF,$UOM,$UOM,1500000,0,0,true,true,true,1,$T)" >/dev/null
done
t_eq "2" "$(pg "SELECT count(*) FROM product_product WHERE categ_id=$LEAF")" "two products are filed in the leaf"

# ------------------------------------------------------------------
sec "2. tree — one call returns the whole hierarchy"
# ------------------------------------------------------------------
TREE=$(call product.category tree '[{}]')
has_error "$TREE" && { no "tree failed: $(echo "$TREE" | head -c 200)"; verdict; exit 1; }

NODES=$(echo "$TREE" | py "
import json,sys
print(len(json.load(sys.stdin)['result']['nodes']))")
DBN_COUNT=$(pg "SELECT count(*) FROM product_category WHERE active = TRUE")
t_eq "$DBN_COUNT" "$NODES" "every active category is in the tree"

# Each node must carry what the screen draws: without parent_id there is no
# hierarchy to build, and without both counts the badges cannot be rendered.
FIELDS=$(echo "$TREE" | py "
import json,sys
n=json.load(sys.stdin)['result']['nodes'][0]
need=['id','name','parent_id','active','direct_count','total_count','child_count']
print(','.join(k for k in need if k not in n) or 'all')")
t_eq "all" "$FIELDS" "every node carries the fields the tree needs"

node_field() {  # node_field <id> <field>
    echo "$TREE" | py "
import json,sys
for n in json.load(sys.stdin)['result']['nodes']:
    if n['id'] == $1: print(n['$2']); break"
}
t_eq "$BRANCH" "$(node_field "$LEAF" parent_id)" "the leaf reports its parent"
t_eq "0"       "$(node_field "$ROOT" parent_id)" "a root reports parent 0, not null"
t_eq "2"       "$(node_field "$ROOT" child_count)" "the root reports two direct children"

# ------------------------------------------------------------------
sec "3. the two counts, checked against the database"
# ------------------------------------------------------------------
# This is the assertion the screen lives or dies by: a parent must roll up
# what its descendants hold, while still reporting 0 for itself.
t_eq "2" "$(node_field "$LEAF" direct_count)"   "the leaf counts its own two products"
t_eq "2" "$(node_field "$LEAF" total_count)"    "and its total is the same (it has no children)"
t_eq "0" "$(node_field "$BRANCH" direct_count)" "the branch holds nothing directly"
t_eq "2" "$(node_field "$BRANCH" total_count)"  "but rolls up the leaf's two"
t_eq "0" "$(node_field "$ROOT" direct_count)"   "the root holds nothing directly"
t_eq "2" "$(node_field "$ROOT" total_count)"    "and rolls up everything beneath it"
t_eq "0" "$(node_field "$EMPTY" total_count)"   "an empty branch rolls up nothing"

# The roll-up must equal what a plain recursive query says, not merely be
# self-consistent with the endpoint's other numbers.
REAL=$(pg "WITH RECURSIVE d AS (
             SELECT $ROOT::int AS node
             UNION ALL SELECT c.id FROM product_category c JOIN d ON c.parent_id = d.node)
           SELECT count(*) FROM product_product WHERE categ_id IN (SELECT node FROM d)")
t_eq "$REAL" "$(node_field "$ROOT" total_count)" "the roll-up agrees with the database"

# ------------------------------------------------------------------
sec "4. archived categories are excluded unless asked for"
# ------------------------------------------------------------------
pg "UPDATE product_category SET active = FALSE WHERE id=$EMPTY" >/dev/null
HIDDEN=$(call product.category tree '[{}]' | py "
import json,sys
print(sum(1 for n in json.load(sys.stdin)['result']['nodes'] if n['id'] == $EMPTY))")
t_eq "0" "$HIDDEN" "an archived category is left out by default"
SHOWN=$(call_k product.category tree '[{}]' '"include_archived":true' | py "
import json,sys
print(sum(1 for n in json.load(sys.stdin)['result']['nodes'] if n['id'] == $EMPTY))")
t_eq "1" "$SHOWN" "and included when the screen asks for archived"
pg "UPDATE product_category SET active = TRUE WHERE id=$EMPTY" >/dev/null

# ------------------------------------------------------------------
sec "5. detail — the right-hand panel, in one call"
# ------------------------------------------------------------------
D=$(call product.category detail "[{\"id\":$LEAF}]")
has_error "$D" && { no "detail failed: $(echo "$D" | head -c 200)"; verdict; exit 1; }

t_eq "CT Leaf" "$(echo "$D" | py "
import json,sys
print(json.load(sys.stdin)['result']['name'])")" "it names the category"

# The breadcrumb has to run root-first, or the panel reads backwards.
PATH_STR=$(echo "$D" | py "
import json,sys
print(' / '.join(p['name'] for p in json.load(sys.stdin)['result']['path']))")
t_eq "CT Root / CT Branch / CT Leaf" "$PATH_STR" "the path runs from the root down to the category"

for k in direct total children descendants; do
    V=$(echo "$D" | py "
import json,sys
print(json.load(sys.stdin)['result']['counts']['$k'])")
    t_nonempty "$V" "counts.$k is present"
done
t_eq "2" "$(echo "$D" | py "
import json,sys
print(json.load(sys.stdin)['result']['counts']['direct'])")" "it counts the two products"

# Products, with prices already in major units — the panel must not have to
# know about micro-unit storage.
PCOUNT=$(echo "$D" | py "
import json,sys
print(len(json.load(sys.stdin)['result']['products']))")
t_eq "2" "$PCOUNT" "both products are listed"
PRICE=$(echo "$D" | py "
import json,sys
print(json.load(sys.stdin)['result']['products'][0]['list_price'])")
t_eq "1.5" "$PRICE" "the price is scaled to major units (1.5, not 1500000)"

# A parent's detail lists its children so the panel can offer them as chips.
DP=$(call product.category detail "[{\"id\":$ROOT}]")
KIDS=$(echo "$DP" | py "
import json,sys
print(','.join(sorted(c['name'] for c in json.load(sys.stdin)['result']['children_list'])))")
t_eq "CT Branch,CT Empty" "$KIDS" "a parent lists its direct children"
# Three, not two: Branch, Leaf and Empty all sit beneath the root. `children`
# counts one level, `descendants` counts the whole subtree — the panel shows
# both precisely because they differ.
t_eq "3" "$(echo "$DP" | py "
import json,sys
print(json.load(sys.stdin)['result']['counts']['descendants'])")" "and counts all three descendants"
t_eq "2" "$(echo "$DP" | py "
import json,sys
print(json.load(sys.stdin)['result']['counts']['children'])")" "while direct children stay at two"

# The accounting block must always be present, even when nothing is set —
# the panel renders "Not set" rather than hiding the section.
ACC=$(echo "$D" | py "
import json,sys
a=json.load(sys.stdin)['result']['accounts']
print(','.join(sorted(a.keys())))")
t_eq "input,journal,output,valuation" "$ACC" "the accounting properties are reported"

# ------------------------------------------------------------------
sec "6. detail refuses what it cannot show"
# ------------------------------------------------------------------
BAD=$(call product.category detail '[{"id":999999999}]')
has_error "$BAD" && ok "an unknown category id is refused" || no "detail accepted a non-existent id"
t_lacks "$BAD" "SELECT " "and the refusal does not leak SQL"
NOID=$(call product.category detail '[{}]')
has_error "$NOID" && ok "a missing id is refused" || no "detail accepted a call with no id"

# ------------------------------------------------------------------
sec "7. the writes the screen makes"
# ------------------------------------------------------------------
# Create a sub-category the way the panel's "Add sub-category" does, and check
# it appears in the parent's subtree immediately.
NEW=$(call product.category create "[{\"name\":\"CT Added\",\"parent_id\":$BRANCH}]" | rid)
t_nonempty "$NEW" "a sub-category can be created under a selected node"
AFTER=$(call product.category tree '[{}]' | py "
import json,sys
for n in json.load(sys.stdin)['result']['nodes']:
    if n['id'] == $NEW: print(n['parent_id']); break")
t_eq "$BRANCH" "$AFTER" "it appears in the tree under the right parent"
t_eq "2" "$(call product.category detail "[{\"id\":$BRANCH}]" | py "
import json,sys
print(json.load(sys.stdin)['result']['counts']['children'])")" "the parent's child count went up"

call product.category write "[[$NEW],{\"name\":\"CT Renamed\"}]" >/dev/null
# Asserted as a row count, not by reading the name back: pg() strips spaces,
# so "CT Renamed" returns as "CTRenamed" and a string comparison fails on a
# rename that worked perfectly.
t_eq "1" "$(pg "SELECT count(*) FROM product_category WHERE id=$NEW AND name='CT Renamed'")" \
     "rename writes through"

call product.category unlink "[[$NEW]]" >/dev/null
t_eq "0" "$(pg "SELECT count(*) FROM product_category WHERE id=$NEW")" "delete removes it"
GONE=$(call product.category tree '[{}]' | py "
import json,sys
print(sum(1 for n in json.load(sys.stdin)['result']['nodes'] if n['id'] == $NEW))")
t_eq "0" "$GONE" "and it leaves the tree"

# ------------------------------------------------------------------
sec "8. the screen is registered and its assets are served"
# ------------------------------------------------------------------
# The component only reaches the browser if index.html loads it AND app.js
# maps the model to it. Both have been forgotten before; neither shows up as
# a server error, only as an empty screen.
#
# The checks in this section are static — they prove the asset is served and
# wired, not that anything renders. Section 9 is the one that opens a browser.
JS=$(http_code "/src/components/CategoryTree.js")
t_eq "200" "$JS" "CategoryTree.js is served"
IDX=$(http_get "/index.html")
t_contains "$IDX" "components/CategoryTree.js" "index.html loads it"
APP=$(http_get "/src/app.js")
t_contains "$APP" "'product.category':   CategoryTree" "app.js maps product.category to it"
CSS=$(http_get "/src/app.css")
t_contains "$CSS" ".ct-shell" "the stylesheet carries the screen's rules"

# The template is parsed as XML, so a stray control character or a '--' inside
# a comment stops the whole app from compiling — silently, at load time.
BODY=$(http_get "/src/components/CategoryTree.js")
if echo "$BODY" | grep -q '<!--[^>]*--[^>]*-->'; then
    no "a template comment contains '--', which breaks XML parsing"
else
    ok "no '--' inside template comments"
fi

# The file must at least be valid JavaScript. This does not prove the OWL
# template compiles — only a browser can show that — but a syntax error here
# takes the whole front end down with no server-side symptom at all.
if command -v node >/dev/null 2>&1; then
    if node --check web/static/src/components/CategoryTree.js 2>/dev/null; then
        ok "CategoryTree.js parses as JavaScript"
    else
        no "CategoryTree.js has a syntax error: $(node --check web/static/src/components/CategoryTree.js 2>&1 | head -2)"
    fi
    node --check web/static/src/app.js 2>/dev/null \
        && ok "app.js still parses after the registration" \
        || no "app.js has a syntax error"
else
    echo "    NOTE  node is not installed — the JavaScript was not syntax-checked"
fi

# Every element the stylesheet styles must exist in the template, and vice
# versa: a class in one and not the other is a rule that never applies or an
# element with no styling, and both look like "the screen is broken".
for cls in ct-shell ct-side ct-grip ct-main ct-row ct-twist ct-stat ct-crumb; do
    inJs=$(grep -c "\"[^\"]*$cls" web/static/src/components/CategoryTree.js)
    inCss=$(grep -c "\.$cls" web/static/src/app.css)
    if [ "${inJs:-0}" -gt 0 ] && [ "${inCss:-0}" -gt 0 ]; then
        ok "$cls is both rendered and styled"
    else
        no "$cls: in template=$inJs, in stylesheet=$inCss"
    fi
done

# ------------------------------------------------------------------
sec "9. it actually renders in a browser"
# ------------------------------------------------------------------
# The only check here that would catch an OWL template error. Everything above
# passes happily while the panel renders nothing: the template is parsed as XML
# in the CLIENT, so a bad template throws in the browser and is invisible
# server-side.
#
# It drives real Chrome through the real menus — Products -> Configuration ->
# Categories — because this app has no hash router; a URL cannot reach a screen
# (tests/docs/browser-render-checks.md).
if [ ! -d node_modules/puppeteer-core ]; then
    echo "    NOTE  puppeteer-core is not installed — rendering was NOT checked."
    echo "          npm i -D puppeteer-core     (see tests/docs/browser-render-checks.md)"
elif [ ! -x /usr/bin/google-chrome ]; then
    echo "    NOTE  Chrome is not at /usr/bin/google-chrome — rendering was NOT checked."
else
    OUT=$(SHOT=/tmp/category-tree.png timeout 180 node tests/lib/render.mjs \
          Products Configuration Categories .ct-shell 2>&1)
    RC=$?
    jq_() { echo "$OUT" | python3 -c "
import json,sys
raw = sys.stdin.read()
s = raw.find('{')
d = json.loads(raw[s:raw.rfind('}')+1]) if s >= 0 else {}
cur = d
for k in '$1'.split('.'):
    cur = (cur or {}).get(k) if isinstance(cur, dict) else None
print(cur if cur is not None else '')" 2>/dev/null; }

    [ "$RC" = "0" ] && ok "the screen renders with no console errors" \
                    || no "render check failed (exit $RC): $(echo "$OUT" | tail -3 | tr '\n' ' ')"
    t_eq "True" "$(jq_ found)"         "the tree shell appeared after clicking through the menus"
    t_ge "$(jq_ dom.rows)" 3           "the tree drew rows"
    t_ge "$(jq_ dom.twists)" 1         "at least one node is expandable"
    t_eq "True" "$(jq_ dom.grip)"      "the resize handle is present"
    t_ge "$(jq_ dom.sidebar)" 180      "the sidebar has a real width"
    t_eq "True" "$(jq_ detail.panel)"  "clicking a category opened the detail panel"
    t_ge "$(jq_ detail.crumbs)" 1      "the breadcrumb rendered"
    STATS=$(jq_ detail.stats)
    t_contains "$STATS" "," "the four stat tiles rendered ($STATS)"
    echo "    screenshot: /tmp/category-tree.png"
fi

verdict

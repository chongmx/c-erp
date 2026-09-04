#!/bin/bash
# --- harness ---------------------------------------------------------------
# Walk up for CMakeLists.txt rather than counting `../`, so this test behaves
# the same whether the runner invoked it or you ran it directly, and so it can
# be nested a folder deeper without a preamble edit.
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# The Help Centre (docs/101).
#
# Help is stored as data rather than as static pages because an AI assistant is
# meant to answer from it later. That intent creates obligations this script
# checks, because none of them are visible by reading a rendered page:
#
#   * every article has a UNIQUE, STABLE slug — it is the address a deep link
#     uses and the citation an assistant will hand back ("see Filling in your
#     timesheet"). A duplicate or missing slug breaks both.
#   * no article is orphaned. An article whose section vanished must still be
#     reachable, or it becomes content nothing can ever link to.
#   * `related` never comes back empty. Keyword overlap alone is sparse, and an
#     empty panel reads as broken rather than as "nothing related".
#   * the shipped content is re-seeded on every start, so a corrected article
#     reaches installs that already have the old text.
#
# The markdown renderer lives in the browser, so the checks here are on the
# CONTENT it will be given: that bodies are non-trivial, that the seeded
# markdown parses into the block types the article claims to use, and that no
# body smuggles raw HTML into a renderer that is supposed to escape it.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
pgraw(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null; }

SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; echo "*** FAILURES ***"; exit 1; }
CTX="\"context\":{\"session_id\":\"$SID\"}"
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"help.article\",\"method\":\"$1\",\"args\":[$2],\"kwargs\":{$CTX}}}"; }
py(){ python3 -c "$1" 2>/dev/null; }
export PYTHONIOENCODING=utf-8

echo "############ content is present ############"
N=$(pg "SELECT count(*) FROM help_article WHERE NOT is_section")
[ -n "$N" ] && [ "$N" -ge 10 ] && ok "$N articles seeded" || no "only $N articles"
S=$(pg "SELECT count(*) FROM help_article WHERE is_section")
[ -n "$S" ] && [ "$S" -ge 4 ] && ok "$S sections seeded" || no "only $S sections"

echo "############ slugs are stable addresses ############"
DUP=$(pg "SELECT count(*) FROM (SELECT slug FROM help_article GROUP BY slug HAVING count(*)>1) t")
[ "$DUP" = "0" ] && ok "no duplicate slugs" || no "$DUP duplicated slugs"
[ "$(pg "SELECT count(*) FROM help_article WHERE COALESCE(slug,'')=''")" = "0" ] \
  && ok "every row has a slug" || no "some rows have no slug"
[ "$(pg "SELECT count(*) FROM help_article WHERE slug !~ '^[a-z0-9-]+$'")" = "0" ] \
  && ok "slugs are URL-safe" || no "some slugs are not URL-safe"
# The unique index is what actually enforces it, not just the seed being careful.
[ "$(pg "SELECT count(*) FROM pg_indexes WHERE indexname='help_article_slug_uniq'")" = "1" ] \
  && ok "a unique index enforces slug uniqueness" || no "no unique index on slug"

echo "############ nothing is orphaned ############"
[ "$(pg "SELECT count(*) FROM help_article WHERE NOT is_section AND parent_id IS NULL")" = "0" ] \
  && ok "every article belongs to a section" || no "orphaned articles exist"
[ "$(pg "SELECT count(*) FROM help_article a WHERE a.parent_id IS NOT NULL
          AND NOT EXISTS (SELECT 1 FROM help_article p WHERE p.id=a.parent_id)")" = "0" ] \
  && ok "no article points at a missing section" || no "dangling parent_id"
[ "$(pg "SELECT count(*) FROM help_article a JOIN help_article p ON p.id=a.parent_id
          WHERE a.book <> p.book")" = "0" ] \
  && ok "no article sits in a section from another book" || no "cross-book section links"

echo "############ books ############"
B=$(call books '{}')
NB=$(echo "$B" | py "
import json,sys
print(len(json.load(sys.stdin)['result']))")
[ -n "$NB" ] && [ "$NB" -ge 10 ] && ok "$NB books offered as tabs" || no "only $NB books"
# A module must get a tab whether or not its help is written — a module silently
# missing from the bar looks like a bug, while an empty tab is information.
#
# Asserting "at least one book is empty" was the wrong test: it passed only
# while books were undocumented and started failing the moment they were all
# filled in. What matters is that the bar is driven by the configured module
# list, not by which books happen to have rows.
ALLBOOKS=$(echo "$B" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(all(b.get('label') and b.get('slug') for b in d))")
[ "$ALLBOOKS" = "True" ] && ok "every tab has a slug and a label" || no "a book is missing slug or label"
for SLUG in project parts product stock sale purchase account mrp rental hr report base settings help; do
    echo "$B" | grep -q "\"slug\":\"$SLUG\"" && ok "'$SLUG' has a tab" || no "'$SLUG' has no tab"
done
echo "$B" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(next((b['count'] for b in d if b['slug']=='project'),0))" | grep -qE '^[1-9]' \
  && ok "the Project book reports its article count" || no "Project book count missing"

echo "############ tree ############"
T=$(call tree '{"book":"project"}')
NS=$(echo "$T" | py "
import json,sys
print(len(json.load(sys.stdin)['result']))")
[ -n "$NS" ] && [ "$NS" -ge 4 ] && ok "the Project tree has $NS sections" || no "tree has $NS sections"
NA=$(echo "$T" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(sum(len(s['articles']) for s in d))")
[ -n "$NA" ] && [ "$NA" -ge 10 ] && ok "the tree exposes $NA articles" || no "tree exposes $NA articles"
# The tree drives the sidebar order, so it must be deterministic.
ORD=$(echo "$T" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(d[0]['title'])")
[ "$ORD" = "Getting started" ] && ok "sections come back in sequence order" || no "first section is '$ORD'"
call tree '{}' | grep -qi 'book is required' && ok "tree requires a book" || no "tree accepted no book"

echo "############ article ############"
A=$(call article '{"slug":"project-overview"}')
echo "$A" | grep -q '"title"' && ok "an article can be fetched by slug" || no "article fetch failed"
echo "$A" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(len(d['body']))" | grep -qE '^[0-9]{3,}' && ok "the body is substantial" || no "body is tiny or empty"
echo "$A" | grep -q '"section_title":"Getting started"' \
  && ok "the article carries its section for the breadcrumb" || no "no section on the article"
call article '{"slug":"does-not-exist"}' | grep -qi 'No such help article' \
  && ok "an unknown slug is a clean error" || no "unknown slug not reported"
call article '{}' | grep -qi 'slug is required' && ok "article requires a slug" || no "article accepted no slug"

echo "############ search ############"
H=$(call search '{"q":"timesheet"}')
NH=$(echo "$H" | py "
import json,sys
print(len(json.load(sys.stdin)['result']))")
[ -n "$NH" ] && [ "$NH" -ge 2 ] && ok "searching 'timesheet' finds $NH articles" || no "search found $NH"
# A title hit must outrank a body mention — a word in a title is what the
# article is about; in a body it may be an aside.
TOP=$(echo "$H" | py "
import json,sys
d=json.load(sys.stdin)['result']
print(d[0]['rank'] >= max(x['rank'] for x in d))")
[ "$TOP" = "True" ] && ok "results are ranked, best first" || no "ranking is not applied"
echo "$H" | grep -q '"excerpt"' && ok "hits carry an excerpt" || no "no excerpt on hits"
[ "$(call search '{"q":"a"}' | py "
import json,sys
print(len(json.load(sys.stdin)['result']))")" = "0" ] \
  && ok "a one-character query returns nothing" || no "single-character search ran"
BK=$(call search '{"q":"help","book":"project"}' | py "
import json,sys
d=json.load(sys.stdin)['result']
print(all(x['book']=='project' for x in d))")
[ "$BK" = "True" ] && ok "search stays inside the requested book" || no "search leaked other books"

echo "############ related — the assistant's retrieval step ############"
# Every article must offer somewhere to go next. This is the check that caught
# the original keyword-only implementation returning an empty panel.
BAD=""
for SLUG in $(pgraw "SELECT slug FROM help_article WHERE active AND NOT is_section ORDER BY slug" | tr -d ' '); do
    [ -z "$SLUG" ] && continue
    N=$(call related "{\"slug\":\"$SLUG\"}" | py "
import json,sys
print(len(json.load(sys.stdin)['result']))")
    [ -z "$N" ] || [ "$N" = "0" ] && BAD="$BAD $SLUG"
done
[ -z "$BAD" ] && ok "every article has related articles" || no "no related articles for:$BAD"
SELF=$(call related '{"slug":"project-overview"}' | grep -c 'project-overview')
[ "$SELF" = "0" ] && ok "an article is not related to itself" || no "self-reference in related"
[ "$(call related '{"slug":"nope"}' | py "
import json,sys
print(len(json.load(sys.stdin)['result']))")" = "0" ] \
  && ok "related on an unknown slug is empty, not an error" || no "related on unknown slug misbehaved"

echo "############ bodies are safe markdown ############"
# The renderer escapes before formatting, but content that ships with raw tags
# would still be a mistake: it would render as visible angle brackets.
[ "$(pg "SELECT count(*) FROM help_article WHERE body ~ '<(script|iframe|img|div|style)'")" = "0" ] \
  && ok "no raw HTML tags in any body" || no "a body contains raw HTML"
# The markdown the articles actually rely on must be present, or the renderer
# features they exercise are untested by anything.
[ "$(pg "SELECT count(*) FROM help_article WHERE body LIKE '%|---%' OR body LIKE '%|--%'")" -ge 1 ] \
  && ok "at least one article uses a table" || no "no tables in the content"
[ "$(pg "SELECT count(*) FROM help_article WHERE body ~ '^- ' OR body ~ E'\n- '")" -ge 1 ] \
  && ok "at least one article uses a bullet list" || no "no bullet lists"
[ "$(pg "SELECT count(*) FROM help_article WHERE body ~ E'\n## '")" -ge 1 ] \
  && ok "at least one article uses headings" || no "no headings"
[ "$(pg "SELECT count(*) FROM help_article WHERE body ~ E'\n> '")" -ge 1 ] \
  && ok "at least one article uses a blockquote" || no "no blockquotes"

echo "############ shipped content is refreshed, not just inserted ############"
# Corrupt a body, restart-equivalent re-seed happens on boot; here we assert the
# upsert exists by checking ON CONFLICT is what the module uses.
grep -q 'ON CONFLICT (slug) DO UPDATE' modules/help/HelpModule.cpp \
  && ok "seeding upserts on slug, so fixes reach existing installs" || no "seed does not upsert"
grep -q 'CREATE UNIQUE INDEX IF NOT EXISTS help_article_slug_uniq' modules/help/HelpModule.cpp \
  && ok "the slug index is created by the module" || no "slug index not in the module"

echo "############ wiring ############"
grep -q "'help.center'" web/static/src/app.js && ok "help.center is a registered custom view" || no "help.center not in CUSTOM_VIEWS"
grep -q 'HelpCenter.js' web/static/index.html && ok "the component is loaded before app.js" || no "HelpCenter.js not loaded"
[ "$(pg "SELECT count(*) FROM ir_ui_menu WHERE parent_id=400")" -ge 1 ] \
  && ok "the Help app has menu entries" || no "no Help menus"
[ "$(pg "SELECT count(*) FROM ir_ui_menu WHERE id=400 AND parent_id IS NULL")" = "1" ] \
  && ok "the Help app root exists" || no "no Help app root"

# OWL parses templates as XML, where a run of hyphens inside a comment is
# illegal and silently kills the whole component. This exact bug cost a render
# cycle here, and the pattern has bitten before (docs/093).
python3 - <<'PY'
import re, sys
src = open('web/static/src/components/HelpCenter.js', encoding='utf-8').read()
m = re.search(r'static template = owl\.xml`(.*?)`;', src, re.S)
if not m:
    print("    FAIL  could not find the OWL template"); sys.exit(1)
tpl = m.group(1)
bad = [c for c in re.findall(r'<!--(.*?)-->', tpl, re.S) if '--' in c]
ctrl = [hex(ord(c)) for c in tpl if ord(c) < 9 or (13 < ord(c) < 32)]
if bad:  print("    FAIL  a template comment contains '--' (invalid XML)")
else:    print("    PASS  no double hyphens inside template comments")
if ctrl: print("    FAIL  control characters in the template:", ctrl[:3])
else:    print("    PASS  no control characters in the template")
PY
python3 - <<'PY'
import re
src = open('web/static/src/components/HelpCenter.js', encoding='utf-8').read()
m = re.search(r'static template = owl\.xml`(.*?)`;', src, re.S)
tpl = m.group(1)
import xml.dom.minidom
try:
    xml.dom.minidom.parseString('<root>' + tpl + '</root>')
    print("    PASS  the OWL template is well-formed XML")
except Exception as e:
    print("    FAIL  the OWL template is not well-formed XML:", str(e)[:90])
PY

[ -z "$FAILED" ] && echo "  All checks passed." || echo "  *** FAILURES ***"

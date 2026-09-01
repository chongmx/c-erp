#!/usr/bin/env bash
# =============================================================
# scripts/seed/website.sh — push resources/website/ into the CMS tables.
#
#   ./scripts/seed.sh website                 deploy every page, menu and form
#   ./scripts/seed.sh website --dry-run       print the SQL, change nothing
#   ./scripts/seed.sh website --page faq      just one page
#   ./scripts/seed.sh website --no-publish    load them, but leave them hidden
#   ./scripts/seed.sh website --status        what is in the database now
#
# WHERE THE CONTENT LIVES. resources/website/, not this script. It used to be a
# 16 KB Python file with the whole site embedded as JSON literals, which meant a
# copy edit was a code change and a diff was unreadable. Now:
#
#   resources/website/pages/<slug>/meta         title, homepage, sequence, ...
#   resources/website/pages/<slug>/blocks.json  the page body, verbatim
#   resources/website/menu.tsv                  sequence, slug, label
#   resources/website/forms/<slug>/meta         title, submit label, ...
#   resources/website/forms/<slug>/fields.tsv   name, label, type, required, ...
#
# blocks.json is EXACTLY what goes into website_page.blocks_json. It is not a
# template and nothing here rewrites it, so the file and the column always agree
# and the in-browser block editor keeps working on the same data.
#
# IDEMPOTENT. Pages upsert on slug, the menu is rebuilt, form fields upsert on
# (form, name). Running it twice changes nothing the second time.
#
# NOT A TEST FIXTURE. The suite seeds its own data and restores a baseline; this
# is the real public site. It publishes by default — pass --no-publish if you
# want to review in Settings -> Website -> Website Pages before it goes live.
# =============================================================
set -uo pipefail
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1

SRC="resources/website"
PGHOST="${PGHOST:-localhost}"; PGUSER="${PGUSER:-odoo}"; PGDATABASE="${PGDATABASE:-odoo}"
export PGPASSWORD="${PGPASSWORD:-odoo}"

DRY=0; ONLY=""; PUBLISH=1; STATUS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)    DRY=1; shift ;;
        --page)       ONLY="${2:?--page needs a slug}"; shift 2 ;;
        --no-publish) PUBLISH=0; shift ;;
        --status)     STATUS=1; shift ;;
        -h|--help)    sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

psql_() { psql -h "$PGHOST" -U "$PGUSER" -d "$PGDATABASE" -v ON_ERROR_STOP=1 -tA "$@"; }
run()   { if [ "$DRY" = 1 ]; then printf '%s\n\n' "$1"; else psql_ -q -c "$1" || exit 1; fi; }

if [ "$STATUS" = 1 ]; then
    echo "slug            | homepage | published | blocks"
    psql_ -F' | ' -c "SELECT rpad(slug,15), is_homepage, is_published,
                             json_array_length(blocks_json::json)
                        FROM website_page WHERE page_kind='page' ORDER BY sequence, id"
    echo
    echo "menu entries : $(psql_ -c 'SELECT count(*) FROM website_menu')"
    echo "form fields  : $(psql_ -c 'SELECT count(*) FROM website_form_field')"
    exit 0
fi

[ -d "$SRC/pages" ] || { echo "ERROR: $SRC/pages not found — nothing to deploy." >&2; exit 1; }

# ------------------------------------------------------------------
# Helpers.
#
# meta files are `key<TAB>value`, one per line, with newline/tab/backslash inside
# a value escaped so a record is always one line. unesc turns them back.
# ------------------------------------------------------------------
metaget() {  # $1 = file, $2 = key, $3 = default
    local v; v=$(awk -F'\t' -v k="$2" '$1==k {sub(/^[^\t]*\t/,""); print; exit}' "$1")
    [ -n "$v" ] && printf '%s' "$v" || printf '%s' "${3:-}"
}
unesc() {  # \n \t \\ -> real characters. Backslash LAST, or it eats the others.
    printf '%s' "$1" | sed -e 's/\\n/\n/g' -e 's/\\t/\t/g' -e 's/\\\\/\\/g'
}
sq() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/''/g")"; }   # SQL string literal

# blocks.json goes in dollar-quoted, so no escaping can mangle the JSON. Guard
# the marker: if the content ever contained it, the statement would end early and
# the rest would be parsed as SQL.
dollar() {  # $1 = file
    if grep -q '\$blocks\$' "$1"; then
        echo "ERROR: $1 contains the dollar-quote marker \$blocks\$ — refusing." >&2
        exit 1
    fi
    printf '$blocks$%s$blocks$' "$(cat "$1")"
}

echo "=== pages ==="
# `website_page_homepage_uniq` is a PARTIAL unique index — btree(is_homepage)
# WHERE is_homepage — so at most one row may claim it, and an INSERT that claims
# it while another page still holds it is REJECTED by the database. Clearing the
# flag first is therefore not tidiness, it is what makes the upsert possible.
#
# Work out which slug claims it among the pages actually being deployed, and
# clear everyone else. Doing this even under --page is deliberate: without it,
# `--page home` fails with a bare unique-violation whenever some other page
# happens to hold the flag.
claimant=""
for d in "$SRC"/pages/*/; do
    slug=$(basename "$d")
    [ -n "$ONLY" ] && [ "$ONLY" != "$slug" ] && continue
    [ -f "$d/meta" ] || continue
    [ "$(awk -F'\t' '$1=="homepage"{print $2; exit}' "$d/meta")" = "yes" ] && claimant="$slug"
done
if [ -n "$claimant" ]; then
    run "UPDATE website_page SET is_homepage = FALSE
          WHERE is_homepage AND slug <> $(sq "$claimant")"
elif [ -z "$ONLY" ]; then
    # A full deploy with no page claiming the homepage defines the whole site,
    # so an old claimant left behind would be stale.
    run "UPDATE website_page SET is_homepage = FALSE WHERE is_homepage"
fi

count=0
for d in "$SRC"/pages/*/; do
    slug=$(basename "$d")
    [ -n "$ONLY" ] && [ "$ONLY" != "$slug" ] && continue
    [ -f "$d/blocks.json" ] || { echo "  skip $slug (no blocks.json)"; continue; }

    title=$(unesc "$(metaget "$d/meta" title "$slug")")
    desc=$(unesc  "$(metaget "$d/meta" description "")")
    seq=$(metaget "$d/meta" sequence 0)
    home=$(metaget "$d/meta" homepage no)
    idx=$(metaget  "$d/meta" indexed yes)
    pub=$(metaget  "$d/meta" published yes)
    [ "$PUBLISH" = 0 ] && pub=no

    homesql=$([ "$home" = yes ] && echo TRUE || echo FALSE)
    idxsql=$([  "$idx"  = yes ] && echo TRUE || echo FALSE)
    pubsql=$([  "$pub"  = yes ] && echo TRUE || echo FALSE)

    run "INSERT INTO website_page (slug, title, blocks_json, is_published, is_homepage,
                                   is_indexed, sequence, meta_description, page_kind)
         VALUES ($(sq "$slug"), $(sq "$title"), $(dollar "$d/blocks.json"),
                 $pubsql, $homesql, $idxsql, $seq, $(sq "$desc"), 'page')
         ON CONFLICT (slug) DO UPDATE
            SET title = EXCLUDED.title, blocks_json = EXCLUDED.blocks_json,
                is_published = EXCLUDED.is_published, is_homepage = EXCLUDED.is_homepage,
                is_indexed = EXCLUDED.is_indexed, sequence = EXCLUDED.sequence,
                meta_description = EXCLUDED.meta_description, write_date = now()"
    printf '  %-14s %2s blocks  seq %-4s %s%s\n' "$slug" \
        "$(grep -c '"type"' "$d/blocks.json")" "$seq" \
        "$([ "$home" = yes ] && echo 'homepage ' || echo '')" \
        "$([ "$pub" = yes ] && echo 'published' || echo 'UNPUBLISHED')"
    count=$((count+1))
done
echo "  $count page(s)"

# The menu is rebuilt rather than upserted: it is a short ordered list, and
# rebuilding is the only way a REMOVED entry actually disappears.
if [ -z "$ONLY" ] && [ -f "$SRC/menu.tsv" ]; then
    echo
    echo "=== menu ==="
    run "DELETE FROM website_menu"
    while IFS=$'\t' read -r mseq mslug mname; do
        [ -z "${mslug:-}" ] && continue
        run "INSERT INTO website_menu (name, page_id, sequence)
             SELECT $(sq "$(unesc "$mname")"), id, $mseq
               FROM website_page WHERE slug = $(sq "$mslug")"
        printf '  %-4s %-16s %s\n' "$mseq" "$mslug" "$mname"
    done < "$SRC/menu.tsv"
fi

if [ -z "$ONLY" ] && [ -d "$SRC/forms" ]; then
    echo
    echo "=== forms ==="
    for f in "$SRC"/forms/*/; do
        [ -f "$f/meta" ] || continue
        fslug=$(basename "$f")
        ftitle=$(unesc "$(metaget "$f/meta" title "$fslug")")
        fdesc=$(unesc  "$(metaget "$f/meta" description "")")
        fsub=$(unesc   "$(metaget "$f/meta" submit_label "Send")")
        fok=$(unesc    "$(metaget "$f/meta" success_message "Thank you.")")
        run "INSERT INTO website_form (slug, title, description, submit_label, success_message)
             VALUES ($(sq "$fslug"), $(sq "$ftitle"), $(sq "$fdesc"), $(sq "$fsub"), $(sq "$fok"))
             ON CONFLICT (slug) DO UPDATE
                SET title = EXCLUDED.title, description = EXCLUDED.description,
                    submit_label = EXCLUDED.submit_label,
                    success_message = EXCLUDED.success_message"
        n=0
        if [ -f "$f/fields.tsv" ]; then
            while IFS=$'\t' read -r fn fl ft freq fseq fopt; do
                [ -z "${fn:-}" ] && continue
                req=$([ "${freq:-no}" = yes ] && echo TRUE || echo FALSE)
                run "INSERT INTO website_form_field
                         (form_id, name, label, field_type, required, sequence, options)
                     SELECT id, $(sq "$fn"), $(sq "$(unesc "$fl")"), $(sq "$ft"),
                            $req, ${fseq:-0}, $(sq "$(unesc "${fopt:-}")")
                       FROM website_form WHERE slug = $(sq "$fslug")
                     ON CONFLICT (form_id, name) DO UPDATE
                        SET label = EXCLUDED.label, field_type = EXCLUDED.field_type,
                            required = EXCLUDED.required, sequence = EXCLUDED.sequence,
                            options = EXCLUDED.options"
                n=$((n+1))
            done < "$f/fields.tsv"
        fi
        printf '  %-18s %s field(s)\n' "$fslug" "$n"
    done
fi

if [ "$DRY" = 1 ]; then
    echo
    echo "(--dry-run: nothing was written)"
    exit 0
fi

echo
echo "=== result ==="
psql_ -F' | ' -c "SELECT rpad(slug,15), is_homepage, is_published
                    FROM website_page WHERE page_kind='page' ORDER BY sequence, id" \
  | sed 's/^/  /'
home=$(psql_ -c "SELECT count(*) FROM website_page WHERE is_homepage AND is_published")
echo
if [ "${home:-0}" -ge 1 ]; then
    echo "  homepage published — / serves the site."
else
    echo "  NOTE  no published homepage, so / falls back to /login."
    echo "        (WebsiteModule.cpp checks is_homepage AND is_published.)"
fi

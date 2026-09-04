#!/usr/bin/env bash
# =============================================================
# audit_schema_doc.sh — is docs/reference/database-schema.md still true?
#
# That page is hand-maintained: 128 tables and their columns, written down.
# Nothing regenerates it, so it rots silently — a column added by a migration
# is simply absent from the page, and the page still looks authoritative.
#
# When this was first run it found nine tables with the wrong column count,
# two whose column NAMES had been mangled (`account_partial_reconcile` listed
# `in`/`at` where the database has `amount`/`amount_base`, and `ir_sequence`
# listed `so` for `reset_policy`), and two control-plane tables presented as
# though they lived in the tenant database.
#
# Run it against a live database after adding or dropping a column:
#
#     ./tests/tools/audit_schema_doc.sh
#
# It reports drift and exits non-zero; it never edits the page. Column ORDER is
# deliberately not checked — the page groups by module and several rows are in
# a curated order, which is fine as long as the set matches.
# =============================================================
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1

DBN=${DBN:-odoo}
export PGPASSWORD=${PGPASSWORD:-odoo}
PAGE=docs/reference/database-schema.md
Q() { psql -h localhost -U "$DBN" -d "$DBN" -tA -c "$1"; }

# The control plane is a SEPARATE database (core/ControlPlane.cpp). Its tables
# are documented on the page but are not expected in a tenant.
CONTROL_PLANE='mc_membership mc_shared_product'

DRIFT=0

grep -oE '^\| `[a-z_]+`' "$PAGE" | sed 's/^| *//; s/`//g' | sort -u > /tmp/asd_page.txt
Q "SELECT table_name FROM information_schema.tables
    WHERE table_schema='public' AND table_type='BASE TABLE' ORDER BY 1" | sort -u > /tmp/asd_db.txt

echo "page lists : $(wc -l < /tmp/asd_page.txt) tables"
echo "database   : $(wc -l < /tmp/asd_db.txt) tables"

echo
echo "-- documented but absent from this database --"
while IFS= read -r t; do
    case " $CONTROL_PLANE " in
        *" $t "*) echo "  $t  (control plane — expected)" ;;
        *)        echo "  $t  ** not created by any migration? **"; DRIFT=$((DRIFT+1)) ;;
    esac
done < <(comm -23 /tmp/asd_page.txt /tmp/asd_db.txt)

echo "-- in this database but MISSING from the page --"
while IFS= read -r t; do
    [ -n "$t" ] || continue
    echo "  $t"; DRIFT=$((DRIFT+1))
done < <(comm -13 /tmp/asd_page.txt /tmp/asd_db.txt)

echo
echo "-- rows whose column set is wrong --"
while IFS= read -r line; do
    t=$(printf '%s' "$line" | sed 's/^| *`\([a-z_]*\)`.*/\1/')
    [ -n "$t" ] || continue
    real=$(Q "SELECT string_agg(column_name, ',' ORDER BY column_name)
                FROM information_schema.columns
               WHERE table_schema='public' AND table_name='$t'")
    [ -n "$real" ] || continue          # control-plane or dropped table
    doc=$(printf '%s' "$line" \
          | sed 's/^| *`[a-z_]*` *| *[0-9]* *| *//; s/ *|$//; s/`//g; s/, /\n/g' \
          | sort | paste -sd,)
    n=$(printf '%s' "$line" | sed 's/^| *`[a-z_]*` *| *\([0-9]*\).*/\1/')
    nreal=$(Q "SELECT count(*) FROM information_schema.columns
                WHERE table_schema='public' AND table_name='$t'")
    if [ "$doc" != "$real" ] || [ "$n" != "$nreal" ]; then
        echo "  $t  (page says $n columns, database has $nreal)"
        comm -23 <(printf '%s' "$doc"  | tr ',' '\n') \
                 <(printf '%s' "$real" | tr ',' '\n') | sed 's/^/      only on the page: /'
        comm -13 <(printf '%s' "$doc"  | tr ',' '\n') \
                 <(printf '%s' "$real" | tr ',' '\n') | sed 's/^/      only in the db  : /'
        DRIFT=$((DRIFT+1))
    fi
done < <(grep -E '^\| `[a-z_]+` \| [0-9]+ \|' "$PAGE")

echo
if [ "$DRIFT" = "0" ]; then
    echo "The page matches the database."
    exit 0
fi
echo "$DRIFT table(s) drifted — update $PAGE."
exit 1

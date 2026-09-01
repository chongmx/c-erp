#!/usr/bin/env bash
# =============================================================
# scripts/seed.sh — one entry point for every kind of seeding.
#
#   ./scripts/seed.sh                 what each dataset is, and whether it is present
#   ./scripts/seed.sh parts           electronics catalogue for the faceted browser
#   ./scripts/seed.sh rental          the demo storage facility
#   ./scripts/seed.sh website         the Easy Locker Space CMS pages
#   ./scripts/seed.sh all             all three, in that order
#
# Flags after the name go straight to the underlying script:
#
#   ./scripts/seed.sh parts --clean
#   ./scripts/seed.sh rental --clear
#   ./scripts/seed.sh rental --status
#
# WHAT THIS IS NOT. It is not test setup. The suite seeds its own fixtures
# (tests/lib/fixtures.sh, driven by tests/setup) and restores a clean baseline
# on every run — see CLAUDE.md. These datasets are for looking at the
# application by hand, and for the two tests that explicitly ask for them.
#
# WHAT A CLEAN DATABASE IS. `db/snapshots/baseline.dump`: schema plus reference
# data, one company, the admin user, nothing else. Seeding is what you do to it
# afterwards, and every dataset below is removable and idempotent, so it stays
# reversible:
#
#   ./scripts/db_snapshot.sh restore db/snapshots/baseline.dump
# =============================================================
set -uo pipefail
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1

DBN=${DBN:-odoo}
pgq() { PGPASSWORD="${PGPASSWORD:-odoo}" psql -h "${PGHOST:-localhost}" -U "$DBN" \
        -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }

status() {
    echo "dataset   script                      present"
    echo "--------  --------------------------  -------"
    printf '%-9s %-27s %s\n' parts   scripts/seed/parts.sh \
        "$(pgq "SELECT count(*) FROM product_product WHERE default_code LIKE 'DP-%'") products"
    printf '%-9s %-27s %s\n' rental  scripts/seed/rental.sh \
        "$(pgq "SELECT count(*) FROM rental_unit") units"
    printf '%-9s %-27s %s\n' website scripts/seed/website.sh \
        "$(pgq "SELECT count(*) FROM website_page") pages"
    echo
    echo "Run './scripts/seed.sh <dataset>' to create one, or --help for the rest."
}

# A count against a table the module has not created yet is not an error worth
# stopping for; pgq swallows it and the cell reads empty.
case "${1:-}" in
    ""|--status|-h|--help)
        sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
        echo
        status
        ;;
    parts)   shift; exec bash    scripts/seed/parts.sh   "$@" ;;
    rental)  shift; exec bash    scripts/seed/rental.sh  "$@" ;;
    website) shift; exec bash    scripts/seed/website.sh "$@" ;;
    all)
        bash    scripts/seed/parts.sh   || exit 1
        bash    scripts/seed/rental.sh  || exit 1
        bash    scripts/seed/website.sh || exit 1
        echo "All datasets seeded."
        ;;
    *)
        echo "unknown dataset '$1' — try: parts, rental, website, all" >&2
        exit 1
        ;;
esac

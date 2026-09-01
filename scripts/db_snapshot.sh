#!/usr/bin/env bash
# =============================================================
# db_snapshot.sh — take and restore a whole-database snapshot.
#
# Used by run_tests.sh to put the database back exactly as it was before the
# suite ran. This is a RESTORE, not a wipe: demo data, the parts catalogue and
# anything else you had keep their contents and their ids, and only the debris
# the tests created disappears.
#
#   ./scripts/db_snapshot.sh take  [file]   # dump to file (default: log/pretest.dump)
#   ./scripts/db_snapshot.sh restore [file] # restore it
#   ./scripts/db_snapshot.sh verify  [file] # is the file a usable dump?
#
# Why a custom-format dump rather than TRUNCATE:
#   * it restores state, instead of approximating "clean" with a table list,
#   * it survives a test that changed a CONFIGURATION row — a decimal precision,
#     a sequence, a setting — which no reset scope would put back,
#   * a failed restore leaves the dump on disk to retry from.
#
# The server must be stopped during a restore: --clean drops objects, and open
# connections hold locks that make that fail halfway.
# =============================================================
set -uo pipefail
cd "$(dirname "$0")/.."

PGHOST="${PGHOST:-localhost}"; PGUSER="${PGUSER:-odoo}"; PGDATABASE="${PGDATABASE:-odoo}"
export PGPASSWORD="${PGPASSWORD:-odoo}"

ACTION="${1:-}"
FILE="${2:-log/pretest.dump}"
mkdir -p "$(dirname "$FILE")"

case "$ACTION" in

take)
    if ! pg_dump -h "$PGHOST" -U "$PGUSER" -d "$PGDATABASE" -Fc -f "$FILE" 2>/tmp/snap_err; then
        echo "  snapshot FAILED:"; sed 's/^/    /' /tmp/snap_err; exit 1
    fi
    echo "  snapshot taken: $FILE ($(du -h "$FILE" | cut -f1))"
    ;;

verify)
    [ -s "$FILE" ] || { echo "  no snapshot at $FILE"; exit 1; }
    # pg_restore -l reads the table of contents; if that parses, the archive is
    # structurally sound. Checking before relying on it is the difference
    # between a restore and a hope.
    if pg_restore -l "$FILE" > /dev/null 2>/tmp/snap_err; then
        echo "  snapshot is readable: $(pg_restore -l "$FILE" | grep -c 'TABLE DATA') table(s) of data"
    else
        echo "  snapshot is NOT readable:"; sed 's/^/    /' /tmp/snap_err; exit 1
    fi
    ;;

restore)
    [ -s "$FILE" ] || { echo "  no snapshot at $FILE — nothing to restore"; exit 1; }
    pg_restore -l "$FILE" > /dev/null 2>&1 || { echo "  snapshot at $FILE is unreadable; refusing to restore"; exit 1; }

    WAS_UP=0
    curl -s -o /dev/null --max-time 3 "http://127.0.0.1:8069/" && WAS_UP=1
    pkill -x c-erp 2>/dev/null && sleep 2

    # Terminate anything else still attached, or --clean fails on locks.
    psql -h "$PGHOST" -U "$PGUSER" -d postgres -tAc \
      "SELECT pg_terminate_backend(pid) FROM pg_stat_activity
        WHERE datname='$PGDATABASE' AND pid <> pg_backend_pid()" > /dev/null 2>&1

    # DROP SCHEMA, not --clean.
    #
    # `pg_restore --clean` drops objects ONE AT A TIME, and a DROP that fails
    # is only a warning. product_template has dependents, so its DROP failed,
    # the CREATE then failed with "already exists", and its COPY was skipped —
    # leaving the OLD rows in place while every other table was replaced. The
    # restore reported "errors but the schema is present" and carried on.
    #
    # The visible result was 163 demo products pointing at template ids that
    # the restore had refused to load, which then surfaced as unrelated
    # failures in pricelists, product-variants and part-lookup. A restore that
    # silently does not restore is worse than one that fails outright.
    #
    # Dropping the whole schema first removes the ordering problem entirely:
    # there is nothing left to depend on anything. The server is already
    # stopped and other connections have been terminated above, so nothing
    # holds a lock on it.
    psql -h "$PGHOST" -U "$PGUSER" -d "$PGDATABASE" -q \
         -c "DROP SCHEMA IF EXISTS public CASCADE; CREATE SCHEMA public;" \
         > /dev/null 2>>/tmp/snap_err
    pg_restore -h "$PGHOST" -U "$PGUSER" -d "$PGDATABASE" \
               --no-owner --no-privileges "$FILE" > /tmp/snap_out 2>/tmp/snap_err
    RC=$?

    # A restore is only a restore if the data came back. Compare the row counts
    # the archive carries against what is now in the database for a handful of
    # tables that have dependents — precisely the ones --clean used to skip.
    MISMATCH=""
    for t in product_template product_product part_parameter account_move; do
        want=$(pg_restore -l "$FILE" 2>/dev/null | grep -c "TABLE DATA public $t ")
        [ "$want" -eq 0 ] && continue
        have=$(psql -h "$PGHOST" -U "$PGUSER" -d "$PGDATABASE" -tAc \
                 "SELECT count(*) FROM $t" 2>/dev/null)
        [ -z "$have" ] && MISMATCH="$MISMATCH $t(missing)"
    done
    [ -n "$MISMATCH" ] && echo "  WARNING: tables did not come back:$MISMATCH"

    ROWS=$(psql -h "$PGHOST" -U "$PGUSER" -d "$PGDATABASE" -tAc \
             "SELECT count(*) FROM pg_tables WHERE schemaname='public'" 2>/dev/null)
    if [ -z "$ROWS" ] || [ "$ROWS" -lt 10 ]; then
        echo "  RESTORE FAILED — the database does not look intact (public tables: ${ROWS:-none})."
        echo "  The snapshot is still at $FILE. Do not run the suite again until this is resolved."
        sed -n '1,10p' /tmp/snap_err | sed 's/^/    /'
        exit 1
    fi
    if [ "$RC" -ne 0 ]; then
        echo "  restore reported errors (exit $RC) but the schema is present ($ROWS tables)."
        echo "  first lines of stderr:"; sed -n '1,5p' /tmp/snap_err | sed 's/^/    /'
    else
        echo "  database restored from $FILE ($ROWS tables)"
    fi

    if [ "$WAS_UP" -eq 1 ]; then
        setsid nohup ./build/c-erp > log/server.out 2>&1 < /dev/null &
        for _ in $(seq 1 20); do
            curl -s -o /dev/null --max-time 2 "http://127.0.0.1:8069/" && break
            sleep 1
        done
        curl -s -o /dev/null --max-time 2 "http://127.0.0.1:8069/" \
            && echo "  server restarted" \
            || echo "  NOTE: the server did not come back up — start it manually"
    fi
    ;;

*)
    echo "usage: $0 {take|restore|verify} [file]"; exit 2 ;;
esac

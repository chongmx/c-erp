#!/usr/bin/env bash
# =============================================================
# db.sh — PostgreSQL helpers and scenario (snapshot) control.
#
# Two rules that cost real debugging time to learn, both encoded here:
#
#  * `psql -tAc "INSERT ... RETURNING id"` prints the returned row AND the
#    command tag. A two-line id silently corrupts every statement built from
#    it, so anything reading an id goes through pgid(), which takes head -1.
#
#  * stderr is swallowed by pg() for convenience, which means a failed INSERT
#    looks exactly like a successful one that returned nothing. When an id
#    comes back empty, that is the first thing to suspect — it is what made a
#    `product_uom` / `product_uom_id` typo surface two assertions later as
#    "sale line row missing". pgv() is the loud variant for when you need to
#    see why.
# =============================================================
[ -n "${ERP_DB_LOADED:-}" ] && return 0
ERP_DB_LOADED=1

DBN=${DBN:-odoo}
: "${PGPASSWORD:=odoo}"
export PGPASSWORD

pg(){  psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
pgv(){ psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1"; }
pgid(){ psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }

# table_exists <name>
table_exists(){ [ "$(pg "SELECT count(*) FROM information_schema.tables WHERE table_name='$1'")" = "1" ]; }

# column_exists <table> <column>
column_exists(){ [ "$(pg "SELECT count(*) FROM information_schema.columns WHERE table_name='$1' AND column_name='$2'")" = "1" ]; }

# ------------------------------------------------------------------
# Scenarios — named database states a test can demand in its `meta`.
#
#   scenario=baseline           db/snapshots/baseline.dump
#   scenario=current            whatever is there (no restore)
#   scenario=<name>             db/snapshots/<name>.dump
#   scenario=path/to/file.dump  an explicit file
#
# The runner switches scenarios only when the NAME CHANGES, so twenty tests
# that all want `baseline` restore once, not twenty times. A restore is ~2s;
# doing it per-test would add a minute to the suite for nothing.
# ------------------------------------------------------------------
scenario_file(){
    case "$1" in
        current|"")  echo "" ;;
        */*|*.dump)  echo "$1" ;;
        *)           echo "db/snapshots/$1.dump" ;;
    esac
}

# scenario_load <name> — restore the named state. Returns 1 if it cannot.
scenario_load(){
    local f; f=$(scenario_file "$1")
    [ -z "$f" ] && return 0
    if [ ! -s "$ERP_ROOT/$f" ] && [ ! -s "$f" ]; then
        echo "  scenario '$1' has no dump at $f"
        return 1
    fi
    bash "$ERP_ROOT/scripts/db_snapshot.sh" restore "$f"
}

# scenario_save <name> — capture the current state as a reusable scenario.
# This is how a corner case gets tested against a large or awkward database
# without carrying one in the working database: build the state once, dump it,
# and name it in a `meta`.
scenario_save(){
    local f; f=$(scenario_file "$1")
    bash "$ERP_ROOT/scripts/db_snapshot.sh" take "$f"
}

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
# Menu / action id allocation — a STATIC check over the source (docs/090).
#
# Every module seeds ir_ui_menu and ir_act_window rows with hardcoded ids.
# Two modules choosing the same id is silent: whichever seeds last wins, the
# loser's menu opens the winner's screen, and seeding on top of an APP ROOT
# deletes a whole app from the home screen. Eight of these shipped before
# anything checked for them (docs/076, docs/089).
#
# Every previous fix was a repair after the fact. This is the control that
# fires at authoring time: it reads the source, not the database, so a
# collision fails the suite whether or not anyone has run the server yet.
#
# It also prints the next free id in each space, which is the number the next
# person needs and the reason the old rule ("grep before you pick") kept
# getting skipped.
# =============================================================
# (repository root: handled by the harness preamble)
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }

# App roots and the Settings children are reserved: seeding over one of these
# removes an app from the home screen rather than merely hijacking a menu.
RESERVED_MENUS="10 20 30 50 60 70 80 90 110 130 300 400"

# ---------------------------------------------------------------
# Extract "<id> <file>" pairs for one table across every module.
#
# A state machine, because the seeds come in two shapes: a multi-row
# "INSERT ... VALUES\n (1,...),\n (2,...)" and a single "INSERT ... VALUES (1,...)".
# Both start a row with "(<digits>," once the INSERT for that table is open.
# SQL comment lines are skipped so a commented-out id is not counted.
# ---------------------------------------------------------------
extract(){
    local table="$1"
    awk -v want="$table" '
        # A new txn.exec( starts a new statement: whatever table the previous
        # one was inserting into no longer applies. Without this reset the mode
        # leaked across statements and an UPDATE mentioning "(101,103)" was
        # counted as an action id, which is how the "next free" hint drifted.
        /txn\.exec\(/ { mode = 0 }
        /INSERT[ \t]+INTO[ \t]+ir_ui_menu/    { mode = (want=="ir_ui_menu")    ? 1 : 0 }
        /INSERT[ \t]+INTO[ \t]+ir_act_window/ { mode = (want=="ir_act_window") ? 1 : 0 }
        /INSERT[ \t]+INTO/ && !/ir_ui_menu|ir_act_window/ { mode = 0 }
        /^[ \t]*--/ { next }
        mode {
            # Every "(<digits>," on the line, not just the first: a seed may put
            # several rows on one line, and stopping at the first would let the
            # rest through unchecked.
            line = $0
            while (match(line, /\([ \t]*[0-9]+[ \t]*,/)) {
                s = substr(line, RSTART, RLENGTH)
                gsub(/[^0-9]/, "", s)
                print s, FILENAME
                line = substr(line, RSTART + RLENGTH)
            }
        }
        # End of the C++ statement — checked AFTER the capture so the closing
        # line of a multi-row INSERT still counts.
        /;[ \t]*$/ { mode = 0 }
    ' modules/*/[A-Z]*.cpp
}

report_dupes(){
    local table="$1" label="$2"
    local tmp; tmp=$(mktemp)
    # `sort -u`, NOT `sort -n -u`: with -n, uniqueness is judged on the numeric
    # key alone, so "10 IrModule.cpp" and "10 UomModule.cpp" collapse into one
    # row and the collision this whole script exists to catch disappears.
    extract "$table" | sort -u > "$tmp"
    # A duplicate is the SAME id claimed from two different files.
    local dupes
    dupes=$(awk '{c[$1]=c[$1]" "$2} END {for (i in c) {n=split(c[i],a," "); if (n>1) print i":"c[i]}}' "$tmp" | sort -n)
    if [ -z "$dupes" ]; then
        ok "$label: no id is claimed by two modules"
    else
        no "$label: id claimed by more than one module"
        echo "$dupes" | while read -r line; do echo "          $line"; done
    fi
    rm -f "$tmp"
}

echo "############ no cross-module id collisions ############"
report_dupes ir_act_window "ir_act_window"
report_dupes ir_ui_menu   "ir_ui_menu"

echo "############ app roots are not overwritten ############"
# An app root may only be seeded by the module that owns it, and only once.
ROOT_BAD=
for r in $RESERVED_MENUS; do
    owners=$(extract ir_ui_menu | awk -v id="$r" '$1==id {print $2}' | sort -u)
    n=$(echo "$owners" | grep -c . )
    if [ "${n:-0}" -gt 1 ]; then
        ROOT_BAD=1
        echo "          app root $r seeded by: $(echo "$owners" | tr '\n' ' ')"
    fi
done
[ -z "$ROOT_BAD" ] && ok "every app root has exactly one owner" \
                   || no "an app root is seeded by more than one module"

echo "############ the extractor actually sees the seeds ############"
# A parser that silently matches nothing would make every check above pass.
# These two counts are the canary: if they collapse, the check is broken, not
# the code.
MC=$(extract ir_ui_menu   | wc -l)
AC=$(extract ir_act_window | wc -l)
[ "${MC:-0}" -ge 40 ] && ok "found $MC ir_ui_menu seeds" || no "only $MC menu seeds found — the parser is broken"
[ "${AC:-0}" -ge 40 ] && ok "found $AC ir_act_window seeds" || no "only $AC action seeds found — the parser is broken"
# And it must find ids we know exist.
extract ir_ui_menu    | awk '$1==10' | grep -q . && ok "the Accounting app root (menu 10) is seen" || no "menu 10 not found by the parser"
extract ir_act_window | awk '$1==94' | grep -q . && ok "Reordering Rules (action 94) is seen"     || no "action 94 not found by the parser"

echo "############ next free ids ############"
# Printed, not asserted — this is the number the next person needs.
next_free(){
    extract "$1" | awk '{print $1}' | sort -n -u | awk -v start="$2" '
        BEGIN { n = start }
        { if ($1 == n) n++ ; else if ($1 > n) { print n; found=1; exit } }
        END { if (!found) print n }
    '
}
echo "    NOTE  next free ir_ui_menu id    (from 63):  $(next_free ir_ui_menu 63)"
echo "    NOTE  next free ir_act_window id (from 94):  $(next_free ir_act_window 94)"
echo "    NOTE  reserved app roots: $RESERVED_MENUS  (+ Settings children 31, 32)"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

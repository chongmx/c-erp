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
# Regression guard for the "New … → Internal Error" class of bugs.
#
# Clicking "New" on a list and then Create used to 500 for most models: a
# missing required field surfaced as a raw "the reference ERP Server Error" (Internal Error)
# instead of a user-facing message. The contract we assert here:
#
#   create() with an empty/partial body must NEVER return a 500 "the reference ERP Server
#   Error". It must return either a real id (success) or a 400 ValidationError
#   whose data.message tells the user what is missing.
#
# A 500 here means the New form is broken for that model.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }

# Records this probe actually created, so they can be removed at the end.
# Without this the script left one row behind PER RUN for every model whose
# create() succeeds — 29 empty "New" transfers and 29 nameless product
# categories had accumulated in the dev database before anyone noticed the
# blank rows in the category sidebar (docs/092).
CREATED=""

# create <model> <json-body> — asserts no 500 Internal Error.
newcheck() {
    local model="$1" body="$2"
    local r=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$model\",\"method\":\"create\",\"args\":[$body],\"kwargs\":{\"context\":{\"session_id\":\"$SID\"}}}}")
    if echo "$r" | grep -q '"result":[0-9]'; then
        CREATED="$CREATED $model:$(echo "$r" | sed -n 's/.*"result":\([0-9]*\).*/\1/p')"
        ok "$model — create succeeded"
    elif echo "$r" | grep -q '"name":"cerp.exceptions.ValidationError"'; then
        local m=$(echo "$r" | sed -n 's/.*"data":{"message":"\([^"]*\)".*/\1/p')
        ok "$model — clean validation error ('${m}')"
    elif echo "$r" | grep -q '"name":"cerp.exceptions.AccessError"'; then
        ok "$model — access-gated (not an Internal Error)"
    else
        no "$model — 500/Internal Error on create: $(echo "$r" | head -c 160)"
    fi
}

echo "############ New/create must not Internal-Error ############"
# Config / master-data models reached from a "New" button (generic form).
for m in uom.uom account.tax account.journal account.account stock.picking.type \
         stock.location stock.warehouse stock.production.lot stock.putaway.rule \
         stock.landed.cost hr.department hr.job hr.employee resource.calendar \
         part.footprint part.unit rental.unit.type rental.expense.category \
         rental.contract rental.expense account.analytic.account product.supplierinfo \
         mrp.workcenter product.category res.partner; do
    newcheck "$m" '{}'
done

echo
echo "############ documents created from a New button ############"
newcheck sale.order     '{}'
newcheck purchase.order '{}'
newcheck account.move   '{}'

echo
echo "############ transfer (stock.picking) created with its required fields ############"
# stock.picking needs picking_type_id + both locations (this is what the
# TransferFormView 'New' now supplies from the chosen Operation Type).
pg() { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
PID=$(pg "SELECT id FROM stock_picking_type ORDER BY id LIMIT 1")
SRC=$(pg "SELECT default_location_src_id  FROM stock_picking_type WHERE id=$PID")
DST=$(pg "SELECT default_location_dest_id FROM stock_picking_type WHERE id=$PID")
newcheck stock.picking "{\"picking_type_id\":$PID,\"location_id\":$SRC,\"location_dest_id\":$DST,\"state\":\"draft\"}"
# and its bare form (no fields) must still be a clean validation error, not a 500
newcheck stock.picking '{}'

echo
echo "############ every menu opens its OWN model (ir_act_window id collisions) ############"
# Two modules hardcoding the same ir_act_window id silently hijacks a menu:
# whichever seeds last wins, and the loser's menu opens the winner's model.
# This bit Sales Orders / Purchase Orders / Employees / Job Positions (docs/076)
# and Reordering Rules → Document Templates. Assert the name still matches.
pgq(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
chkmenu(){ # menu-name, expected res_model
    local got=$(pgq "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='$1' LIMIT 1")
    [ "$got" = "$2" ] && ok "menu '$1' opens $2" || no "menu '$1' opens '$got' (expected $2)"
}
chkmenu "Sales Orders"       sale.order
chkmenu "Purchase Orders"    purchase.order
chkmenu "Reordering Rules"   stock.warehouse.orderpoint
chkmenu "Document Templates" ir.report.template
chkmenu "Landed Costs"       stock.landed.cost
chkmenu "Putaway Rules"      stock.putaway.rule
chkmenu "ERP Settings"       ir.erp.settings
chkmenu "Groups"             res.groups
chkmenu "Work Centers"       mrp.workcenter
# Portal Users and Master Production Schedule both lost their ids to MRP
# (action 35 / menu 120) — Settings ▸ Portal Users had vanished entirely.
# Found by the menu-id check, now tests/integration/core/menu-ids/ (docs/090);
# these two keep it fixed.
chkmenu "Portal Users"               portal.partner
chkmenu "Master Production Schedule" mrp.production.schedule
chkmenu "Manufacturing Orders"       mrp.production
chkmenu "Packages"                   stock.quant.package
chkmenu "Employee Expenses"          hr.expense
chkmenu "Expense Reports"            hr.expense.sheet

# App roots are structural: seeding a menu on top of one removes a whole app
# from the home screen (this happened to Settings — docs/089).
for app in Accounting Settings Contacts Sales Purchase Inventory Products Employees Manufacturing; do
    got=$(pgq "SELECT COUNT(*) FROM ir_ui_menu WHERE name='$app' AND parent_id IS NULL")
    [ "${got:-0}" -ge 1 ] && ok "app root '$app' present on the home screen" \
                          || no "app root '$app' is MISSING (its menu id was overwritten?)"
done
# and the Settings app still has its own children
KIDS=$(pgq "SELECT COUNT(*) FROM ir_ui_menu WHERE parent_id=(SELECT id FROM ir_ui_menu WHERE name='Settings' AND parent_id IS NULL LIMIT 1)")
[ "${KIDS:-0}" -ge 3 ] && ok "Settings app has its submenus ($KIDS)" || no "Settings app has only $KIDS submenus"
# and no two menus with different names may share one action that names a model
DUP=$(pgq "SELECT COUNT(*) FROM (SELECT action_id FROM ir_ui_menu WHERE action_id IS NOT NULL GROUP BY action_id HAVING COUNT(DISTINCT name) > 1) x")
[ "${DUP:-0}" = "0" ] && ok "no action is shared by differently-named menus" \
    || echo "    NOTE  $DUP action(s) shared by differently-named menus (review if unintended)"

echo
echo "############ housekeeping ############"
# Remove what this probe created. A create that succeeds here is a throwaway
# record with no business meaning — leaving it behind is how the dev database
# filled up with blank categories and empty transfers.
REMOVED=0
for pair in $CREATED; do
    m="${pair%%:*}"; i="${pair##*:}"
    [ -z "$i" ] && continue
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$m\",\"method\":\"unlink\",\"args\":[[$i]],\"kwargs\":{\"context\":{\"session_id\":\"$SID\"}}}}" >/dev/null
    REMOVED=$((REMOVED + 1))
done
LEFT=$(pgq "SELECT count(*) FROM stock_picking p WHERE p.name='New' AND p.state='draft' AND NOT EXISTS (SELECT 1 FROM stock_move m WHERE m.picking_id=p.id)")
[ "${LEFT:-0}" = "0" ] && ok "removed $REMOVED probe record(s); none left behind" \
                       || no "$LEFT empty draft transfer(s) still in the database"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

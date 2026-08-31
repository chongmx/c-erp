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
# Rental phase 3 — the unit grid (docs/046 §4).
#
# A browser cannot be driven from here, so this checks the two things
# that actually break a view of this kind:
#
#   1. the assets are served at the paths index.html references
#   2. the RPC returns exactly the fields and shapes the component reads
#
# A component that renders blank almost always fails at (2) — a many2one
# arriving as a bare int instead of [id, label], or a field simply
# missing from the response — and that produces no error anywhere.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

echo "############ 1. assets are served ############"
for p in /index.html /src/app.js /src/components/rental/RentalUnitGrid.js \
         /src/components/rental/rental.css; do
    code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE$p")
    printf '    %-48s %s\n' "$p" "$code"
    [ "$code" = "200" ] && ok "served" || no "$p returned $code"
done

echo
echo "############ 2. index.html loads them, in the right order ############"
HTML=$(curl -s "$BASE/index.html")
printf '%s' "$HTML" | grep -q 'rental/rental.css'        && ok "rental.css linked"        || no "rental.css not linked"
printf '%s' "$HTML" | grep -q 'rental/RentalUnitGrid.js' && ok "RentalUnitGrid.js loaded" || no "grid script not loaded"
# The component must be DEFINED before app.js runs, because app.js
# references it at definition time in CUSTOM_VIEWS. Out of order means a
# ReferenceError at load and a blank application.
GRID_LINE=$(printf '%s' "$HTML" | grep -n 'RentalUnitGrid.js' | head -1 | cut -d: -f1)
APP_LINE=$(printf '%s' "$HTML" | grep -n 'src/app.js' | head -1 | cut -d: -f1)
echo "    grid at line $GRID_LINE, app.js at line $APP_LINE"
[ -n "$GRID_LINE" ] && [ -n "$APP_LINE" ] && [ "$GRID_LINE" -lt "$APP_LINE" ] \
    && ok "grid is defined before app.js references it" \
    || no "load order wrong — app.js would throw ReferenceError"

echo
echo "############ 3. app.js registers the model ############"
APPJS=$(curl -s "$BASE/src/app.js")
printf '%s' "$APPJS" | grep -q "'rental.unit':" && ok "rental.unit in CUSTOM_VIEWS" \
                                                || no "rental.unit not registered"

echo
echo "############ 4. the RPC returns what the component reads ############"
# This suite used to rely on the demo facility being present — ambient
# data left by another script. When that script started restoring an
# EMPTY state, the field assertions below failed against zero rows on a
# machine where nothing was wrong. A suite that needs a row creates one.
OWN_UNIT=
if [ "$(pg "SELECT count(*) FROM rental_unit WHERE active")" = "0" ]; then
    TY=$(pg "SELECT id FROM rental_unit_type ORDER BY id LIMIT 1")
    OWN_UNIT=$(pg "INSERT INTO rental_unit (code,name,type_id,zone,state,company_id)
                   VALUES ('GRIDPROBE-1','grid probe',$TY,'Probe Zone','available',1)
                   RETURNING id")
    echo "    (no units present; created probe unit $OWN_UNIT for this run)"
fi
cat > /tmp/rg_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/rg_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }

cat > /tmp/rg_u.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"rental.unit","method":"search_read",
 "args":[[]],
 "kwargs":{"fields":["id","code","name","type_id","zone","floor","site","state","notes"],
           "limit":1000,"context":{"session_id":"$SID"}}}}
EOF
R=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/rg_u.json)

for f in '"code"' '"state"' '"zone"' '"type_id"'; do
    printf '%s' "$R" | grep -q "$f" && ok "response carries $f" || no "$f missing from search_read"
done

# This backend returns a many2one as a BARE ID, not the reference ERP's [id, label]
# pair — formatCell() in app.js copes with both, and that is the
# convention here. The component must therefore resolve the label itself
# from the unit types it loads separately. Assuming the pair rendered a
# blank Type column and broke the type filter, with no error anywhere.
printf '%s' "$R" | grep -qE '"type_id":([0-9]+|\[[0-9]+,")' \
    && ok "type_id present as a bare id or a pair" \
    || no "type_id has an unexpected shape"

JS_EARLY=$(curl -s "$BASE/src/components/rental/RentalUnitGrid.js")
printf '%s' "$JS_EARLY" | grep -q 'typeId(u)' \
    && ok "component resolves type by id rather than assuming a pair" \
    || no "component assumes [id,label] — Type column would render blank"
printf '%s' "$JS_EARLY" | grep -q 'this.state.types.find' \
    && ok "type label looked up from the loaded types" \
    || no "no type lookup — label would be empty"

echo "    sample: $(printf '%s' "$R" | head -c 200)"

echo
echo "############ 5. every state value has a style rule ############"
# The component maps state -> CSS class and a glyph. A state in the data
# with no matching rule renders as an unstyled, unlabelled cell.
CSS=$(curl -s "$BASE/src/components/rental/rental.css")
JS=$(curl -s "$BASE/src/components/rental/RentalUnitGrid.js")
STATES=$(PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc \
         "SELECT DISTINCT state FROM rental_unit ORDER BY 1" 2>/dev/null)
for s in $STATES; do
    a=0; b=0
    printf '%s' "$CSS" | grep -q "is-$s"     && a=1
    printf '%s' "$JS"  | grep -q "'$s'"      && b=1
    if [ "$a" = "1" ] && [ "$b" = "1" ]; then ok "state '$s' has a class and a glyph"
    else no "state '$s' missing style ($a) or glyph ($b)"; fi
done

echo
echo "############ 6. the palette matches the application shell ############"
# app.css is dark and has no theme toggle, so the grid must INHERIT its
# surfaces rather than hold a second opinion. The earlier version of this
# check required a prefers-color-scheme media query — which is exactly
# what put white cells inside a dark navy app on a light-preferring OS.
printf '%s' "$CSS" | grep -q 'var(--bg,' \
    && ok "page surface inherited from app.css --bg"        || no "page surface hard-coded"
printf '%s' "$CSS" | grep -q 'var(--surface,' \
    && ok "panel surface inherited from app.css --surface"  || no "panel surface hard-coded"
printf '%s' "$CSS" | grep -q 'var(--text,' \
    && ok "ink inherited from app.css --text"               || no "ink hard-coded"
printf '%s' "$CSS" | grep -qE '@media[^{]*prefers-color-scheme' \
    && no "keyed to the OS preference — the app is dark regardless" \
    || ok "not keyed to the OS preference"
printf '%s' "$CSS" | grep -q 'data-theme="light"' \
    && ok "an explicit light variant exists for previews"    || no "no light variant"
# The unit-state fills must be the DARK-validated steps, since that is
# the surface they now sit on.
printf '%s' "$CSS" | grep -q '#3987e5' \
    && ok "occupied uses the dark-validated step"            || no "still on the light step"

echo
echo "############ 7. occupancy arithmetic ############"
# Retired units are not lettable stock, so they must not sit in the
# denominator — including them would understate occupancy forever.
printf '%s' "$JS" | grep -q 'c.occupied + c.available + c.reserved + c.maintenance' \
    && ok "retired excluded from the occupancy denominator" \
    || no "occupancy denominator does not match the documented rule"

OCC=$(pg "SELECT count(*) FROM rental_unit WHERE state='occupied'")
LET=$(pg "SELECT count(*) FROM rental_unit WHERE state <> 'retired'")
echo "    facility: $OCC occupied of $LET lettable"
[ "$LET" -gt 0 ] && ok "there is data for the grid to draw" || no "no units to display"

# Remove only what this suite created, and only if it created it.
if [ -n "$OWN_UNIT" ]; then
    pg "DELETE FROM rental_unit WHERE id=$OWN_UNIT" >/dev/null
    echo "    probe unit removed"
fi

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."

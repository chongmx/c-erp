#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# FUNCTIONAL — the parameter vocabulary, on screen.  (CERP-8)
#
# The screen over `part.parameter.keyword`, driven through the menu.
#
# Asked for: a configuration page to "look at and configure the list of
# supported parameters keywords", and a system that suggests what to do with a
# parameter name it has not seen — add it, or file it under one we have.
#
# The journey is clicks only (tests/lib/render_param_keywords.mjs): the menu,
# the decisions list, Add as new, Merge into, and the "try a name" box. This
# script seeds two parameter names that need deciding — one with nothing like
# it, one that plainly belongs under Resistance — and afterwards re-reads the
# DATABASE, because merging is the action that rewrites product rows and a
# screen that only *says* it did is worth nothing.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
PFX='ZZPK'
ADOPT="${PFX} Ripple Current"
MERGE="${PFX} Ohmic Value"

cleanup(){
    pg "DELETE FROM part_parameter WHERE product_id IN
          (SELECT id FROM product_product WHERE name LIKE '${PFX}%')" >/dev/null
    pg "DELETE FROM product_product  WHERE name LIKE '${PFX}%'" >/dev/null
    pg "DELETE FROM product_template WHERE name LIKE '${PFX}%'" >/dev/null
    pg "DELETE FROM part_parameter_alias   WHERE alias LIKE '${PFX}%'" >/dev/null
    pg "DELETE FROM part_parameter_keyword WHERE name  LIKE '${PFX}%'" >/dev/null
}
cleanup; trap cleanup EXIT
auth_or_die

# -------------------------------------------------------------------------
sec "1. two names in the catalogue that nobody has ruled on"
# -------------------------------------------------------------------------
PROD=$(pgid "INSERT INTO product_product (name, type, active) VALUES ('${PFX} Capacitor','product',true) RETURNING id")
PRO2=$(pgid "INSERT INTO product_product (name, type, active) VALUES ('${PFX} Resistor','product',true) RETURNING id")
UO=$(pg "SELECT id FROM part_unit WHERE symbol='Ω'")
t_nonempty "$PROD" "a part to hang the first name on"
t_nonempty "$PRO2" "and one for the second"
pg "INSERT INTO part_parameter (product_id,name,value_numeric,value_base)
    VALUES ($PROD,'$ADOPT',2.5,2.5)" >/dev/null
pg "INSERT INTO part_parameter (product_id,name,value_numeric,unit_id,value_base)
    VALUES ($PRO2,'$MERGE',4700,${UO:-NULL},4700)" >/dev/null
t_eq "1" "$(pg "SELECT count(*) FROM part_parameter WHERE name='$ADOPT'")" \
     "'$ADOPT' — nothing in the vocabulary is like it"
t_eq "1" "$(pg "SELECT count(*) FROM part_parameter WHERE name='$MERGE'")" \
     "'$MERGE' — plainly a way of writing Resistance"

CHROME=${CHROME_PATH:-/usr/bin/google-chrome}
if [ ! -x "$CHROME" ]; then
    echo "    NOTE  no Chrome at $CHROME — skipping the on-screen journey"
    verdict; exit $?
fi
if [ ! -d node_modules/puppeteer-core ]; then
    echo "    NOTE  puppeteer-core is not installed — skipping the on-screen journey"
    verdict; exit $?
fi
ok "Chrome and puppeteer-core are present"

# -------------------------------------------------------------------------
sec "2. the journey, on screen"
# -------------------------------------------------------------------------
OUT=$(SHOTDIR=/tmp/param_keywords BASE="$BASE" DBN="$DBN" \
      timeout 340 node tests/lib/render_param_keywords.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "both decisions can be taken from the page"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. what it actually changed, checked independently"
# -------------------------------------------------------------------------
t_eq "1" "$(pg "SELECT count(*) FROM part_parameter_keyword WHERE name='$ADOPT'")" \
     "'Add as new' added it to the vocabulary"
t_eq "1" "$(pg "SELECT count(*) FROM part_parameter WHERE name='$ADOPT'")" \
     "and left the parameter on the part exactly as it was"

t_eq "0" "$(pg "SELECT count(*) FROM part_parameter WHERE name='$MERGE'")" \
     "'Merge into' removed the odd name from the catalogue"
t_eq "Resistance" "$(pg "SELECT name FROM part_parameter WHERE product_id=${PRO2:-0}")" \
     "the part is filed under Resistance, where a parametric search will find it"
t_eq "4700" "$(pg "SELECT value_base::numeric(10,0) FROM part_parameter WHERE product_id=${PRO2:-0}")" \
     "with its value untouched — a rename is not a re-measure"
RESID=$(pg "SELECT id FROM part_parameter_keyword WHERE name='Resistance'")
t_eq "$RESID" "$(pg "SELECT keyword_id FROM part_parameter_alias WHERE alias='$MERGE'")" \
     "and the old spelling survives as an alias, so the question never comes back"

verdict

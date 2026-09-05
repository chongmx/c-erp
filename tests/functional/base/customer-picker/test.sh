#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# "I made it a customer and it still is not an option on a new contract."
#
# Reported three times. Every API test stayed green throughout, and that is the
# point of this file: none of them created a contact the way a person does.
# They all did
#
#     create res.partner {name, is_company: true, customer_rank: 1}
#
# in one call — already labelled, already a customer. The reported journey is
# different in a way that matters:
#
#     new contact -> Save -> reopen -> click Customer -> Save
#
# Two round trips, a badge click, and a second write over an existing row. A
# label that does not persist, a save that archives the row, a picker that
# holds the list it fetched before the contact existed — every one of those
# breaks the journey while leaving `create` perfectly correct.
#
# So this drives the whole thing by CLICKING, and plants nothing over the API.
# tests/lib/render_customer_flow.mjs does the driving; this file owns the
# fixtures, the cleanup and the verdict.
#
# It also checks the one thing that turned out to matter most in practice:
# whether the page has actually loaded the widget at all. A browser tab opened
# before the frontend changed keeps running the old code however current the
# server is, and reports exactly this symptom.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZCPK'
cleanup() {
    pg "DELETE FROM rental_contract WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "UPDATE res_partner SET parent_id=NULL WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

CHROME=${CHROME_PATH:-/usr/bin/google-chrome}

# -------------------------------------------------------------------------
sec "1. the browser tooling"
# -------------------------------------------------------------------------
# Skip rather than fail when the tooling is absent: a missing Chrome is a
# missing tool, and a suite that goes red for that teaches people to ignore it.
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
sec "2. the reported journey, driven by clicking"
# -------------------------------------------------------------------------
#   A. new contact, no labels          -> Save
#   B. reopen, click Customer          -> Save
#   C. new rental contract: findable in the Customer picker
#   D. second contact: Individual + Customer together
#   E. pick it and SAVE the contract; the stored partner_id must match
OUT=$(SHOTDIR=/tmp/customer_flow_test BASE="$BASE" DBN="$DBN" \
      timeout 300 node tests/lib/render_customer_flow.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "the whole journey works on screen"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the same facts, checked independently of the driver"
# -------------------------------------------------------------------------
# The driver reports its own PASS lines. Assert the database separately, so a
# driver that silently stopped early cannot report success by omission.
CO=$(pg "SELECT id FROM res_partner WHERE name='${PFX} Big Carrot Bhd'")
t_nonempty "$CO" "the company was created through the form"
t_ge "$(pg "SELECT customer_rank FROM res_partner WHERE id=${CO:-0}")" "1" \
     "clicking Customer and saving really set customer_rank"
t_eq "t" "$(pg "SELECT active FROM res_partner WHERE id=${CO:-0}")" \
     "and left it active — an archived contact would vanish from every picker"

IND=$(pg "SELECT id FROM res_partner WHERE name='${PFX} Ind Customer'")
t_nonempty "$IND" "the individual customer was created through the form"
t_eq "t" "$(pg "SELECT is_individual FROM res_partner WHERE id=${IND:-0}")" \
     "Individual persisted"
t_ge "$(pg "SELECT customer_rank FROM res_partner WHERE id=${IND:-0}")" "1" \
     "Customer persisted alongside it"

t_eq "$IND" "$(pg "SELECT partner_id FROM rental_contract WHERE name='${PFX}-RC-UI'")" \
     "the contract saved from the screen carries the customer that was picked"

# -------------------------------------------------------------------------
sec "4. the server would have offered it all along"
# -------------------------------------------------------------------------
# Separating "the server can find it" from "the screen shows it" is what tells
# you which half to go and look at when this is reported again.
FOUND=$(call_k res.partner search_read "[[[\"name\",\"ilike\",\"${PFX} Big Carrot\"]]]" \
        '"fields":["id","name"],"limit":20,"order":"name ASC"')
t_contains "$FOUND" "${PFX} Big Carrot Bhd" "search_read finds the customer by name"
t_eq "1" "$(call res.partner search_count "[[[\"name\",\"=\",\"${PFX} Big Carrot Bhd\"]]]" | rid)" \
     "search_count agrees"

# A picker resolves its CURRENT value by id, so a saved contract reopens with
# the customer shown even if it is not on the first page of results.
t_contains "$(call res.partner read "[[${CO:-0}],[\"name\"]]")" "${PFX} Big Carrot Bhd" \
    "and read() resolves it by id, which is how a saved value redisplays"

# -------------------------------------------------------------------------
sec "5. a customer with no label at all is still selectable"
# -------------------------------------------------------------------------
# There is no customer_rank filter on the contract's picker, and there should
# not be: a contact created in a hurry, with no badges, must still be usable on
# a contract. Asserting it stops anyone "tidying up" by adding a domain.
PLAIN=$(call res.partner create "[{\"name\":\"${PFX} Unlabelled Co\"}]" | rid)
t_eq "0" "$(pg "SELECT customer_rank FROM res_partner WHERE id=${PLAIN:-0}")" \
     "the contact has no customer label"
t_contains "$(call_k res.partner search_read "[[[\"name\",\"ilike\",\"${PFX} Unlabelled\"]]]" \
            '"fields":["id","name"],"limit":20')" "${PFX} Unlabelled Co" \
    "and it is still offered to the Customer picker"

# -------------------------------------------------------------------------
sec "6. an archived contact is NOT offered"
# -------------------------------------------------------------------------
# The one case where disappearing from the picker is correct.
call res.partner write "[[${PLAIN:-0}],{\"active\":false}]" >/dev/null
t_lacks "$(call_k res.partner search_read "[[[\"name\",\"ilike\",\"${PFX} Unlabelled\"]]]" \
         '"fields":["id","name"],"limit":20')" "${PFX} Unlabelled Co" \
    "an archived contact drops out of the picker"

verdict

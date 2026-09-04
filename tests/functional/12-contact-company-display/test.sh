#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# The company name a PERSON sees, at every place they see it.
#
# Reported: "I created a customer company, then an individual and selected that
# company as her company. Her company name does not show up in the list."
#
# The cause was a half-finished change of mine. docs/130 phase 5 clears
# company_name once parent_id is set, because the relation is then the source of
# truth — and the contact list rendered that very column. The source of truth
# moved; the display did not. Every linked contact showed a blank Company cell.
#
# Odoo does not have this problem because it derives a SECOND value
# (res_partner.py:228, 300-303):
#
#     commercial_company_name = commercial_partner.is_company
#                             ? commercial_partner.name     -- the linked company
#                             : company_name                -- imported free text
#
# So this test walks the user's exact steps and then checks the name is present
# in every representation the UI can read: the list view's own columns, a plain
# search_read, and the record itself. A test that only checked the database
# column would have passed throughout the bug.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZCD'
cleanup() {
    pg "UPDATE res_partner SET parent_id=NULL WHERE name LIKE '${PFX} %'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX} %'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

# -------------------------------------------------------------------------
sec "1. the user's steps, exactly"
# -------------------------------------------------------------------------
CO=$(call res.partner create "[{\"name\":\"${PFX} Acme Bhd\",\"is_company\":true,
     \"customer_rank\":1,\"email\":\"ap@acme.test\"}]" | rid)
t_nonempty "$CO" "a customer COMPANY is created"

JANE=$(call res.partner create "[{\"name\":\"${PFX} Jane Tan\",\"parent_id\":$CO,
       \"customer_rank\":1,\"email\":\"jane@acme.test\"}]" | rid)
t_nonempty "$JANE" "an INDIVIDUAL is created with that company selected"
t_eq "$CO" "$(pg "SELECT parent_id FROM res_partner WHERE id=$JANE")" \
     "the link is stored"

# -------------------------------------------------------------------------
sec "2. THE BUG: her company name must appear in the list"
# -------------------------------------------------------------------------
# The list view declares its own columns. Read them, then fetch exactly those
# — which is what the UI does — so this test fails if the arch names a column
# the record cannot fill.
# get_views returns the list AND form archs together; the FORM legitimately
# still offers company_name for imported data, so scope this to the <list> arch.
LISTARCH=$(call res.partner get_views '[[],["list"]]' | grep -oE '<list>.*</list>' | head -1)
t_nonempty "$LISTARCH" "the list view has an arch"
# Extract the LIST's field names and assert the set directly. A substring test
# cannot work here: "commercial_company_name" CONTAINS "company_name", so
# t_lacks would fail on the very column we want.
# The arch arrives JSON-escaped, so a field reads  name=\"company_name\".
# The quote immediately before the name is what separates the two columns:
#   "company_name              <- the raw field
#   "commercial_company_name   <- the derived one (preceded by an l, not a quote)
# so searching for the quoted form distinguishes them where a plain substring
# match cannot.
case "$LISTARCH" in
    *'"company_name'*) no "the LIST still renders raw company_name (phase 5 empties it)" ;;
    *) ok "the LIST does not render raw company_name (phase 5 empties it)" ;;
esac
case "$LISTARCH" in
    *commercial_company_name*) ok "the LIST renders the derived company name instead" ;;
    *) no "the LIST does not render commercial_company_name" ;;
esac

ROW=$(call res.partner search_read "[[[\"id\",\"=\",$JANE]],
      [\"name\",\"commercial_company_name\",\"parent_id\"]]")
t_contains "$ROW" "${PFX} Acme Bhd" \
    "her row carries the company NAME — the reported symptom"

# The database column behind it, so a failure says which layer broke.
t_eq "${PFX} Acme Bhd" "$(pgv "SELECT COALESCE(commercial_company_name,'') FROM res_partner WHERE id=$JANE")" \
     "stored on the row, so the list can sort and filter on it"

# -------------------------------------------------------------------------
sec "3. and it is right for every KIND of partner"
# -------------------------------------------------------------------------
t_eq "${PFX} Acme Bhd" "$(pgv "SELECT COALESCE(commercial_company_name,'') FROM res_partner WHERE id=$CO")" \
     "a company shows its own name"

IMPORTED=$(call res.partner create "[{\"name\":\"${PFX} Imported Person\",
           \"company_name\":\"Legacy Trading Co\"}]" | rid)
t_eq "Legacy Trading Co" "$(pgv "SELECT COALESCE(commercial_company_name,'') FROM res_partner WHERE id=$IMPORTED")" \
     "a partner with only free text still shows it (imported data keeps working)"

LONE=$(call res.partner create "[{\"name\":\"${PFX} Walk In\"}]" | rid)
t_eq "" "$(pg "SELECT COALESCE(commercial_company_name,'') FROM res_partner WHERE id=$LONE")" \
     "a person with no company shows nothing, not the word 'false'"

# -------------------------------------------------------------------------
sec "4. renaming the company reaches her"
# -------------------------------------------------------------------------
# The reason to derive rather than copy: one edit, everyone follows.
call res.partner write "[[$CO],{\"name\":\"${PFX} Acme Holdings Bhd\"}]" >/dev/null
t_eq "${PFX} Acme Holdings Bhd" \
     "$(pgv "SELECT COALESCE(commercial_company_name,'') FROM res_partner WHERE id=$JANE")" \
     "renaming the company updates what her row displays"

# -------------------------------------------------------------------------
sec "5. re-homing her moves the name too"
# -------------------------------------------------------------------------
OTHER=$(call res.partner create "[{\"name\":\"${PFX} Rival Bhd\",\"is_company\":true}]" | rid)
call res.partner write "[[$JANE],{\"parent_id\":$OTHER}]" >/dev/null
t_eq "${PFX} Rival Bhd" \
     "$(pgv "SELECT COALESCE(commercial_company_name,'') FROM res_partner WHERE id=$JANE")" \
     "moving her to another company shows the new one"

call res.partner write "[[$JANE],{\"parent_id\":false}]" >/dev/null
t_eq "" "$(pgv "SELECT COALESCE(commercial_company_name,'') FROM res_partner WHERE id=$JANE")" \
     "detaching her clears it — she works nowhere now"

# -------------------------------------------------------------------------
sec "6. searching and sorting by company"
# -------------------------------------------------------------------------
# What the column is FOR, beyond being visible.
call res.partner write "[[$JANE],{\"parent_id\":$CO}]" >/dev/null
BOB=$(call res.partner create "[{\"name\":\"${PFX} Bob Lee\",\"parent_id\":$CO}]" | rid)
FOUND=$(call res.partner search_count \
        "[[[\"commercial_company_name\",\"=\",\"${PFX} Acme Holdings Bhd\"]]]" | rid)
t_ge "${FOUND:-0}" "3" "filtering by company finds the company and both its people"

# -------------------------------------------------------------------------
sec "7. a client cannot fake it"
# -------------------------------------------------------------------------
# It is derived. If a caller could set it, the Company column would become a
# free-text field again with extra steps.
call res.partner write "[[$BOB],{\"commercial_company_name\":\"Totally Different Co\"}]" >/dev/null 2>&1
t_ne "Totally Different Co" \
     "$(pgv "SELECT COALESCE(commercial_company_name,'') FROM res_partner WHERE id=$BOB")" \
     "a client write cannot override the derived name"

verdict

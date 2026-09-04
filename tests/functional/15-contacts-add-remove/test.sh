#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Adding and removing contacts — the whole round trip.
#
# Contacts → Contacts, as a person uses it: add a company, add the people who
# work there, find them in the list, correct a typo, remove someone who was
# added by mistake, and remove a company that turned out not to be a customer.
# Then the two cases that are not simply "delete": a contact with history, and
# getting an archived contact back.
#
# tests/integration/core/contact-delete asserts the deletion RULES in detail —
# which of twenty relations block, which are cleared. This file asserts the
# JOURNEY: that each step leaves the next one possible, and that what the user
# sees after each step is what actually happened.
#
# The list is checked through the view's own columns rather than by reading
# the table, because the two disagreed once before: setting a company on a
# contact cleared the free-text company_name that the list was rendering, so
# every linked contact showed a blank Company cell while the database was
# perfectly correct.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZCAR'
cleanup() {
    pg "DELETE FROM rental_contract WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "UPDATE res_partner SET parent_id=NULL WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

# What the Contacts list actually asks the server for.
LISTCOLS='["name","commercial_company_name","email","phone","active"]'
inlist() {   # inlist <name-fragment> -> the matching rows, as the list sees them
    call_k res.partner search_read "[[[\"name\",\"ilike\",\"$1\"]]]" \
        "\"fields\":$LISTCOLS,\"limit\":50,\"order\":\"name ASC\""
}

# -------------------------------------------------------------------------
sec "1. add a customer company"
# -------------------------------------------------------------------------
CO=$(call res.partner create "[{\"name\":\"${PFX} Green Valley Sdn Bhd\",\"is_company\":true,
     \"customer_rank\":1,\"email\":\"hello@greenvalley.test\",\"phone\":\"04-1234567\",
     \"city\":\"Ipoh\"}]" | rid)
t_nonempty "$CO" "the company is added"
t_contains "$(inlist "${PFX} Green Valley")" "${PFX} Green Valley Sdn Bhd" \
    "and appears in the Contacts list"
t_contains "$(inlist "${PFX} Green Valley")" "hello@greenvalley.test" \
    "with the details that were typed"

# -------------------------------------------------------------------------
sec "2. add the people who work there"
# -------------------------------------------------------------------------
AMY=$(call res.partner create "[{\"name\":\"${PFX} Amy Lim\",\"parent_id\":$CO,
      \"email\":\"amy@greenvalley.test\",\"job_position\":\"Director\"}]" | rid)
BEN=$(call res.partner create "[{\"name\":\"${PFX} Ben Ooi\",\"parent_id\":$CO,
      \"email\":\"ben@greenvalley.test\"}]" | rid)
t_nonempty "$AMY" "the first contact is added"
t_nonempty "$BEN" "the second contact is added"

ROWS=$(inlist "${PFX}")
t_contains "$ROWS" "${PFX} Amy Lim" "Amy is in the list"
t_contains "$ROWS" "${PFX} Ben Ooi" "Ben is in the list"
# The reported bug from before: the Company column must not be blank.
t_contains "$ROWS" "${PFX} Green Valley Sdn Bhd" \
    "and their rows show which company they work for"
t_eq "2" "$(pg "SELECT count(*) FROM res_partner WHERE parent_id=$CO")" \
     "the company has two people"

# -------------------------------------------------------------------------
sec "3. correct a typo"
# -------------------------------------------------------------------------
# Editing must not disturb the company link — a save that quietly cleared it
# is exactly the failure the picker rewrite was about.
call res.partner write "[[$BEN],{\"name\":\"${PFX} Ben Ooi Wei\",\"phone\":\"012-9998888\"}]" >/dev/null
t_eq "${PFX} Ben Ooi Wei" "$(pgv "SELECT name FROM res_partner WHERE id=$BEN")" "the name is corrected"
t_eq "$CO" "$(pg "SELECT parent_id FROM res_partner WHERE id=$BEN")" \
     "and he is still linked to the company"
t_eq "${PFX} Green Valley Sdn Bhd" \
     "$(pgv "SELECT COALESCE(commercial_company_name,'') FROM res_partner WHERE id=$BEN")" \
     "his Company cell still reads correctly"

# -------------------------------------------------------------------------
sec "4. remove a contact added by mistake"
# -------------------------------------------------------------------------
OOPS=$(call res.partner create "[{\"name\":\"${PFX} Wrong Person\",\"parent_id\":$CO}]" | rid)
t_nonempty "$OOPS" "a contact is added by mistake"
t_eq "3" "$(pg "SELECT count(*) FROM res_partner WHERE parent_id=$CO")" "the company now has three"

CHK=$(call res.partner check_unlink "[[$OOPS]]")
t_contains "$CHK" '"blockers":[]' "nothing depends on them, so they can go"

OUT=$(call res.partner unlink "[[$OOPS]]")
if has_error "$OUT"; then no "removing a fresh contact failed: $(echo "$OUT" | head -c 140)"
else ok "the contact is removed"; fi
t_eq "0" "$(pg "SELECT count(*) FROM res_partner WHERE id=$OOPS")" "the row is gone"
t_lacks "$(inlist "${PFX} Wrong")" "${PFX} Wrong Person" "and it has left the list"
t_eq "2" "$(pg "SELECT count(*) FROM res_partner WHERE parent_id=$CO")" \
     "the other two are untouched"

# -------------------------------------------------------------------------
sec "5. remove a company — its people survive"
# -------------------------------------------------------------------------
# Deleting an employer must not delete the people. They become independent
# contacts, which is recoverable; deleting them would not be.
DEAD=$(call res.partner create "[{\"name\":\"${PFX} Not A Customer Bhd\",\"is_company\":true}]" | rid)
TEMP=$(call res.partner create "[{\"name\":\"${PFX} Temp Contact\",\"parent_id\":$DEAD}]" | rid)

CHK=$(call res.partner check_unlink "[[$DEAD]]")
t_contains "$CHK" 'contacts working at this company' \
    "the confirmation warns that a contact will be detached"

OUT=$(call res.partner unlink "[[$DEAD]]")
if has_error "$OUT"; then no "removing an unused company failed"
else ok "the company is removed"; fi
t_eq "0" "$(pg "SELECT count(*) FROM res_partner WHERE id=$DEAD")" "the company is gone"
t_eq "1" "$(pg "SELECT count(*) FROM res_partner WHERE id=$TEMP")" "its contact is NOT deleted"
t_eq "" "$(pg "SELECT COALESCE(parent_id::text,'') FROM res_partner WHERE id=$TEMP")" \
     "they are detached rather than left pointing at nothing"
t_contains "$(inlist "${PFX} Temp")" "${PFX} Temp Contact" \
    "and they are still in the Contacts list"

# -------------------------------------------------------------------------
sec "6. a contact with history is archived, not removed"
# -------------------------------------------------------------------------
# Give the company a rental contract, then try to remove it.
call rental.contract create "[{\"name\":\"${PFX} RC-9\",\"partner_id\":$CO,
     \"billing_period\":\"monthly\",\"date_start\":\"2026-01-01\"}]" >/dev/null

OUT=$(call res.partner unlink "[[$CO]]")
if has_error "$OUT"; then ok "a company with a contract cannot be removed"
else no "a company with a rental contract was removed"; fi
t_contains "$OUT" "rental contracts (1)" "the reason names the contract"
t_eq "1" "$(pg "SELECT count(*) FROM res_partner WHERE id=$CO")" "the company is still there"

call res.partner write "[[$CO],{\"active\":false}]" >/dev/null
t_eq "f" "$(pg "SELECT active FROM res_partner WHERE id=$CO")" "it is archived instead"
t_lacks "$(inlist "${PFX} Green Valley")" "${PFX} Green Valley Sdn Bhd" \
    "and it drops out of the Contacts list"

# The picker must not offer it either — an archived customer should not be
# selectable on a new contract.
t_lacks "$(call_k res.partner search_read "[[[\"is_company\",\"=\",true],
           [\"name\",\"ilike\",\"${PFX} Green\"]]]" '"fields":["id","name"],"limit":20')" \
    "${PFX} Green Valley Sdn Bhd" "nor is it offered in a picker"

# -------------------------------------------------------------------------
sec "7. an archived contact can be found and brought back"
# -------------------------------------------------------------------------
# Archiving must not be a one-way door. Naming `active` in the domain is what
# the list's "Show archived" button does.
SHOWN=$(call_k res.partner search_read \
        "[[\"|\",[\"active\",\"=\",true],[\"active\",\"=\",false],
           [\"name\",\"ilike\",\"${PFX} Green\"]]]" \
        "\"fields\":$LISTCOLS,\"limit\":50")
t_contains "$SHOWN" "${PFX} Green Valley Sdn Bhd" \
    "'Show archived' finds it again"

call res.partner write "[[$CO],{\"active\":true}]" >/dev/null
t_eq "t" "$(pg "SELECT active FROM res_partner WHERE id=$CO")" "it is unarchived"
t_contains "$(inlist "${PFX} Green Valley")" "${PFX} Green Valley Sdn Bhd" \
    "and it is back in the ordinary list"

# Its people were never touched by any of this.
t_eq "2" "$(pg "SELECT count(*) FROM res_partner WHERE parent_id=$CO")" \
     "both of its contacts are still attached"

# -------------------------------------------------------------------------
sec "8. a name can be reused after removal"
# -------------------------------------------------------------------------
# There is no unique constraint on a contact name, and there should not be:
# two people really are called the same thing, and a removed contact's name
# must not be reserved forever.
AGAIN=$(call res.partner create "[{\"name\":\"${PFX} Temp Contact\"}]" | rid)
t_nonempty "$AGAIN" "a contact can be created with a name already in use"
t_ge "$(pg "SELECT count(*) FROM res_partner WHERE name='${PFX} Temp Contact'")" "2" \
     "both exist independently"

verdict

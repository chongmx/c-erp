#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Adding and removing contacts, ENTIRELY BY CLICKING.
#
# Contacts → Contacts, as a person uses it: add a company, add the people who
# work there, find them in the list, correct a typo, remove someone added by
# mistake, remove a company that turned out not to be a customer. Then the two
# cases that are not simply "delete": a contact with history, and getting an
# archived contact back.
#
# The earlier version did all of this over the API. It proved the deletion
# RULES — which of twenty relations block, which are cleared — and nothing at
# all about whether a person can perform any of it. The rules are still
# covered, in detail, by tests/integration/core/contact-delete; this file is
# the journey.
#
# Converting it found that the Contacts list rendered its columns in JSON key
# order rather than the order the view declares, so the list led with "Active"
# and buried "Name" in the fourth column. The same bug made the rental contract
# form read Active, Billing Period, Currency, Start Date… in no order anyone
# would choose. Both now follow the arch.
#
# The list is checked through the browser's own table rather than by reading
# the database, because those two disagreed once before: setting a company on a
# contact cleared the free-text company_name the list was rendering, and every
# linked contact showed a blank Company cell while the database was perfectly
# correct.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZCAR'
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
if [ ! -x "$CHROME" ]; then
    echo "    NOTE  no Chrome at $CHROME — skipping (this test is click-driven only)"
    verdict; exit $?
fi
if [ ! -d node_modules/puppeteer-core ]; then
    echo "    NOTE  puppeteer-core is not installed — skipping"
    verdict; exit $?
fi
ok "Chrome and puppeteer-core are present"

# -------------------------------------------------------------------------
sec "2. the journey, by clicking"
# -------------------------------------------------------------------------
OUT=$(SHOTDIR=/tmp/contacts_test BASE="$BASE" DBN="$DBN" \
      timeout 400 node tests/lib/render_contacts.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "the whole add/remove journey works on screen"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the same facts, checked against the database"
# -------------------------------------------------------------------------
# The driver reports its own PASS lines; assert independently so a driver that
# stopped early cannot pass by omission.
CO=$(pg "SELECT id FROM res_partner WHERE name='${PFX} Green Valley Sdn Bhd'")
t_nonempty "$CO" "the company exists"
t_eq "t" "$(pg "SELECT is_company FROM res_partner WHERE id=${CO:-0}")" "it is a company"
t_eq "t" "$(pg "SELECT active FROM res_partner WHERE id=${CO:-0}")" \
     "and it is active again after being archived and restored"

t_eq "2" "$(pg "SELECT count(*) FROM res_partner WHERE parent_id=${CO:-0}")" \
     "two people still work there"
t_eq "${PFX} Green Valley Sdn Bhd" \
     "$(pgv "SELECT COALESCE(commercial_company_name,'') FROM res_partner
               WHERE name='${PFX} Ben Ooi Wei'")" \
     "the renamed contact still shows the company — editing did not drop the link"

# Removed by clicking Delete, and gone.
t_eq "0" "$(pg "SELECT count(*) FROM res_partner WHERE name='${PFX} Wrong Person'")" \
     "the contact added by mistake is gone"
t_eq "0" "$(pg "SELECT count(*) FROM res_partner WHERE name='${PFX} Not A Customer Bhd'")" \
     "so is the company that was not a customer"

# Its person survived, detached rather than deleted.
t_ge "$(pg "SELECT count(*) FROM res_partner WHERE name='${PFX} Temp Contact'")" "1" \
     "that company's contact was NOT deleted with it"
t_eq "0" "$(pg "SELECT count(*) FROM res_partner
                 WHERE name='${PFX} Temp Contact' AND parent_id IS NOT NULL")" \
     "and is detached rather than pointing at a row that no longer exists"

# -------------------------------------------------------------------------
sec "4. history survived the archive"
# -------------------------------------------------------------------------
# Archiving is offered precisely so a contact with documents can be taken out
# of the way without destroying what it is attached to.
t_eq "1" "$(pg "SELECT count(*) FROM rental_contract WHERE name='${PFX}-RC-9'")" \
     "the rental contract raised during the journey is intact"
t_eq "$CO" "$(pg "SELECT partner_id FROM rental_contract WHERE name='${PFX}-RC-9'")" \
     "and still points at the company"

# -------------------------------------------------------------------------
sec "5. a name is not reserved by a deletion"
# -------------------------------------------------------------------------
t_ge "$(pg "SELECT count(*) FROM res_partner WHERE name='${PFX} Temp Contact'")" "2" \
     "the same contact name can be used again"

verdict

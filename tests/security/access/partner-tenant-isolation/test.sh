#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Two "company" concepts on res.partner, and neither may eat the other.
#
#   parent_id   the CUSTOMER'S company. Acme Sdn Bhd, who you sell to. Every
#               ERP user may create these freely — customers and suppliers are
#               the business, and adding a contact under one is ordinary work.
#
#   company_id  the TENANT that owns the row: which ERP subscriber's data this
#               is. Stamped by the server (docs/094). A user may never set it,
#               and must never see another tenant's rows.
#
# CONFUSING THEM IS NOT THEORETICAL. The contact form used to put the customer's
# company id into company_id, so saving a contact sent company_id=<partner id>.
# No res.company has that id, the multi-company guard refused with
#
#     "You cannot create records for another company."
#
# and the user simply could not add a contact to a customer. The guard was
# right; the caller was wrong. This test pins both halves so the two fields can
# never be swapped again:
#
#   1-3  adding customers, suppliers and their contacts must WORK
#   4-6  tenant isolation must HOLD, and must not be settable from outside
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZTI'
cleanup() {
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX} %' AND parent_id IS NOT NULL" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX} %'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup

auth_or_die

MYCO=$(pg "SELECT company_id FROM res_users WHERE login='admin'")
[ -z "$MYCO" ] && MYCO=$(pg "SELECT id FROM res_company ORDER BY id LIMIT 1")

# -------------------------------------------------------------------------
sec "1. an ERP user can add a CUSTOMER company"
# -------------------------------------------------------------------------
CUST=$(call res.partner create "[{\"name\":\"${PFX} Acme Bhd\",\"is_company\":true,
       \"customer_rank\":1,\"company_name\":\"${PFX} Acme Bhd\"}]" | rid)
t_nonempty "$CUST" "customer company created"
t_eq "$MYCO" "$(pg "SELECT company_id FROM res_partner WHERE id=$CUST")" \
     "the server stamped it with MY tenant, not something the client chose"

# -------------------------------------------------------------------------
sec "2. and a SUPPLIER company"
# -------------------------------------------------------------------------
SUPP=$(call res.partner create "[{\"name\":\"${PFX} Bolt Supplies\",\"is_company\":true,
       \"vendor_rank\":1}]" | rid)
t_nonempty "$SUPP" "supplier company created"

# -------------------------------------------------------------------------
sec "3. and CONTACTS under both — the thing that was blocked"
# -------------------------------------------------------------------------
# Exactly the payload the contact form sends: parent_id for the customer's
# company, and NO company_id.
JANE=$(call res.partner create "[{\"name\":\"${PFX} Jane Tan\",\"parent_id\":$CUST,
       \"company_name\":\"${PFX} Acme Bhd\",\"email\":\"jane@acme.test\",
       \"job_position\":\"Buyer\"}]" | rid)
t_nonempty "$JANE" "a contact under the CUSTOMER company saves"

BOB=$(call res.partner create "[{\"name\":\"${PFX} Bob Lee\",\"parent_id\":$SUPP,
      \"company_name\":\"${PFX} Bolt Supplies\"}]" | rid)
t_nonempty "$BOB" "a contact under the SUPPLIER company saves"

t_eq "$CUST" "$(pg "SELECT parent_id FROM res_partner WHERE id=$JANE")" \
     "Jane is linked to Acme through parent_id"
t_eq "$MYCO" "$(pg "SELECT company_id FROM res_partner WHERE id=$JANE")" \
     "and still belongs to MY tenant"

# The regression itself: the old form sent the partner id as company_id.
BAD=$(call res.partner create "[{\"name\":\"${PFX} Wrong\",\"company_id\":$CUST}]")
if has_error "$BAD"; then
    ok "sending a PARTNER id as company_id is refused (the old form's bug)"
    printf '          %s\n' "$(jfield "$BAD" message)"
else
    no "a partner id was accepted as company_id — the tenant field is writable"
fi

# -------------------------------------------------------------------------
sec "4. the customer list is visible to its own tenant"
# -------------------------------------------------------------------------
MINE=$(call res.partner search_read "[[[\"name\",\"like\",\"${PFX}\"]],[\"name\"]]")
for who in "Acme Bhd" "Bolt Supplies" "Jane Tan" "Bob Lee"; do
    t_contains "$MINE" "$who" "I can see my own '$who'"
done

# -------------------------------------------------------------------------
sec "5. another tenant's partners are NOT visible"
# -------------------------------------------------------------------------
# Plant a partner owned by a different tenant directly in SQL — the API will not
# let us create one, which is the point of section 3's last check.
OTHER=$(pg "SELECT id FROM res_company WHERE id <> ${MYCO:-0} ORDER BY id LIMIT 1")
if [ -z "$OTHER" ]; then
    OTHER=$(pgid "INSERT INTO res_company (name) VALUES ('${PFX} Rival Tenant') RETURNING id")
    MADE_CO=1
fi
if [ -n "$OTHER" ]; then
    RIVAL=$(pgid "INSERT INTO res_partner (name, is_company, company_id, customer_rank)
                  VALUES ('${PFX} Rival Customer', TRUE, $OTHER, 1) RETURNING id")
    t_nonempty "$RIVAL" "a rival tenant's customer exists in the database"

    SEEN=$(call res.partner search_read "[[[\"name\",\"like\",\"${PFX}\"]],[\"name\"]]")
    t_lacks "$SEEN" "Rival Customer" \
        "it does NOT appear in my customer list (tenant isolation holds)"

    DIRECT=$(call res.partner read "[[$RIVAL],[\"name\"]]")
    t_lacks "$DIRECT" "Rival Customer" \
        "nor can I read it by id, even knowing the id"

    WROTE=$(call res.partner write "[[$RIVAL],{\"name\":\"${PFX} Hijacked\"}]")
    t_eq "${PFX} Rival Customer" "$(pgv "SELECT name FROM res_partner WHERE id=$RIVAL")" \
        "nor write to it"

    pg "DELETE FROM res_partner WHERE id=$RIVAL" >/dev/null
    [ "${MADE_CO:-0}" = "1" ] && pg "DELETE FROM res_company WHERE id=$OTHER" >/dev/null
else
    echo "    SKIP  could not obtain a second tenant"
fi

# -------------------------------------------------------------------------
sec "6. the two fields stay distinct in the schema"
# -------------------------------------------------------------------------
# A future refactor that points either one at the wrong table reintroduces the
# whole bug, so assert the relations themselves.
FORM=$(call res.partner get_views '[[],["form"]]')
t_contains "$FORM" '"relation":"res.partner"' "parent_id relates to res.partner (the customer)"
t_contains "$FORM" '"relation":"res.company"' "company_id relates to res.company (the tenant)"
t_eq "res_partner" \
     "$(pg "SELECT ccu.table_name FROM information_schema.table_constraints tc
              JOIN information_schema.constraint_column_usage ccu ON ccu.constraint_name=tc.constraint_name
             WHERE tc.constraint_name='res_partner_parent_fk'")" \
     "parent_id's foreign key really points at res_partner"

verdict

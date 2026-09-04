#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# The partner hierarchy, phases 1-5 of docs/130.
#
# contact-company-link proves parent_id exists. This proves the five things
# built on top of it behave like the reference implementation (Odoo 14
# res_partner.py, vendored at zzref2/odoo14):
#
#   1  commercial_partner_id   the company at the TOP of the chain (:289)
#   2  tenant descends         a contact inherits its company's tenant (:369)
#   3  type                    contact / invoice / delivery / other (:185)
#   4  address inheritance     a contact adopts its company's address (:344)
#   5  company_name cleared    the relation wins over the free text (:529)
#
# Every one of these is maintained in the DATABASE (trigger or constraint) or at
# the service boundary, never by the client, because the whole reason this work
# exists is that a client was trusted with a field it did not understand and
# quietly filed contacts under the wrong company.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZPH'
cleanup() {
    pg "UPDATE res_partner SET parent_id = NULL WHERE name LIKE '${PFX} %'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX} %'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

# -------------------------------------------------------------------------
sec "0. the migrations landed"
# -------------------------------------------------------------------------
for c in commercial_partner_id type street2; do
    column_exists res_partner "$c" && ok "res_partner.$c exists" \
                                   || no "res_partner.$c MISSING — migration 10/12 did not run"
done
t_eq "3" "$(pg "SELECT count(*) FROM schema_migrations WHERE version IN (10,11,12)")" \
     "migrations 10, 11 and 12 are recorded as applied"

# -------------------------------------------------------------------------
sec "1. commercial_partner_id — the company at the top"
# -------------------------------------------------------------------------
ACME=$(call res.partner create "[{\"name\":\"${PFX} Acme Bhd\",\"is_company\":true,
       \"street\":\"12 Jalan Sultan\",\"city\":\"Kuala Lumpur\",\"zip\":\"50000\"}]" | rid)
t_eq "$ACME" "$(pg "SELECT commercial_partner_id FROM res_partner WHERE id=$ACME")" \
     "a company is its own commercial entity"

JANE=$(call res.partner create "[{\"name\":\"${PFX} Jane\",\"parent_id\":$ACME}]" | rid)
t_eq "$ACME" "$(pg "SELECT commercial_partner_id FROM res_partner WHERE id=$JANE")" \
     "a contact resolves to its company"

# The depth case the old OR query got wrong.
BRANCH=$(call res.partner create "[{\"name\":\"${PFX} Acme North\",\"parent_id\":$ACME}]" | rid)
DEEP=$(call res.partner create "[{\"name\":\"${PFX} Deep Lee\",\"parent_id\":$BRANCH}]" | rid)
t_eq "$ACME" "$(pg "SELECT commercial_partner_id FROM res_partner WHERE id=$DEEP")" \
     "a contact under a BRANCH still resolves to the top company (depth 2)"

# Re-parenting must move the whole subtree, not just the row touched.
OTHERCO=$(call res.partner create "[{\"name\":\"${PFX} Rival Bhd\",\"is_company\":true}]" | rid)
call res.partner write "[[$BRANCH],{\"parent_id\":$OTHERCO}]" >/dev/null
t_eq "$OTHERCO" "$(pg "SELECT commercial_partner_id FROM res_partner WHERE id=$BRANCH")" \
     "re-parenting the branch updates the branch"
t_eq "$OTHERCO" "$(pg "SELECT commercial_partner_id FROM res_partner WHERE id=$DEEP")" \
     "and CASCADES to the contact beneath it"

# The payoff: one indexed equality replaces the OR across two columns.
t_eq "2" "$(pg "SELECT count(*) FROM res_partner WHERE commercial_partner_id=$ACME")" \
     "'everything for Acme' is now a single equality (Acme + Jane)"

# -------------------------------------------------------------------------
sec "2. the tenant descends the hierarchy"
# -------------------------------------------------------------------------
MYCO=$(pg "SELECT company_id FROM res_partner WHERE id=$ACME")
t_nonempty "$MYCO" "the company has a tenant"
t_eq "$MYCO" "$(pg "SELECT company_id FROM res_partner WHERE id=$JANE")" \
     "its contact inherited the SAME tenant"

# Moving a company between tenants must take its people along, or they become
# invisible to the very people who own them.
RIVALCO=$(pg "SELECT id FROM res_company WHERE id <> ${MYCO:-0} ORDER BY id LIMIT 1")
if [ -z "$RIVALCO" ]; then
    RIVALCO=$(pgid "INSERT INTO res_company (name) VALUES ('${PFX} Second Tenant') RETURNING id")
    MADE=1
fi
if [ -n "$RIVALCO" ]; then
    pg "UPDATE res_partner SET company_id=$RIVALCO WHERE id=$ACME" >/dev/null
    t_eq "$RIVALCO" "$(pg "SELECT company_id FROM res_partner WHERE id=$JANE")" \
         "moving the company moved its contact's tenant too"
    pg "UPDATE res_partner SET company_id=$MYCO WHERE id=$ACME" >/dev/null
fi

# -------------------------------------------------------------------------
sec "3. address types"
# -------------------------------------------------------------------------
t_eq "contact" "$(pgv "SELECT type FROM res_partner WHERE id=$JANE")" \
     "a contact defaults to type 'contact'"

INV=$(call res.partner create "[{\"name\":\"${PFX} Acme Accounts Payable\",
      \"parent_id\":$ACME,\"type\":\"invoice\"}]" | rid)
DEL=$(call res.partner create "[{\"name\":\"${PFX} Acme Warehouse\",
      \"parent_id\":$ACME,\"type\":\"delivery\",\"street\":\"9 Jalan Gudang\"}]" | rid)
t_nonempty "$INV" "an INVOICE address can be created under the company"
t_nonempty "$DEL" "a DELIVERY address can be created under the company"

BADT=$(call res.partner create "[{\"name\":\"${PFX} Nonsense\",\"parent_id\":$ACME,
       \"type\":\"banana\"}]")
has_error "$BADT" && ok "an invalid address type is rejected" \
                  || no "type accepted a value outside the four allowed"

t_eq "2" "$(pg "SELECT count(*) FROM res_partner
                 WHERE parent_id=$ACME AND type IN ('invoice','delivery')")" \
     "the company's addresses are findable by parent + type"
# sale_order has been carrying partner_invoice_id / partner_shipping_id with
# nothing able to fill them since SaleModule.cpp:98.
SO=$(call sale.order create "[{\"partner_id\":$ACME,\"partner_invoice_id\":$INV,
     \"partner_shipping_id\":$DEL}]" | rid)
if [ -n "$SO" ]; then
    t_eq "$INV" "$(pg "SELECT partner_invoice_id  FROM sale_order WHERE id=$SO")" \
         "a sale order can bill the invoice address"
    t_eq "$DEL" "$(pg "SELECT partner_shipping_id FROM sale_order WHERE id=$SO")" \
         "and ship to the delivery address"
    pg "DELETE FROM sale_order WHERE id=$SO" >/dev/null
fi

# -------------------------------------------------------------------------
sec "4. address inheritance"
# -------------------------------------------------------------------------
# Jane was created with no address, under a company that has one.
t_eq "12 Jalan Sultan" "$(pgv "SELECT COALESCE(street,'') FROM res_partner WHERE id=$JANE")" \
     "a contact with no address of its own adopts the company's"
t_eq "Kuala Lumpur" "$(pgv "SELECT COALESCE(city,'') FROM res_partner WHERE id=$JANE")" \
     "including the city"

# An address the caller DID supply must survive untouched.
t_eq "9 Jalan Gudang" "$(pgv "SELECT COALESCE(street,'') FROM res_partner WHERE id=$DEL")" \
     "an explicitly given address is NOT overwritten by the parent's"

# And a later edit to the company must not reach back and clobber the contact.
call res.partner write "[[$ACME],{\"street\":\"99 Jalan Baru\"}]" >/dev/null
t_eq "12 Jalan Sultan" "$(pgv "SELECT COALESCE(street,'') FROM res_partner WHERE id=$JANE")" \
     "editing the company later does NOT rewrite the contact's address"

# -------------------------------------------------------------------------
sec "5. company_name gives way to the relation"
# -------------------------------------------------------------------------
BOTH=$(call res.partner create "[{\"name\":\"${PFX} Both Given\",\"parent_id\":$ACME,
       \"company_name\":\"Typed By Hand Sdn Bhd\"}]" | rid)
t_eq "" "$(pg "SELECT COALESCE(company_name,'') FROM res_partner WHERE id=$BOTH")" \
     "setting parent_id clears the free-text company_name (no two sources of truth)"

STANDALONE=$(call res.partner create "[{\"name\":\"${PFX} No Parent\",
             \"company_name\":\"Imported Co\"}]" | rid)
t_eq "Imported Co" "$(pgv "SELECT COALESCE(company_name,'') FROM res_partner WHERE id=$STANDALONE")" \
     "but a partner with NO parent keeps it (imported data still has a home)"

# -------------------------------------------------------------------------
sec "6. the invariants still hold"
# -------------------------------------------------------------------------
SELF=$(call res.partner write "[[$ACME],{\"parent_id\":$ACME}]")
has_error "$SELF" && ok "a partner still cannot be its own company" \
                  || no "self-parenting was allowed"
CYC=$(call res.partner write "[[$ACME],{\"parent_id\":$JANE}]")
has_error "$CYC" && ok "a cycle is still refused" || no "a cycle was allowed"

GHOST=$(call res.partner create "[{\"name\":\"${PFX} Ghost\",\"invented_field\":1}]")
has_error "$GHOST" && ok "unknown fields are still rejected" \
                   || no "an invented field was accepted"

# commercial_partner_id is derived — a client must not be able to claim it.
call res.partner create "[{\"name\":\"${PFX} Claimer\",\"commercial_partner_id\":$OTHERCO}]" >/dev/null 2>&1
CLAIM=$(pg "SELECT commercial_partner_id FROM res_partner WHERE name='${PFX} Claimer'")
if [ -n "$CLAIM" ]; then
    t_ne "$OTHERCO" "$CLAIM" "a client cannot claim another company's commercial entity"
fi

[ "${MADE:-0}" = "1" ] && pg "DELETE FROM res_company WHERE id=$RIVALCO" >/dev/null
verdict

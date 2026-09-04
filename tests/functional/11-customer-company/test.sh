#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# A corporate customer, end to end.
#
# The unit test next door (integration/core/contact-company-link) proves the
# relation exists. This proves it is USEFUL: that a company you created can be
# sold to, rented to, and invoiced, and that its people can too — which is the
# whole reason to model a company rather than type its name into a text box.
#
# The story, in the order a person actually does it:
#
#   1. create Acme Sdn Bhd                      a company
#   2. add Jane and Ali to it                   its people
#   3. quote and confirm a sale to ACME         the company buys
#   4. quote a sale to JANE                     a named person buys
#   5. rent a unit to ACME                      a tenancy against the company
#   6. ask "everything for this customer"       company + its people, one answer
#
# Step 6 is the one that fails without a relation. With company_name as free
# text the only way to gather a customer's activity is to match strings, so a
# contact filed under "Acme Sdn. Bhd." is invisible next to "Acme Sdn Bhd".
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZEE'
cleanup() {
    pg "DELETE FROM rental_contract_line WHERE contract_id IN (SELECT id FROM rental_contract WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE '${PFX} %'))" >/dev/null 2>&1
    pg "DELETE FROM rental_contract      WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE '${PFX} %')" >/dev/null 2>&1
    pg "DELETE FROM rental_unit          WHERE name LIKE '${PFX} %'" >/dev/null 2>&1
    pg "DELETE FROM sale_order_line      WHERE order_id IN (SELECT id FROM sale_order WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE '${PFX} %'))" >/dev/null 2>&1
    pg "DELETE FROM sale_order           WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE '${PFX} %')" >/dev/null 2>&1
    pg "DELETE FROM product_product      WHERE name LIKE '${PFX} %'" >/dev/null 2>&1
    # children first: parent_id is ON DELETE SET NULL, but be explicit
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX} %' AND parent_id IS NOT NULL" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX} %'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup                     # in case a previous run died before its trap

auth_or_die

# -------------------------------------------------------------------------
sec "1. the customer is a company"
# -------------------------------------------------------------------------
ACME=$(call res.partner create "[{\"name\":\"${PFX} Acme Sdn Bhd\",\"is_company\":true,
       \"email\":\"ap@acme.test\",\"street\":\"12 Jalan Sultan\",\"city\":\"Kuala Lumpur\",
       \"zip\":\"50000\",\"customer_rank\":1}]" | rid)
t_nonempty "$ACME" "Acme Sdn Bhd created"
t_eq "t" "$(pg "SELECT is_company FROM res_partner WHERE id=$ACME")" "it is a company"

# -------------------------------------------------------------------------
sec "2. its people"
# -------------------------------------------------------------------------
JANE=$(call res.partner create "[{\"name\":\"${PFX} Jane Tan\",\"parent_id\":$ACME,
       \"email\":\"jane@acme.test\",\"job_position\":\"Finance Manager\"}]" | rid)
ALI=$(call res.partner create "[{\"name\":\"${PFX} Ali bin Osman\",\"parent_id\":$ACME,
      \"email\":\"ali@acme.test\",\"job_position\":\"Operations\"}]" | rid)
t_nonempty "$JANE" "Jane attached to Acme"
t_nonempty "$ALI"  "Ali attached to Acme"
t_eq "2" "$(pg "SELECT count(*) FROM res_partner WHERE parent_id=$ACME")" \
     "Acme has two people"

# -------------------------------------------------------------------------
sec "3. selling to the company"
# -------------------------------------------------------------------------
PROD=$(call product.product create "[{\"name\":\"${PFX} Storage Box\",\"list_price\":\"120.00\",
       \"type\":\"consu\"}]" | rid)
t_nonempty "$PROD" "a product to sell"

SO=$(call sale.order create "[{\"partner_id\":$ACME}]" | rid)
t_nonempty "$SO" "quotation raised against Acme"
if [ -n "$SO" ] && [ -n "$PROD" ]; then
    # sale.order.line requires `name` — omitting it fails the create, and the
    # order then sits at zero with no line, which is how the assertion below
    # used to pass by comparing 0 to 0.
    LINE=$(call sale.order.line create "[{\"order_id\":$SO,\"product_id\":$PROD,
           \"name\":\"${PFX} 10 x Storage Box\",
           \"product_uom_qty\":10,\"price_unit\":120}]" | rid)
    t_nonempty "$LINE" "a line for 10 boxes at 120"
    t_eq "draft" "$(pg "SELECT state FROM sale_order WHERE id=$SO")" "it starts as a draft"

    # Assert the ARITHMETIC, not just internal agreement: 10 x 120 = 1200.
    # "header equals lines" is true of an empty order too, so on its own it
    # would pass while nothing had been sold.
    HDR=$(pg "SELECT amount_total FROM sale_order WHERE id=$SO")
    LIN=$(pg "SELECT COALESCE(SUM(price_total),0) FROM sale_order_line WHERE order_id=$SO")
    t_ne "0" "${HDR:-0}"   "the order has a non-zero total (not a vacuous pass)"
    t_eq "$LIN" "$HDR"     "header total equals the sum of its lines"

    call sale.order action_confirm "[[$SO]]" >/dev/null
    t_eq "sale" "$(pg "SELECT state FROM sale_order WHERE id=$SO")" "the company's order confirms"
    t_eq "$ACME" "$(pg "SELECT partner_id FROM sale_order WHERE id=$SO")" \
         "the order is filed against the COMPANY, not a copy of its name"
fi

# -------------------------------------------------------------------------
sec "4. selling to a named person at that company"
# -------------------------------------------------------------------------
SOJ=$(call sale.order create "[{\"partner_id\":$JANE}]" | rid)
t_nonempty "$SOJ" "quotation raised against Jane"
if [ -n "$SOJ" ] && [ -n "$PROD" ]; then
    call sale.order.line create "[{\"order_id\":$SOJ,\"product_id\":$PROD,
         \"name\":\"${PFX} 2 x Storage Box\",
         \"product_uom_qty\":2,\"price_unit\":120}]" >/dev/null
    call sale.order action_confirm "[[$SOJ]]" >/dev/null
    t_eq "sale" "$(pg "SELECT state FROM sale_order WHERE id=$SOJ")" "Jane's order confirms"
fi

# -------------------------------------------------------------------------
sec "5. renting a unit to the company"
# -------------------------------------------------------------------------
# rental.unit requires code, type_id and location_id as well as a name — the
# earlier "not available in this build" was this test sending too little, not
# the module being absent.
UTYPE=$(pg "SELECT id FROM rental_unit_type ORDER BY id LIMIT 1")
ULOC=$(pg  "SELECT id FROM stock_location   ORDER BY id LIMIT 1")
UNIT=""
if [ -n "$UTYPE" ] && [ -n "$ULOC" ]; then
    UNIT=$(call rental.unit create "[{\"name\":\"${PFX} Unit A-01\",\"code\":\"${PFX}-A01\",
           \"type_id\":$UTYPE,\"location_id\":$ULOC,\"state\":\"available\"}]" | rid)
fi
if [ -z "$UNIT" ]; then
    echo "    SKIP  no rental unit type/location seeded — cannot open a tenancy"
else
    t_nonempty "$UNIT" "a rental unit exists"
    RC=$(call rental.contract create "[{\"partner_id\":$ACME,
         \"date_start\":\"$(date +%Y-%m-%d)\",\"state\":\"draft\"}]" | rid)
    t_nonempty "$RC" "tenancy opened for Acme"
    if [ -n "$RC" ]; then
        t_eq "$ACME" "$(pg "SELECT partner_id FROM rental_contract WHERE id=$RC")" \
             "the tenancy points at the company"
        RL=$(call rental.contract.line create "[{\"contract_id\":$RC,\"unit_id\":$UNIT,
             \"partner_id\":$ACME,\"date_start\":\"$(date +%Y-%m-%d)\",
             \"unit_price\":\"850.00\",\"billing_mode\":\"recurring\"}]" | rid)
        t_nonempty "$RL" "a unit is on the tenancy at RM850, billed recurring"
    fi
fi

# -------------------------------------------------------------------------
sec "6. 'show me everything for this customer'"
# -------------------------------------------------------------------------
# The question a company record exists to answer. It must cover the company AND
# the people under it — one relational query, no string matching.
ALLIDS=$(pg "SELECT string_agg(id::text, ',') FROM res_partner
              WHERE id=$ACME OR parent_id=$ACME")
t_eq "3" "$(echo "$ALLIDS" | tr ',' '\n' | grep -c .)" \
     "the customer resolves to 3 partners: Acme, Jane, Ali"

ORDERS=$(pg "SELECT count(*) FROM sale_order
              WHERE partner_id IN (SELECT id FROM res_partner
                                    WHERE id=$ACME OR parent_id=$ACME)")
t_eq "2" "${ORDERS:-0}" \
     "both orders — the company's and Jane's — come back under one customer"

REVENUE=$(pg "SELECT COALESCE(SUM(amount_total),0)::bigint FROM sale_order
               WHERE partner_id IN (SELECT id FROM res_partner
                                     WHERE id=$ACME OR parent_id=$ACME)")
t_ne "0" "${REVENUE:-0}" "the customer's total revenue is a single number"

# The failure this replaces: with free text, a differently-spelled company name
# silently splits one customer into two.
t_eq "0" "$(pg "SELECT count(*) FROM res_partner
                 WHERE parent_id=$ACME AND COALESCE(company_name,'') <> ''")" \
     "no contact relies on free-text company_name to say where it belongs"

# -------------------------------------------------------------------------
sec "7. deleting the company does not delete the trading history"
# -------------------------------------------------------------------------
# Orders and tenancies are financial records. Removing a customer must not take
# them with it, and must not orphan the people either.
call res.partner unlink "[[$ACME]]" >/dev/null 2>&1
GONE=$(pg "SELECT count(*) FROM res_partner WHERE id=$ACME")
if [ "${GONE:-0}" = "0" ]; then
    t_eq "1" "$(pg "SELECT count(*) FROM res_partner WHERE id=$JANE")" \
         "Jane survives her company being removed"
    t_eq "" "$(pg "SELECT COALESCE(parent_id::text,'') FROM res_partner WHERE id=$JANE")" \
         "her parent_id is cleared, not left pointing at a deleted row"
    t_eq "1" "$(pg "SELECT count(*) FROM sale_order WHERE id=$SOJ")" \
         "Jane's confirmed order is still there"
else
    ok "the company refused deletion while it has trading history (also acceptable)"
fi

verdict

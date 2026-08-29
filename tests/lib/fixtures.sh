#!/usr/bin/env bash
# =============================================================
# core_fixtures.sh — the canonical data the suite is built on.
#
# Sourced, never run directly. Provides fx_create, fx_drop and fx_report.
#
# WHY THIS EXISTS
# ---------------
# Seven scripts pass against the working database and fail against a clean
# baseline, because each looks up "the first product" or "the first sale order"
# and assumes one exists. On a clean database none does, so they fail for a
# reason that has nothing to do with what they test.
#
# The fix is not seven copies of a seeding block. It is one canonical set,
# created before the suite and removed after — which also makes the LIFECYCLE
# itself testable: creation is asserted, use is the suite, deletion is asserted.
#
# EVERYTHING IS PREFIXED `FX-` / `FX ` so drop is exact. Nothing here guesses at
# what to delete; it removes what it made and nothing else.
# =============================================================

FX_PREFIX='FX-'
: "${DBN:=odoo}"
fxq(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
# head -1: psql prints the RETURNING row AND the command tag, and a two-line id
# silently corrupts every statement built from it (docs/098).
fxid(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }

# ------------------------------------------------------------------
# fx_create — idempotent. Safe to run twice; returns the same ids.
# ------------------------------------------------------------------
fx_create() {
    local uom cat acc_recv acc_inc jrn_sale

    uom=$(fxq "SELECT id FROM uom_uom ORDER BY id LIMIT 1")
    cat=$(fxq "SELECT id FROM product_category ORDER BY id LIMIT 1")
    [ -z "$uom" ] && { echo "fx: no uom_uom rows — is the schema initialised?"; return 1; }
    [ -z "$cat" ] && { echo "fx: no product_category rows"; return 1; }

    # ---- partner ----
    # company_id is set explicitly. A NULL company means "shared with every
    # company", which is not what a fixture should be — and it is a leak the
    # multi-company isolation test checks for globally. It only ever passed
    # because the startup backfill happened to attribute these rows whenever
    # some earlier test restarted the server; without that restart, the
    # fixtures failed a test that has nothing to do with them.
    FX_PARTNER=$(fxq "SELECT id FROM res_partner WHERE name='FX Customer'")
    if [ -z "$FX_PARTNER" ]; then
        FX_PARTNER=$(fxid "INSERT INTO res_partner (name, active, company_id) VALUES ('FX Customer', true, 1) RETURNING id")
    fi
    [ -z "$FX_PARTNER" ] && { echo "fx: could not create partner"; return 1; }

    # ---- product (with its template) ----
    # A variant needs a template. Inserting product_product alone leaves
    # product_tmpl_id NULL, which the variant screens cannot show and which the
    # global integrity checks count as broken — product-variants asserts
    # database-wide that no product is template-less, so a fixture that skips
    # this fails a test that has nothing to do with it.
    FX_PRODUCT=$(fxq "SELECT id FROM product_product WHERE default_code='FX-PROD-1'")
    if [ -z "$FX_PRODUCT" ]; then
        FX_TMPL=$(fxq "SELECT id FROM product_template WHERE default_code='FX-PROD-1'")
        if [ -z "$FX_TMPL" ]; then
            FX_TMPL=$(fxid "INSERT INTO product_template
                (name, default_code, type, categ_id, uom_id, uom_po_id, list_price,
                 standard_price, active, sale_ok, purchase_ok, company_id)
                VALUES ('FX Reference Product','FX-PROD-1','product',$cat,$uom,$uom,
                        100000000, 60000000, true, true, true, 1) RETURNING id")
        fi
        FX_PRODUCT=$(fxid "INSERT INTO product_product
            (name, default_code, type, categ_id, uom_id, uom_po_id, list_price,
             standard_price, qty_available, active, sale_ok, purchase_ok, company_id,
             product_tmpl_id)
            VALUES ('FX Reference Product','FX-PROD-1','product',$cat,$uom,$uom,
                    100000000, 60000000, 0, true, true, true, 1, $FX_TMPL) RETURNING id")
    fi
    [ -z "$FX_PRODUCT" ] && { echo "fx: could not create product"; return 1; }

    # ---- sale order + line ----
    FX_SALE=$(fxq "SELECT id FROM sale_order WHERE name='FX-SO-1'")
    if [ -z "$FX_SALE" ]; then
        FX_SALE=$(fxid "INSERT INTO sale_order (name, partner_id, state, date_order, company_id)
                        VALUES ('FX-SO-1', $FX_PARTNER, 'draft', now(), 1) RETURNING id")
    fi
    [ -z "$FX_SALE" ] && { echo "fx: could not create sale order"; return 1; }

    FX_SALE_LINE=$(fxq "SELECT id FROM sale_order_line WHERE order_id=$FX_SALE ORDER BY id LIMIT 1")
    if [ -z "$FX_SALE_LINE" ]; then
        # product_uom_id, not product_uom. The wrong name made the INSERT fail,
        # and because fxq swallows stderr the id came back empty and the failure
        # only surfaced two assertions later as "sale line row missing".
        FX_SALE_LINE=$(fxid "INSERT INTO sale_order_line
            (order_id, product_id, name, product_uom_qty, product_uom_id, price_unit, company_id)
            VALUES ($FX_SALE, $FX_PRODUCT, 'FX reference line', 2000000, $uom, 100000000, 1)
            RETURNING id")
    fi

    # ---- invoice with a positive total and a draft out_invoice ----
    # Two of the failing scripts want an account_move with amount_total > 0, and
    # one specifically wants a DRAFT out_invoice, so both are provided.
    acc_recv=$(fxq "SELECT id FROM account_account ORDER BY id LIMIT 1")
    jrn_sale=$(fxq "SELECT id FROM account_journal WHERE code='SAL' LIMIT 1")
    [ -z "$jrn_sale" ] && jrn_sale=$(fxq "SELECT id FROM account_journal ORDER BY id LIMIT 1")

    FX_INVOICE=$(fxq "SELECT id FROM account_move WHERE name='FX-INV-1'")
    if [ -z "$FX_INVOICE" ] && [ -n "$jrn_sale" ]; then
        FX_INVOICE=$(fxid "INSERT INTO account_move
            (name, move_type, state, date, journal_id, company_id, partner_id,
             amount_total, amount_residual, payment_state)
            VALUES ('FX-INV-1','out_invoice','draft', CURRENT_DATE, $jrn_sale, 1, $FX_PARTNER,
                    100000000, 100000000, 'not_paid') RETURNING id")
        if [ -n "$FX_INVOICE" ] && [ -n "$acc_recv" ]; then
            fxq "INSERT INTO account_move_line (move_id, account_id, company_id, name, debit, credit)
                 VALUES ($FX_INVOICE, $acc_recv, 1, 'FX receivable', 100000000, 0)" >/dev/null
        fi
    fi

    export FX_PARTNER FX_PRODUCT FX_SALE FX_SALE_LINE FX_INVOICE
    return 0
}

# ------------------------------------------------------------------
# fx_drop — removes exactly what fx_create made, children first.
# ------------------------------------------------------------------
fx_drop() {
    fxq "DELETE FROM account_move_line WHERE move_id IN (SELECT id FROM account_move WHERE name LIKE 'FX-%')" >/dev/null
    fxq "DELETE FROM account_move      WHERE name LIKE 'FX-%'" >/dev/null
    fxq "DELETE FROM sale_order_line   WHERE order_id IN (SELECT id FROM sale_order WHERE name LIKE 'FX-%')" >/dev/null
    fxq "DELETE FROM sale_order        WHERE name LIKE 'FX-%'" >/dev/null
    fxq "DELETE FROM part_parameter          WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'FX-%')" >/dev/null
    fxq "DELETE FROM part_manufacturer_info  WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'FX-%')" >/dev/null
    fxq "DELETE FROM product_product   WHERE default_code LIKE 'FX-%'" >/dev/null
    fxq "DELETE FROM product_template  WHERE default_code LIKE 'FX-%'" >/dev/null
    fxq "DELETE FROM res_partner       WHERE name LIKE 'FX %'" >/dev/null
    return 0
}

# ------------------------------------------------------------------
# fx_report — how much of the set is currently present. Used by both the
# create and the delete checks so they measure the same thing.
# ------------------------------------------------------------------
fx_report() {
    echo "partner=$(fxq "SELECT count(*) FROM res_partner WHERE name LIKE 'FX %'")"
    echo "product=$(fxq "SELECT count(*) FROM product_product WHERE default_code LIKE 'FX-%'")"
    echo "sale=$(fxq "SELECT count(*) FROM sale_order WHERE name LIKE 'FX-%'")"
    echo "saleline=$(fxq "SELECT count(*) FROM sale_order_line WHERE order_id IN (SELECT id FROM sale_order WHERE name LIKE 'FX-%')")"
    echo "invoice=$(fxq "SELECT count(*) FROM account_move WHERE name LIKE 'FX-%'")"
    echo "invoiceline=$(fxq "SELECT count(*) FROM account_move_line WHERE move_id IN (SELECT id FROM account_move WHERE name LIKE 'FX-%')")"
}

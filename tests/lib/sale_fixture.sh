#!/bin/bash
# =============================================================
# tests/lib/sale_fixture.sh — ensure_sale_fixture, one idempotent sale order.
#
# Sourced by the handful of tests that assume "at least one sale order with a
# line exists" — they do `SELECT id FROM sale_order LIMIT 1` and drive the
# report / recompute / tax paths off it. A freshly-provisioned database has zero
# sale orders, so those probes had nothing to bite on and failed for lack of
# data rather than a real defect (docs/070). This guarantees the precondition,
# through the real API so totals/scaling are correct.
#
#   source tests/lib/sale_fixture.sh; ensure_sale_fixture "$SID"
#
# Safe to run repeatedly: it creates nothing if a sale line already exists.
#
# NOT the same thing as tests/lib/fixtures.sh. That one owns the canonical `FX-`
# set, created by tests/setup and dropped by tests/teardown, and is what
# `needs=fixtures` in a meta refers to. This is a narrower, self-contained
# precondition a test asks for inline.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

_fx_pg()   { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
_fx_call() { # model method args sid
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":{\"context\":{\"session_id\":\"$4\"}}}}"
}

ensure_sale_fixture() {
    local have partner product order i r
    have=$(_fx_pg "SELECT count(*) FROM sale_order_line")
    if [ "${have:-0}" -ge 1 ]; then
        return 0
    fi

    # Authenticate (standalone) unless a SID was passed in.
    local sid="${1:-}"
    if [ -z "$sid" ]; then
        sid=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
              --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
              | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
    fi
    [ -z "$sid" ] && { echo "seed: cannot authenticate" >&2; return 1; }

    partner=$(_fx_pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
    product=$(_fx_pg "SELECT id FROM product_product ORDER BY id LIMIT 1")
    [ -z "$partner" ] || [ -z "$product" ] && { echo "seed: need a partner and a product first" >&2; return 1; }

    # Two orders so probes that want a couple of records are happy. The line is
    # deliberately UNTAXED: verify_money_recompute rewrites the first line and
    # asserts subtotal == total (no tax). verify_tax_engine adds its own taxed
    # lines at runtime, so it does not depend on the fixture carrying a tax.
    for i in 1 2; do
        r=$(_fx_call sale.order create "[{\"partner_id\":$partner}]" "$sid")
        order=$(printf '%s' "$r" | sed -n 's/.*"result":\([0-9]*\).*/\1/p')
        if [ -z "$order" ]; then
            echo "seed: sale.order create failed: $(printf '%s' "$r" | head -c 160)" >&2
            return 1
        fi
        _fx_call sale.order.line create \
          "[{\"order_id\":$order,\"product_id\":$product,\"name\":\"Fixture line\",\"product_uom_qty\":3,\"price_unit\":100}]" \
          "$sid" >/dev/null
    done
    echo "seed: created 2 sale orders with untaxed lines (partner=$partner product=$product)"
}

# When executed directly (not sourced), run the seeder.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    ensure_sale_fixture "$@"
fi

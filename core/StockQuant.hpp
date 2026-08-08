#pragma once
#include <string>
// =============================================================
// core/StockQuant.hpp
//
// The quant engine — the single authority on on-hand quantity and
// reservation. Every code path that moves physical stock (stock.picking
// validation, manufacturing consume/produce, subcontracting backflush)
// goes through here, so on-hand is computed one way, in one place.
//
// All quantities are int64 MICRO-units (scale 6), matching stock_move
// (markScaled, migration 940). Callers pass and receive micros.
//
// On-hand is the sum of stock_quant.quantity across INTERNAL locations
// only; virtual locations (supplier/customer/production/inventory) hold
// quants too but are not counted as owned stock — they are the other end
// of every real move and go negative naturally.
//
// Policy: allow-negative (Odoo default). applyMove never refuses; a move
// that drives a location below zero simply records a negative quant, to be
// reconciled by a later inventory adjustment. reserve() still only reserves
// what is physically available.
// =============================================================

// pqxx::work is a TYPE ALIAS for transaction<...>, so it cannot be
// forward-declared as a class. transaction_base is its base — declaring that
// keeps <pqxx/pqxx> out of this header (PERF-E) while still accepting any
// pqxx::work by reference.
namespace pqxx { class transaction_base; }

namespace odoo::core {

class StockQuant {
public:
    // Move `qtyMicros` of `productId` from `srcLocId` to `destLocId` (both
    // stock.location ids). Decrements the source quant, increments the
    // destination quant, refreshes product_product.qty_available, and — when the
    // move crosses the owned-stock boundary (internal ↔ virtual) — records a
    // stock_valuation_layer and (Phase B) posts its journal entry. Runs inside
    // the caller's transaction.
    //
    // inputCostMicros: the unit cost to value an INCOMING move at (a receipt's
    // purchase price, a finished good's build cost). -1 = use the product's
    // standard_price. Ignored for outgoing moves, which are valued by the
    // product's cost method.
    // lotId 0 = untracked; a positive id keys the quant to a specific
    // stock.production.lot, so on-hand is tracked per (product, location, lot).
    static void applyMove(pqxx::transaction_base& txn, int productId,
                          int srcLocId, int destLocId,
                          long long qtyMicros, int companyId,
                          long long inputCostMicros = -1, int lotId = 0);

    // Reserve up to `qtyMicros` of available stock at `locId` (for `lotId`).
    // Never reserves more than is physically available (quantity -
    // reserved_quantity, floored at 0). Returns the amount actually reserved.
    static long long reserve(pqxx::transaction_base& txn, int productId, int locId,
                             long long qtyMicros, int companyId, int lotId = 0);

    // Release a previously-held reservation at `locId`/`lotId` (floored at 0).
    static void release(pqxx::transaction_base& txn, int productId, int locId,
                        long long qtyMicros, int lotId = 0);

    // Pure value adjustment (no quantity change) — a landed cost / revaluation.
    // Adds `valueMicros` to the product's inventory value: writes a zero-quantity
    // stock_valuation_layer and updates value_svl (and, for average, standard_price;
    // for FIFO, the remaining cost layers proportionally to their remaining_qty).
    // Returns the new layer's id (0 = no-op) so the caller can link its GL entry.
    // Does not post GL itself.
    static int revalue(pqxx::transaction_base& txn, int productId,
                       long long valueMicros, const std::string& description,
                       int companyId);

    // On-hand across all INTERNAL locations (all lots) — what qty_available reflects.
    static long long onHandInternal(pqxx::transaction_base& txn, int productId);

    // available = quantity - reserved_quantity at a single location/lot.
    static long long availableAt(pqxx::transaction_base& txn, int productId, int locId,
                                 int lotId = 0);
};

} // namespace odoo::core

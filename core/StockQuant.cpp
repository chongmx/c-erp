// =============================================================
// core/StockQuant.cpp
// =============================================================
#include "StockQuant.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace odoo::core {

// Add `deltaMicros` (may be negative) to the quant at (product, location, lot),
// creating the row if absent. company_id 0 is stored as NULL (no FK target).
static void upsertDelta(pqxx::transaction_base& txn, int productId, int locId,
                        long long deltaMicros, int companyId, int lotId) {
    txn.exec(
        "INSERT INTO stock_quant (product_id, location_id, lot_id, quantity, company_id) "
        "VALUES ($1, $2, $3, $4, NULLIF($5, 0)) "
        "ON CONFLICT (product_id, location_id, lot_id) DO UPDATE "
        "SET quantity = stock_quant.quantity + EXCLUDED.quantity, "
        "    write_date = now()",
        pqxx::params{productId, locId, lotId, deltaMicros, companyId});
}

// Recompute the denormalised on-hand cached on the product, so the product
// list/form and downstream (min-stock, MPS) can read qty_available without a
// join. Kept transactionally consistent with the quant rows it summarises.
static void refreshProductOnHand(pqxx::transaction_base& txn, int productId) {
    txn.exec(
        "UPDATE product_product SET qty_available = COALESCE(("
        "  SELECT SUM(q.quantity) FROM stock_quant q "
        "  JOIN stock_location l ON l.id = q.location_id "
        "  WHERE q.product_id = $1 AND l.usage = 'internal'), 0) "
        "WHERE id = $1",
        pqxx::params{productId});
}

// micros × micros ÷ 1e6 = micros, via 128-bit intermediate (no overflow).
static long long mulMicros(long long qtyMic, long long costMic) {
    return static_cast<long long>((static_cast<__int128>(qtyMic) * costMic) / 1000000);
}
static long long divMicros(long long valueMic, long long qtyMic) {
    if (qtyMic == 0) return 0;
    return static_cast<long long>((static_cast<__int128>(valueMic) * 1000000) / qtyMic);
}
static std::string usageOf(pqxx::transaction_base& txn, int locId) {
    auto r = txn.exec("SELECT usage FROM stock_location WHERE id=$1", pqxx::params{locId});
    return r.empty() || r[0][0].is_null() ? std::string() : r[0][0].c_str();
}

// Real-time GL posting for one valuation layer. Balanced 2-line entry in the
// stock journal: the Stock Valuation account on one side, and a counterpart
// account chosen by the virtual location's usage on the other —
//   supplier   -> Stock Input (interim)     customer   -> COGS
//   production -> Production (WIP)           inventory  -> Inventory Adjustment
//   subcontract-> Production (WIP)
// Accounts come from the product category (property_* overrides) or fall back
// to seeded defaults by code. If the stock journal or an account is missing
// (GL not configured), it silently skips — valuation still tracks without GL.
static void postValuationEntry_(pqxx::transaction_base& txn, int layerId, int productId,
                                const std::string& counterpartUsage, long long valueMicros,
                                bool incoming, int companyId) {
    if (valueMicros <= 0) return;
    const int comp = companyId > 0 ? companyId : 1;

    auto acctByCode = [&](const char* code) -> int {
        auto r = txn.exec("SELECT id FROM account_account WHERE code=$1 AND company_id=$2 LIMIT 1",
                          pqxx::params{std::string(code), comp});
        return r.empty() ? 0 : r[0][0].as<int>();
    };
    // category overrides
    int catId = 0;
    { auto r = txn.exec("SELECT categ_id FROM product_product WHERE id=$1", pqxx::params{productId});
      if (!r.empty() && !r[0][0].is_null()) catId = r[0][0].as<int>(); }
    auto catAcct = [&](const std::string& col) -> int {
        if (catId <= 0) return 0;
        auto r = txn.exec("SELECT " + col + " FROM product_category WHERE id=$1", pqxx::params{catId});
        return (r.empty() || r[0][0].is_null()) ? 0 : r[0][0].as<int>();
    };

    int journalId = catAcct("property_stock_journal_id");
    if (journalId <= 0) {
        auto r = txn.exec("SELECT id FROM account_journal WHERE code='STJ' AND company_id=$1 LIMIT 1",
                          pqxx::params{comp});
        journalId = r.empty() ? 0 : r[0][0].as<int>();
    }
    int valAcct = catAcct("property_stock_valuation_account_id");
    if (valAcct <= 0) valAcct = acctByCode("1400");

    int cpAcct = 0;
    if (counterpartUsage == "supplier") {
        cpAcct = catAcct("property_stock_account_input_id");
        if (cpAcct <= 0) cpAcct = acctByCode("1410");
    } else if (counterpartUsage == "customer") {
        cpAcct = catAcct("property_stock_account_output_id");
        if (cpAcct <= 0) cpAcct = acctByCode("5000");
    } else if (counterpartUsage == "production" || counterpartUsage == "subcontract") {
        cpAcct = acctByCode("1430");
    } else if (counterpartUsage == "inventory") {
        cpAcct = acctByCode("5100");
    }

    if (journalId <= 0 || valAcct <= 0 || cpAcct <= 0) return;   // not configured → skip

    const int moveId = txn.exec(
        "INSERT INTO account_move (name, move_type, state, date, journal_id, company_id) "
        "VALUES ('/','entry','posted',CURRENT_DATE,$1,$2) RETURNING id",
        pqxx::params{journalId, comp})[0][0].as<int>();
    txn.exec("UPDATE account_move SET name=$2 WHERE id=$1",
             pqxx::params{moveId, std::string("STJ/") + std::to_string(moveId)});

    // Incoming: Dr Valuation / Cr Counterpart.  Outgoing: Cr Valuation / Dr Counterpart.
    const long long drVal = incoming ? valueMicros : 0;
    const long long crVal = incoming ? 0 : valueMicros;
    const long long drCp  = incoming ? 0 : valueMicros;
    const long long crCp  = incoming ? valueMicros : 0;
    auto line = [&](int acct, long long debit, long long credit) {
        txn.exec(
            "INSERT INTO account_move_line "
            "(move_id, account_id, journal_id, company_id, date, name, debit, credit) "
            "VALUES ($1,$2,$3,$4,CURRENT_DATE,'Inventory valuation',$5,$6)",
            pqxx::params{moveId, acct, journalId, comp, debit, credit});
    };
    line(valAcct, drVal, crVal);
    line(cpAcct,  drCp,  crCp);

    txn.exec("UPDATE stock_valuation_layer SET account_move_id=$2 WHERE id=$1",
             pqxx::params{layerId, moveId});
}

// The valuation layer. Fires only when a move crosses the owned-stock boundary
// (an internal location on exactly one side). Incoming adds value at the input
// (or standard) cost; outgoing removes value by the product's cost method.
// Maintains the product's quantity_svl / value_svl caches and, for AVCO, its
// standard_price, then posts the layer's journal entry.
static void valuateMove_(pqxx::transaction_base& txn, int productId,
                         int srcLocId, int destLocId, long long qtyMicros,
                         int companyId, long long inputCostMicros) {
    if (qtyMicros <= 0) return;
    const std::string srcUsage = usageOf(txn, srcLocId);
    const std::string dstUsage = usageOf(txn, destLocId);
    const bool srcInt = (srcUsage == "internal");
    const bool dstInt = (dstUsage == "internal");
    if (srcInt == dstInt) return;   // internal transfer or virtual↔virtual: no value change

    auto pr = txn.exec(
        "SELECT cost_method, standard_price, quantity_svl, value_svl "
        "FROM product_product WHERE id=$1", pqxx::params{productId});
    if (pr.empty()) return;
    const std::string method  = pr[0]["cost_method"].is_null() ? "standard" : pr[0]["cost_method"].c_str();
    const long long    stdCost = pr[0]["standard_price"].as<long long>(0);
    const long long    qSvl    = pr[0]["quantity_svl"].as<long long>(0);
    const long long    vSvl    = pr[0]["value_svl"].as<long long>(0);

    if (!srcInt && dstInt) {
        // ---- INCOMING ----
        const long long unitCost   = inputCostMicros >= 0 ? inputCostMicros : stdCost;
        const long long layerValue = mulMicros(qtyMicros, unitCost);
        const long long newQ = qSvl + qtyMicros;
        const long long newV = vSvl + layerValue;
        const long long remQ = (method == "fifo") ? qtyMicros  : 0;
        const long long remV = (method == "fifo") ? layerValue : 0;
        const int layerId = txn.exec(
            "INSERT INTO stock_valuation_layer "
            "(product_id, quantity, unit_cost, value, remaining_qty, remaining_value, "
            " counterpart_usage, description, company_id) "
            "VALUES ($1,$2,$3,$4,$5,$6,$7,'in',NULLIF($8,0)) RETURNING id",
            pqxx::params{productId, qtyMicros, unitCost, layerValue, remQ, remV, srcUsage, companyId})[0][0].as<int>();
        if (method == "average" && newQ > 0) {
            const long long newStd = divMicros(newV, newQ);
            txn.exec("UPDATE product_product SET quantity_svl=$2, value_svl=$3, standard_price=$4 WHERE id=$1",
                     pqxx::params{productId, newQ, newV, newStd});
        } else {
            txn.exec("UPDATE product_product SET quantity_svl=$2, value_svl=$3 WHERE id=$1",
                     pqxx::params{productId, newQ, newV});
        }
        postValuationEntry_(txn, layerId, productId, srcUsage, layerValue, /*incoming=*/true, companyId);
    } else {
        // ---- OUTGOING ----
        long long outValue = 0;
        if (method == "fifo") {
            long long toConsume = qtyMicros;
            auto layers = txn.exec(
                "SELECT id, remaining_qty, remaining_value FROM stock_valuation_layer "
                "WHERE product_id=$1 AND remaining_qty > 0 ORDER BY id ASC", pqxx::params{productId});
            for (const auto& L : layers) {
                if (toConsume <= 0) break;
                const int       lid = L["id"].as<int>();
                const long long lq  = L["remaining_qty"].as<long long>(0);
                const long long lv  = L["remaining_value"].as<long long>(0);
                const long long take    = std::min(lq, toConsume);
                const long long portion = (lq > 0) ? static_cast<long long>((static_cast<__int128>(lv) * take) / lq) : 0;
                outValue += portion;
                txn.exec("UPDATE stock_valuation_layer "
                         "SET remaining_qty = remaining_qty - $2, remaining_value = remaining_value - $3 WHERE id=$1",
                         pqxx::params{lid, take, portion});
                toConsume -= take;
            }
            if (toConsume > 0) outValue += mulMicros(toConsume, stdCost);  // shortfall at standard
        } else {
            outValue = mulMicros(qtyMicros, stdCost);   // standard / average: current unit cost
        }
        const long long unitCost = divMicros(outValue, qtyMicros);
        const long long newQ = qSvl - qtyMicros;
        const long long newV = vSvl - outValue;
        const int layerId = txn.exec(
            "INSERT INTO stock_valuation_layer "
            "(product_id, quantity, unit_cost, value, remaining_qty, remaining_value, "
            " counterpart_usage, description, company_id) "
            "VALUES ($1,$2,$3,$4,0,0,$5,'out',NULLIF($6,0)) RETURNING id",
            pqxx::params{productId, -qtyMicros, unitCost, -outValue, dstUsage, companyId})[0][0].as<int>();
        txn.exec("UPDATE product_product SET quantity_svl=$2, value_svl=$3 WHERE id=$1",
                 pqxx::params{productId, newQ, newV});
        postValuationEntry_(txn, layerId, productId, dstUsage, outValue, /*incoming=*/false, companyId);
    }
}

void StockQuant::applyMove(pqxx::transaction_base& txn, int productId,
                           int srcLocId, int destLocId,
                           long long qtyMicros, int companyId,
                           long long inputCostMicros, int lotId) {
    if (qtyMicros == 0 || srcLocId == destLocId) return;
    upsertDelta(txn, productId, srcLocId,  -qtyMicros, companyId, lotId);
    upsertDelta(txn, productId, destLocId,  qtyMicros, companyId, lotId);
    refreshProductOnHand(txn, productId);
    valuateMove_(txn, productId, srcLocId, destLocId, qtyMicros, companyId, inputCostMicros);
}

long long StockQuant::availableAt(pqxx::transaction_base& txn, int productId, int locId,
                                  int lotId) {
    auto r = txn.exec(
        "SELECT quantity - reserved_quantity FROM stock_quant "
        "WHERE product_id = $1 AND location_id = $2 AND lot_id = $3",
        pqxx::params{productId, locId, lotId});
    if (r.empty() || r[0][0].is_null()) return 0;
    return r[0][0].as<long long>(0);
}

long long StockQuant::reserve(pqxx::transaction_base& txn, int productId, int locId,
                              long long qtyMicros, int companyId, int lotId) {
    if (qtyMicros <= 0) return 0;
    const long long avail = availableAt(txn, productId, locId, lotId);
    long long toReserve = qtyMicros < avail ? qtyMicros : avail;
    if (toReserve <= 0) return 0;
    // A row must already exist for there to be anything available.
    txn.exec(
        "UPDATE stock_quant "
        "SET reserved_quantity = reserved_quantity + $4, write_date = now() "
        "WHERE product_id = $1 AND location_id = $2 AND lot_id = $3",
        pqxx::params{productId, locId, lotId, toReserve});
    (void)companyId;
    return toReserve;
}

void StockQuant::release(pqxx::transaction_base& txn, int productId, int locId,
                         long long qtyMicros, int lotId) {
    if (qtyMicros <= 0) return;
    txn.exec(
        "UPDATE stock_quant "
        "SET reserved_quantity = GREATEST(reserved_quantity - $4, 0), "
        "    write_date = now() "
        "WHERE product_id = $1 AND location_id = $2 AND lot_id = $3",
        pqxx::params{productId, locId, lotId, qtyMicros});
}

int StockQuant::revalue(pqxx::transaction_base& txn, int productId,
                        long long valueMicros, const std::string& description,
                        int companyId) {
    if (valueMicros == 0) return 0;
    auto pr = txn.exec("SELECT cost_method, quantity_svl, value_svl FROM product_product WHERE id=$1",
                       pqxx::params{productId});
    if (pr.empty()) return 0;
    const std::string method = pr[0]["cost_method"].is_null() ? "standard" : pr[0]["cost_method"].c_str();
    const long long   qSvl   = pr[0]["quantity_svl"].as<long long>(0);
    const long long   vSvl   = pr[0]["value_svl"].as<long long>(0);
    const long long   newV   = vSvl + valueMicros;

    const int layerId = txn.exec(
        "INSERT INTO stock_valuation_layer "
        "(product_id, quantity, unit_cost, value, remaining_qty, remaining_value, "
        " counterpart_usage, description, company_id) "
        "VALUES ($1,0,0,$2,0,0,'revaluation',$3,NULLIF($4,0)) RETURNING id",
        pqxx::params{productId, valueMicros, description, companyId})[0][0].as<int>();

    if (method == "average" && qSvl > 0) {
        txn.exec("UPDATE product_product SET value_svl=$2, standard_price=$3 WHERE id=$1",
                 pqxx::params{productId, newV, divMicros(newV, qSvl)});
    } else {
        txn.exec("UPDATE product_product SET value_svl=$2 WHERE id=$1",
                 pqxx::params{productId, newV});
    }

    // FIFO: push the added value onto the remaining cost layers so it flows out
    // with the units it belongs to.
    if (method == "fifo") {
        auto tot = txn.exec("SELECT COALESCE(SUM(remaining_qty),0) FROM stock_valuation_layer "
                            "WHERE product_id=$1 AND remaining_qty>0", pqxx::params{productId});
        const long long totRem = tot.empty() ? 0 : tot[0][0].as<long long>(0);
        if (totRem > 0) {
            auto layers = txn.exec("SELECT id, remaining_qty FROM stock_valuation_layer "
                                   "WHERE product_id=$1 AND remaining_qty>0 ORDER BY id",
                                   pqxx::params{productId});
            std::vector<std::pair<int, long long>> adds;
            long long distributed = 0;
            for (const auto& L : layers) {
                const int       lid = L["id"].as<int>();
                const long long rq  = L["remaining_qty"].as<long long>(0);
                const long long share = static_cast<long long>((static_cast<__int128>(valueMicros) * rq) / totRem);
                adds.emplace_back(lid, share);
                distributed += share;
            }
            if (!adds.empty()) adds.back().second += (valueMicros - distributed);  // rounding remainder
            for (const auto& [lid, share] : adds)
                txn.exec("UPDATE stock_valuation_layer SET remaining_value = remaining_value + $2 WHERE id=$1",
                         pqxx::params{lid, share});
        }
    }
    return layerId;
}

long long StockQuant::onHandInternal(pqxx::transaction_base& txn, int productId) {
    auto r = txn.exec(
        "SELECT COALESCE(SUM(q.quantity), 0) FROM stock_quant q "
        "JOIN stock_location l ON l.id = q.location_id "
        "WHERE q.product_id = $1 AND l.usage = 'internal'",
        pqxx::params{productId});
    return r.empty() ? 0 : r[0][0].as<long long>(0);
}

} // namespace odoo::core

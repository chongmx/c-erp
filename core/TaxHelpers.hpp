#pragma once
// ============================================================
// core/TaxHelpers.hpp
//
// Glue between the DB and TaxEngine. (P3)
//
// Sale and purchase both need "read tax ids from a JSON column, look them
// up, compute, write the three amount fields back". Written once here so
// the two modules cannot drift — which is how the original inline version
// ended up handling price_include in neither of them.
// ============================================================
#include "Money.hpp"
#include "TaxEngine.hpp"
#include "DecimalPrecision.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <string>
#include <vector>

namespace cerp::core {

/**
 * @brief Load tax definitions for a JSON id array, in sequence order.
 *
 * Unknown or inactive ids are skipped rather than throwing: a tax removed
 * after a line referenced it should not make the line uncomputable.
 */
inline std::vector<TaxDef> loadTaxes(pqxx::transaction_base& txn,
                                     const std::string& taxIdsJson) {
    std::vector<TaxDef> out;
    if (taxIdsJson.empty()) return out;

    nlohmann::json ids;
    try { ids = nlohmann::json::parse(taxIdsJson); }
    catch (...) { return out; }              // malformed column — no taxes
    if (!ids.is_array() || ids.empty()) return out;

    for (const auto& tid : ids) {
        if (!tid.is_number_integer()) continue;
        auto r = txn.exec(
            "SELECT id, name, amount, amount_type, price_include "
            "  FROM account_tax WHERE id = $1 AND active",
            pqxx::params{tid.get<int>()});
        if (r.empty()) continue;

        TaxDef t;
        t.id           = r[0]["id"].as<int>();
        t.name         = r[0]["name"].c_str();
        // account_tax.amount is a RATE (8.0 = 8%), deliberately NUMERIC and
        // not micro-scaled (docs/049 §3) — so it is parsed from its decimal
        // text rather than read as micro-units.
        t.rate         = Money::parse(r[0]["amount"].c_str());
        t.amountType   = r[0]["amount_type"].c_str();
        t.priceInclude = r[0]["price_include"].as<bool>(false);
        t.sequence     = t.id;               // no sequence column yet; id is stable
        out.push_back(std::move(t));
    }
    return out;
}

/**
 * @brief Compute a line's amounts and write them back into `vals`.
 *
 * `vals` carries MAJOR units (it is the JSON the client sent), so values are
 * converted in and out via Money::fromJson / toJson — BaseModel's write
 * boundary then rescales to micro-units.
 *
 * @param linePrecision decimals to round to; defaults to the `Account`
 *                      setting when 0 (docs/048 §2.3).
 */
inline void applyLineTaxes(pqxx::transaction_base& txn,
                           nlohmann::json& vals,
                           const std::string& qtyField,
                           int linePrecision = 0) {
    const int dp = linePrecision > 0
                 ? linePrecision
                 : (DecimalPrecision::ready()
                        ? DecimalPrecision::instance().digits(DecimalPrecision::kAccount, 2)
                        : 2);

    const Money qty  = Money::fromJson(vals.value(qtyField,      1.0));
    const Money unit = Money::fromJson(vals.value("price_unit",  0.0));
    const Money disc = Money::fromJson(vals.value("discount",    0.0));

    const auto taxes = loadTaxes(txn, vals.value("tax_ids_json", std::string("[]")));
    const auto res   = TaxEngine::compute(unit, qty, disc, taxes, dp);

    vals["price_subtotal"] = res.baseExcluded.toJson();
    vals["price_tax"]      = res.totalTax.toJson();
    vals["price_total"]    = res.totalIncluded.toJson();
}

} // namespace cerp::core

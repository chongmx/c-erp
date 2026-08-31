#pragma once
// ============================================================
// core/TaxEngine.hpp
//
// Tax computation. (P3 — docs/045, docs/048 §3 and §5)
//
// WHAT EXISTED BEFORE
//   A fragment inside SaleModule that handled exactly one case:
//       if (amount_type == "percent" && !price_include)
//           taxAmt += subtotal * rate / 100.0;
//   So a PRICE-INCLUDED tax contributed zero — the whole gross amount
//   landed in the subtotal and the invoice reported no tax at all. And it
//   worked in `double` with ad-hoc rounding, which P2 replaced everywhere
//   else.
//
// THE TWO RULES THAT MAKE THIS CORRECT
//
//   1. INCLUSIVE TAX IS DERIVED BY SUBTRACTION, NEVER BY MULTIPLICATION.
//      Given a gross of 108.00 that includes 8% tax:
//          base = round(108.00 / 1.08) = 100.00
//          tax  = 108.00 - 100.00      = 8.00     <- subtraction
//      Computing the tax as base x 8% instead can produce 8.01 for some
//      values, and then base + tax no longer equals the price the customer
//      was quoted. Deriving by subtraction makes that impossible.
//
//   2. ROUND PER LINE, THEN SUM (docs/048 §3, option A).
//      Each line's tax is rounded to the document's precision before the
//      totals are summed, so the printed column always foots to the printed
//      total. The alternative — sum exact, round once — is "more accurate"
//      but produces invoices whose lines do not add up, which is worse.
//
// ORDER OF OPERATIONS (fixed, docs/048 §5)
//      compute per line -> round to document precision -> sum
//      -> round to currency precision -> convert to base currency
//   Converting first and taxing second gives different, wrong answers.
//
// WHAT IS DELIBERATELY NOT HERE
//   Tax-on-tax (compound), fiscal positions, and per-country rules. The
//   `sequence` field is carried so compound tax can be added later without
//   a schema change, but nothing computes it — pretending otherwise would
//   be worse than not having it.
// ============================================================
#include "Money.hpp"

#include <string>
#include <vector>

namespace cerp::core {

/// One tax as configured in account_tax.
struct TaxDef {
    int         id           = 0;
    std::string name;
    Money       rate;                    ///< 8% arrives as 8.000000
    std::string amountType   = "percent";///< "percent" | "fixed"
    bool        priceInclude = false;
    int         sequence     = 0;
};

/// The tax produced by one tax on one line.
struct TaxComponent {
    int         taxId = 0;
    std::string name;
    Money       base;      ///< taxable base this tax applied to
    Money       amount;    ///< tax charged
};

/// Result for a single document line.
struct TaxComputation {
    Money baseExcluded;                  ///< net — goes to price_subtotal
    Money totalTax;                      ///< sum of components
    Money totalIncluded;                 ///< base + tax — price_total
    std::vector<TaxComponent> components;///< one per tax, for tax lines
};

class TaxEngine {
public:
    /**
     * @brief Compute tax for one line.
     *
     * @param priceUnit   unit price (may be tax-inclusive — see TaxDef)
     * @param quantity    quantity
     * @param discountPct percentage discount, e.g. 10.0 for 10%
     * @param taxes       taxes to apply, in `sequence` order
     * @param dp          decimal places to round each amount to
     *                    (document line precision — docs/048 §2.3)
     *
     * Guarantees, asserted by tests/test_tax.cpp:
     *   baseExcluded + totalTax == totalIncluded, exactly;
     *   for inclusive taxes, totalIncluded == the gross the caller passed in.
     */
    static TaxComputation compute(const Money& priceUnit,
                                  const Money& quantity,
                                  const Money& discountPct,
                                  const std::vector<TaxDef>& taxes,
                                  int dp);
};

} // namespace cerp::core

// ============================================================
// core/TaxEngine.cpp — see TaxEngine.hpp
// ============================================================
#include "TaxEngine.hpp"

#include <algorithm>

namespace cerp::core {

TaxComputation TaxEngine::compute(const Money& priceUnit,
                                  const Money& quantity,
                                  const Money& discountPct,
                                  const std::vector<TaxDef>& taxesIn,
                                  int dp) {
    TaxComputation out;

    // The line's currency. Every Money this function builds must carry it,
    // or Money's own currency guard rejects the arithmetic — which is exactly
    // what happened before this was threaded through: a USD line threw
    // "cannot add amounts in different currencies (0 vs 2)" and no USD invoice
    // could be computed at all.
    //
    // Rates, quantities and percentages are dimensionless and stay at
    // currency 0; only money-valued results are tagged.
    const int ccy = priceUnit.currencyId();
    auto M = [ccy](__int128 micros) {
        return Money::fromMicros(static_cast<int64_t>(micros), ccy);
    };
    out.baseExcluded  = Money::zero(ccy);
    out.totalTax      = Money::zero(ccy);
    out.totalIncluded = Money::zero(ccy);

    // ── gross line amount, before any tax reasoning ───────────
    Money gross = priceUnit.mulQty(quantity);
    if (!discountPct.isZero())
        gross = gross - gross.percent(discountPct);
    gross = gross.roundTo(dp);

    if (taxesIn.empty()) {
        out.baseExcluded  = gross;
        out.totalTax      = Money::zero(ccy);
        out.totalIncluded = gross;
        return out;
    }

    // Apply in configured order so a future compound implementation and the
    // current one agree on which tax is "first".
    std::vector<TaxDef> taxes = taxesIn;
    std::stable_sort(taxes.begin(), taxes.end(),
                     [](const TaxDef& a, const TaxDef& b) { return a.sequence < b.sequence; });

    // ── separate inclusive from exclusive ─────────────────────
    std::vector<const TaxDef*> included, excluded;
    Money includedRateSum = Money::zero();          // dimensionless
    Money includedFixed   = Money::zero(ccy);       // money

    for (const auto& t : taxes) {
        if (t.priceInclude) {
            included.push_back(&t);
            if (t.amountType == "percent") includedRateSum += t.rate;
            else                           includedFixed   += M(t.rate.mulQty(quantity).micros());
        } else {
            excluded.push_back(&t);
        }
    }

    // ── back out inclusive tax ────────────────────────────────
    Money base = gross;
    Money includedTaxTotal = Money::zero(ccy);

    if (!included.empty()) {
        // A fixed inclusive amount is simply part of the gross: remove it
        // before the percentage is unwound, or the division is applied to
        // money the percentage never taxed.
        Money grossForPct = gross - includedFixed;

        if (!includedRateSum.isZero()) {
            // base = grossForPct * 100 / (100 + rateSum)
            const Money hundred = Money::parse("100");
            const Money divisor = hundred + includedRateSum;
            // (grossForPct * 100) / divisor, kept in micro-units throughout
            const Money numerator = grossForPct.mulQty(hundred);
            base = M((static_cast<__int128>(numerator.micros()) * Money::ONE)
                     / divisor.micros()).roundTo(dp);
        } else {
            base = grossForPct;
        }

        // RULE 1: derive by subtraction. This is what guarantees
        // base + tax == the gross the customer was quoted, for every value.
        includedTaxTotal = gross - base;
    }

    out.baseExcluded = base;

    // ── distribute the inclusive tax across its taxes ─────────
    // Proportional to rate, with the LAST tax absorbing the remainder so the
    // components sum to includedTaxTotal exactly rather than to within a
    // rounding step.
    if (!included.empty()) {
        Money allocated = Money::zero(ccy);
        for (std::size_t i = 0; i < included.size(); ++i) {
            const TaxDef& t = *included[i];
            Money share = Money::zero(ccy);
            if (i + 1 == included.size()) {
                share = includedTaxTotal - allocated;      // remainder
            } else if (t.amountType == "fixed") {
                share = M(t.rate.mulQty(quantity).micros()).roundTo(dp);
            } else if (!includedRateSum.isZero()) {
                // includedTaxTotal * (rate / rateSum)
                share = M((static_cast<__int128>(includedTaxTotal.micros()) * t.rate.micros())
                          / includedRateSum.micros()).roundTo(dp);
            }
            allocated += share;
            out.components.push_back({t.id, t.name, base, share});
        }
    }

    // ── exclusive taxes ───────────────────────────────────────
    for (const TaxDef* t : excluded) {
        Money amt = Money::zero(ccy);
        if (t->amountType == "fixed") amt = M(t->rate.mulQty(quantity).micros());
        else                          amt = base.percent(t->rate);
        // RULE 2: round each line's tax before summing.
        amt = amt.roundTo(dp);
        out.components.push_back({t->id, t->name, base, amt});
    }

    for (const auto& c : out.components) out.totalTax += c.amount;
    out.totalIncluded = out.baseExcluded + out.totalTax;
    return out;
}

} // namespace cerp::core

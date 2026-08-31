#pragma once
// ============================================================
// core/Money.hpp
//
// Exact fixed-point money / price / quantity. (P2 — docs/047, docs/048)
//
// WHY THIS EXISTS
//   Amounts were held in `double` above a NUMERIC(16,2..4) database.
//   Measured consequences (scripts/precision_demo.cpp):
//     * 0.00042 is not representable — it stores as 0.00042000000000000002,
//       so component prices were already inexact at the precision that
//       matters most for cheap parts.
//     * Accumulating a price in a loop and multiplying it give DIFFERENT
//       results from identical data, so two code paths silently disagree.
//   Both vanish with an integer representation.
//
// REPRESENTATION
//   int64_t of MICRO-UNITS — scale 6, i.e. value x 1'000'000.
//     resolution  0.000001   (a 0.00042 resistor is 420 micros)
//     range       +/- 9.22 trillion major units
//   Storage scale is FIXED and internal. User-configurable precision
//   (docs/048 §2) is a *display and rounding* concern applied on the way
//   out — never a change to how values are stored.
//
// CURRENCY
//   Every Money carries a currency id (0 = company base). Adding two
//   different currencies throws rather than silently producing nonsense —
//   the exact bug a `double` cannot catch.
//
// ROUNDING
//   Half away from zero ("half-up" in the accounting sense): 2.5 -> 3,
//   -2.5 -> -3. Applied per invoice line, after tax (docs/048 §3 option A),
//   so a printed column always foots to its printed total.
// ============================================================
#include "infrastructure/Errors.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cerp::core {

// ── Currency descriptor ───────────────────────────────────────
// Mirrors res_currency. `rate` follows the convention fixed in docs/048 §4.3:
//   rate = how many BASE units equal 1 unit of THIS currency.
//   With MYR as base: MYR = 1.000000, USD = 4.700000.
struct Currency {
    int         id            = 0;        ///< res_currency.id; 0 = base
    std::string code          = "";       ///< "MYR", "USD"
    int         decimalPlaces = 2;        ///< display/rounding precision
    int64_t     roundingStep  = 10000;    ///< micros; 0.01 = 10'000. 0 = use decimalPlaces
    int64_t     rate          = 1000000;  ///< micros; base units per 1 of this
};


class Money {
public:
    static constexpr int     SCALE = 6;
    static constexpr int64_t ONE   = 1000000;   ///< one major unit in micros

    // ── Construction ──────────────────────────────────────────
    constexpr Money() = default;
    constexpr Money(int64_t micros, int currencyId)
        : u_(micros), ccy_(currencyId) {}

    static constexpr Money fromMicros(int64_t m, int ccy = 0) { return Money(m, ccy); }
    static constexpr Money zero(int ccy = 0)                  { return Money(0, ccy); }

    /// Parse a decimal string ("0.00042", "-1234.5", "12"). Throws on garbage.
    static Money parse(std::string_view text, int ccy = 0);

    /// From a BIGINT column read as text — the value is already micros.
    static Money fromDb(std::string_view bigintText, int ccy = 0);

    /// From a JSON number in MAJOR units. Rounds to SCALE.
    /// Only for the wire boundary; never for arithmetic.
    static Money fromJson(double major, int ccy = 0);

    // ── Access ────────────────────────────────────────────────
    constexpr int64_t micros()     const { return u_; }
    constexpr int     currencyId() const { return ccy_; }
    constexpr bool    isZero()     const { return u_ == 0; }
    constexpr bool    isNegative() const { return u_ < 0; }

    /// Value for a BIGINT column — micros, unchanged.
    constexpr int64_t toDb() const { return u_; }

    /// MAJOR units for JSON. Lossy by design; the wire is not the ledger.
    constexpr double toJson() const { return static_cast<double>(u_) / ONE; }

    /// Fixed-point decimal string at `dp` places, e.g. toString(2) -> "164.52".
    std::string toString(int dp) const;

    // ── Arithmetic ────────────────────────────────────────────
    // + and - require matching currencies. This is the guardrail: silently
    // adding USD to MYR is precisely what this type exists to prevent.
    Money operator+(const Money& o) const;
    Money operator-(const Money& o) const;
    Money operator-() const { return Money(-u_, ccy_); }
    Money& operator+=(const Money& o) { *this = *this + o; return *this; }
    Money& operator-=(const Money& o) { *this = *this - o; return *this; }

    constexpr bool operator==(const Money& o) const { return u_ == o.u_ && ccy_ == o.ccy_; }
    constexpr bool operator!=(const Money& o) const { return !(*this == o); }
    bool operator<(const Money& o) const;
    bool operator>(const Money& o) const { return o < *this; }
    bool operator<=(const Money& o) const { return !(o < *this); }
    bool operator>=(const Money& o) const { return !(*this < o); }

    /// price x quantity. Both scale 6; __int128 intermediate so a large
    /// quantity of a cheap part cannot overflow before rescaling.
    Money mulQty(const Money& qty) const;

    /// Scale by an integer count (10'000 resistors). Exact, no rescale.
    Money mulInt(int64_t n) const;

    /// Percentage, where `pct` is itself a Money-scaled figure (8% -> 8.0).
    /// Used for tax and discount.
    Money percent(const Money& pct) const;

    /// Partial-period apportionment: amount x days / totalDays.
    /// The rental billing engine's first-period proration.
    Money prorate(int64_t days, int64_t totalDays) const;

    /// Integer division into n equal parts, distributing the remainder so the
    /// parts sum EXACTLY back to the original — no lost or invented micros.
    /// Used to split a payment across allocations.
    std::vector<Money> split(int n) const;

    // ── Rounding ──────────────────────────────────────────────
    /// Round to `dp` decimal places, half away from zero. dp in [0, SCALE].
    Money roundTo(int dp) const;

    /// Round per the currency: honours roundingStep when set (so a 0.05 cash
    /// step works without special-casing), else decimalPlaces.
    Money roundToCurrency(const Currency& c) const;

    // ── Currency conversion ───────────────────────────────────
    /// Convert to `to` using an explicit rate (base units per 1 of THIS
    /// currency). The rate is always passed in, never looked up, because
    /// documents must use their own snapshot rather than today's value
    /// (docs/048 §4.4).
    Money convertTo(int toCurrencyId, int64_t rate) const;

    /// Effective rate implied by receiving `received` for this amount.
    /// Used at settlement, where the bank's rate is derived from what
    /// actually landed rather than entered (docs/048 §4.6).
    int64_t impliedRate(const Money& received) const;

private:
    int64_t u_   = 0;   ///< micro-units
    int     ccy_ = 0;   ///< res_currency.id; 0 = base

    void requireSameCurrency_(const Money& o, const char* op) const;
};

} // namespace cerp::core

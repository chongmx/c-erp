// ============================================================
// core/Money.cpp — see Money.hpp for the rationale
// ============================================================
#include "Money.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <vector>

namespace cerp::core {

using infrastructure::ValidationError;

namespace {

/// 10^n for n in [0,18]. Table rather than pow() — exact, and pow() on
/// doubles is precisely the thing this file exists to avoid.
constexpr int64_t kPow10[19] = {
    1LL, 10LL, 100LL, 1000LL, 10000LL, 100000LL, 1000000LL,
    10000000LL, 100000000LL, 1000000000LL, 10000000000LL,
    100000000000LL, 1000000000000LL, 10000000000000LL,
    100000000000000LL, 1000000000000000LL, 10000000000000000LL,
    100000000000000000LL, 1000000000000000000LL
};

/// Divide, rounding half away from zero. Both operands may be negative.
inline __int128 divRoundHalfAway(__int128 num, __int128 den) {
    if (den == 0) throw ValidationError("Money: division by zero");
    const bool neg = (num < 0) != (den < 0);
    __int128 a = num < 0 ? -num : num;
    __int128 b = den < 0 ? -den : den;
    __int128 q = (a + b / 2) / b;
    return neg ? -q : q;
}

inline int64_t clampToInt64(__int128 v, const char* what) {
    constexpr __int128 kMax = static_cast<__int128>(std::numeric_limits<int64_t>::max());
    constexpr __int128 kMin = static_cast<__int128>(std::numeric_limits<int64_t>::min());
    if (v > kMax || v < kMin)
        throw ValidationError(std::string("Money: ") + what + " overflowed the representable range");
    return static_cast<int64_t>(v);
}

} // namespace


// ── Construction ──────────────────────────────────────────────

Money Money::parse(std::string_view text, int ccy) {
    // Trim
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))  text.remove_suffix(1);
    if (text.empty()) return Money(0, ccy);

    bool neg = false;
    if (text.front() == '+' || text.front() == '-') {
        neg = (text.front() == '-');
        text.remove_prefix(1);
    }
    if (text.empty()) throw ValidationError("Money: malformed number");

    int64_t whole = 0;
    std::size_t i = 0;
    bool sawDigit = false;
    for (; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i) {
        sawDigit = true;
        // Guard before multiplying so a long digit run cannot wrap silently.
        if (whole > (std::numeric_limits<int64_t>::max() - 9) / 10)
            throw ValidationError("Money: value too large");
        whole = whole * 10 + (text[i] - '0');
    }

    int64_t frac = 0;
    if (i < text.size() && text[i] == '.') {
        ++i;
        int digits = 0;
        for (; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i) {
            sawDigit = true;
            if (digits < SCALE) { frac = frac * 10 + (text[i] - '0'); ++digits; }
            // Digits beyond SCALE are discarded, not rounded: parse() is an
            // input path, and silently rounding user input hides data entry
            // errors. Callers that want rounding call roundTo() explicitly.
        }
        frac *= kPow10[SCALE - digits];
    }

    if (!sawDigit || i != text.size())
        throw ValidationError("Money: malformed number '" + std::string(text) + "'");

    const __int128 total = static_cast<__int128>(whole) * ONE + frac;
    const int64_t  v     = clampToInt64(total, "parsed value");
    return Money(neg ? -v : v, ccy);
}

Money Money::fromDb(std::string_view bigintText, int ccy) {
    if (bigintText.empty()) return Money(0, ccy);
    // Already micros — a plain integer.
    bool neg = false;
    std::size_t i = 0;
    if (bigintText[0] == '+' || bigintText[0] == '-') { neg = bigintText[0] == '-'; i = 1; }
    int64_t v = 0;
    for (; i < bigintText.size(); ++i) {
        const char c = bigintText[i];
        if (!std::isdigit(static_cast<unsigned char>(c)))
            throw ValidationError("Money::fromDb: not an integer: '" + std::string(bigintText) + "'");
        if (v > (std::numeric_limits<int64_t>::max() - 9) / 10)
            throw ValidationError("Money::fromDb: value out of range");
        v = v * 10 + (c - '0');
    }
    return Money(neg ? -v : v, ccy);
}

Money Money::fromJson(double major, int ccy) {
    // Wire boundary only. Round rather than truncate so 0.1 arriving as
    // 0.09999999999999999 lands on 100000 micros and not 99999.
    const double scaled = major * static_cast<double>(ONE);
    if (!(scaled >= -9.2e18 && scaled <= 9.2e18))
        throw ValidationError("Money::fromJson: value out of range");
    return Money(static_cast<int64_t>(scaled < 0 ? scaled - 0.5 : scaled + 0.5), ccy);
}


// ── Formatting ────────────────────────────────────────────────

std::string Money::toString(int dp) const {
    if (dp < 0) dp = 0;
    if (dp > SCALE) dp = SCALE;

    const Money r  = roundTo(dp);
    const bool  neg = r.u_ < 0;
    // Negate via unsigned to stay well-defined at INT64_MIN.
    const uint64_t mag = neg ? (~static_cast<uint64_t>(r.u_) + 1u)
                             : static_cast<uint64_t>(r.u_);

    const uint64_t whole = mag / static_cast<uint64_t>(ONE);
    const uint64_t frac  = mag % static_cast<uint64_t>(ONE);

    std::string out;
    if (neg) out += '-';
    out += std::to_string(whole);
    if (dp > 0) {
        std::string f = std::to_string(frac);
        f.insert(f.begin(), SCALE - f.size(), '0');   // pad to SCALE digits
        f.resize(dp);                                  // already rounded
        out += '.';
        out += f;
    }
    return out;
}


// ── Currency guard ────────────────────────────────────────────

void Money::requireSameCurrency_(const Money& o, const char* op) const {
    if (ccy_ != o.ccy_)
        throw ValidationError(
            std::string("Money: cannot ") + op + " amounts in different currencies (" +
            std::to_string(ccy_) + " vs " + std::to_string(o.ccy_) +
            "). Convert to a common currency first.");
}


// ── Arithmetic ────────────────────────────────────────────────

Money Money::operator+(const Money& o) const {
    requireSameCurrency_(o, "add");
    return Money(clampToInt64(static_cast<__int128>(u_) + o.u_, "sum"), ccy_);
}

Money Money::operator-(const Money& o) const {
    requireSameCurrency_(o, "subtract");
    return Money(clampToInt64(static_cast<__int128>(u_) - o.u_, "difference"), ccy_);
}

bool Money::operator<(const Money& o) const {
    requireSameCurrency_(o, "compare");
    return u_ < o.u_;
}

Money Money::mulQty(const Money& qty) const {
    // Both operands are scale 6, so the raw product is scale 12 — divide by
    // ONE to return to scale 6. __int128 keeps 10'000 x a cheap part exact.
    const __int128 prod = static_cast<__int128>(u_) * static_cast<__int128>(qty.u_);
    return Money(clampToInt64(divRoundHalfAway(prod, ONE), "product"), ccy_);
}

Money Money::mulInt(int64_t n) const {
    return Money(clampToInt64(static_cast<__int128>(u_) * n, "product"), ccy_);
}

Money Money::percent(const Money& pct) const {
    // pct is a Money-scaled figure: 8% arrives as 8.000000.
    const __int128 prod = static_cast<__int128>(u_) * static_cast<__int128>(pct.u_);
    return Money(clampToInt64(divRoundHalfAway(prod, static_cast<__int128>(ONE) * 100),
                              "percentage"), ccy_);
}

Money Money::prorate(int64_t days, int64_t totalDays) const {
    if (totalDays == 0) throw ValidationError("Money::prorate: totalDays is zero");
    const __int128 num = static_cast<__int128>(u_) * days;
    return Money(clampToInt64(divRoundHalfAway(num, totalDays), "prorated amount"), ccy_);
}

std::vector<Money> Money::split(int n) const {
    if (n <= 0) throw ValidationError("Money::split: parts must be positive");
    std::vector<Money> out;
    out.reserve(static_cast<std::size_t>(n));

    const int64_t base = u_ / n;
    int64_t       rem  = u_ - base * n;      // signed remainder, |rem| < n

    // Distribute the remainder one micro at a time so the parts sum back
    // EXACTLY. Splitting 10.00 three ways gives 3.333334/3.333333/3.333333,
    // not three equal parts that lose a micro.
    const int64_t step = (rem < 0) ? -1 : 1;
    for (int i = 0; i < n; ++i) {
        int64_t v = base;
        if (rem != 0) { v += step; rem -= step; }
        out.push_back(Money(v, ccy_));
    }
    return out;
}


// ── Rounding ──────────────────────────────────────────────────

Money Money::roundTo(int dp) const {
    if (dp < 0)      dp = 0;
    if (dp >= SCALE) return *this;              // nothing to do at full scale

    const int64_t factor = kPow10[SCALE - dp];
    const __int128 q = divRoundHalfAway(static_cast<__int128>(u_), factor);
    return Money(clampToInt64(q * factor, "rounded value"), ccy_);
}

Money Money::roundToCurrency(const Currency& c) const {
    if (c.roundingStep > 0) {
        const __int128 q = divRoundHalfAway(static_cast<__int128>(u_), c.roundingStep);
        return Money(clampToInt64(q * c.roundingStep, "rounded value"), ccy_);
    }
    return roundTo(c.decimalPlaces);
}


// ── Conversion ────────────────────────────────────────────────

Money Money::convertTo(int toCurrencyId, int64_t rate) const {
    // rate is base-units-per-1-of-this-currency, itself scale 6.
    const __int128 prod = static_cast<__int128>(u_) * static_cast<__int128>(rate);
    return Money(clampToInt64(divRoundHalfAway(prod, ONE), "converted amount"), toCurrencyId);
}

int64_t Money::impliedRate(const Money& received) const {
    if (u_ == 0) throw ValidationError("Money::impliedRate: source amount is zero");
    const __int128 num = static_cast<__int128>(received.micros()) * ONE;
    return clampToInt64(divRoundHalfAway(num, u_), "implied rate");
}

} // namespace cerp::core

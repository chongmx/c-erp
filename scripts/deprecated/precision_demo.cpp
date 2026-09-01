// =============================================================
// Does `double` actually hold up at 4-5 decimal places?
// Concrete test with component-pricing numbers.
//   g++ -std=c++20 -O2 -o /tmp/precdemo scripts/precision_demo.cpp && /tmp/precdemo
// =============================================================
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

// --- fixed point: int64 scaled by 1e6 (micro-units) ---
struct Dec {
    int64_t u = 0;                                   // value x 1'000'000
    static Dec of(long long whole, long long micros) { return {whole * 1000000 + micros}; }
    static Dec fromStr(double d) { return {(int64_t)std::llround(d * 1000000.0)}; }
    Dec operator+(Dec o) const { return {u + o.u}; }
    // price x qty -> exact, then round once to the money scale (2dp)
    Dec mulQty(int64_t qty) const { return {u * qty}; }
    int64_t roundToCents() const {                    // half-up
        int64_t r = (u < 0 ? -u : u), q = (r + 5000) / 10000;
        return u < 0 ? -q : q;
    }
    std::string str(int dp) const {
        char b[64]; double v = (double)u / 1000000.0;
        std::snprintf(b, sizeof b, "%.*f", dp, v); return b;
    }
};

int main() {
    std::puts("=== 1. can double even hold a 5-dp component price? ===");
    const double p = 0.00042;                        // RM 0.00042 per resistor
    std::printf("  literal 0.00042 stored as double : %.20f\n", p);
    std::printf("  exact?                            : %s\n\n",
                (p == 0.00042 ? "compares equal to itself (meaningless)" : "no"));

    std::puts("=== 2. 10,000 resistors at 0.00042 ===");
    double acc = 0.0;
    for (int i = 0; i < 10000; ++i) acc += p;        // accumulate, as a line loop would
    const double mul = p * 10000;
    std::printf("  double, accumulated : %.10f\n", acc);
    std::printf("  double, multiplied  : %.10f\n", mul);
    std::printf("  they agree?         : %s\n", (acc == mul ? "yes" : "NO  <-- same data, two answers"));

    Dec dp_ = Dec::of(0, 420);                       // 0.000420 exactly
    Dec tot = dp_.mulQty(10000);
    std::printf("  fixed-point         : %s   (cents: %lld)\n\n",
                tot.str(5).c_str(), (long long)tot.roundToCents());

    std::puts("=== 3. a BOM: 7 lines, odd quantities, then invoice ===");
    struct L { double price; int qty; };
    L lines[] = {{0.00042,10000},{0.00135,4500},{0.0007,33000},
                 {0.01925,850},{0.00008,120000},{0.00317,2750},{0.00061,9900}};
    double dsum = 0.0; Dec fsum{0};
    for (auto& l : lines) {
        dsum += l.price * l.qty;
        fsum = fsum + Dec::fromStr(l.price).mulQty(l.qty);
    }
    std::printf("  double sum          : %.10f\n", dsum);
    std::printf("  fixed sum           : %s\n", fsum.str(10).c_str());
    std::printf("  double -> cents     : %lld\n", (long long)std::llround(dsum * 100));
    std::printf("  fixed  -> cents     : %lld\n", (long long)fsum.roundToCents());
    std::printf("  drift               : %.12f\n\n", dsum - (double)fsum.u / 1000000.0);

    std::puts("=== 4. the case that actually bites: rent proration + tax ===");
    const double rate = 300.0;                        // RM 300/month
    const double prorated = rate * 17.0 / 31.0;       // 17 of 31 days
    const double taxed = prorated * 1.08;             // +8%
    std::printf("  double prorated     : %.10f\n", prorated);
    std::printf("  double +8%%          : %.10f\n", taxed);
    std::printf("  rounded             : %.2f\n", std::round(taxed * 100) / 100);

    // 12 such invoices, summed, vs the sum rounded per invoice
    double sumOfRounded = 0.0, roundOfSum = 0.0;
    for (int i = 0; i < 12; ++i) { sumOfRounded += std::round(taxed * 100) / 100; roundOfSum += taxed; }
    roundOfSum = std::round(roundOfSum * 100) / 100;
    std::printf("  12x, rounded each   : %.2f\n", sumOfRounded);
    std::printf("  12x, rounded once   : %.2f\n", roundOfSum);
    std::printf("  disagreement        : %.2f  <-- the AR-vs-invoices mismatch\n", sumOfRounded - roundOfSum);
    return 0;
}

// ============================================================
// tests/test_money.cpp — unit tests for core/Money (P2)
//
//   g++ -std=c++20 -I core -I core/infrastructure \
//       -o /tmp/test_money tests/test_money.cpp core/Money.cpp && /tmp/test_money
//
// Seeds the Stage 3 ERP_TEST harness (docs/040); ports with a main() swap.
// ============================================================
#include "Money.hpp"
#include "TestHarness.hpp"

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using odoo::core::Money;
using odoo::core::Currency;
using erptest::section;

// Soft checks throughout: a money bug is usually systematic, so seeing all
// of it in one run beats stopping at the first symptom. The reporting now
// comes from the shared harness rather than a per-file counter.
static void ck(bool cond, const std::string& what) {
    erptest::check(cond, what);
}
static void eq(const Money& got, const Money& want, const std::string& what) {
    erptest::check(got == want,
        got == want ? what
                    : what + "\n            got  " + got.toString(6) +
                      " (" + std::to_string((long long)got.micros()) + " micros)" +
                      "\n            want " + want.toString(6) +
                      " (" + std::to_string((long long)want.micros()) + " micros)");
}
static void eqs(const std::string& got, const std::string& want, const std::string& what) {
    erptest::check(got == want,
        got == want ? what
                    : what + "\n            got  '" + got + "'\n            want '" + want + "'");
}

constexpr int MYR = 1, USD = 2;

// One case per file rather than one per section: the sections share setup
// and read as a single narrative about the type. section() still labels
// them in the output, and soft checks mean a failure in section 2 does not
// hide one in section 9.
ERP_TEST(Money, all) {
    // ---------------------------------------------------------
    section("parse / format round-trip");
    eq(Money::parse("0.00042"), Money::fromMicros(420),      "0.00042 -> 420 micros");
    eq(Money::parse("164.52"),  Money::fromMicros(164520000), "164.52");
    eq(Money::parse("-2.5"),    Money::fromMicros(-2500000),  "negative");
    eq(Money::parse("12"),      Money::fromMicros(12000000),  "integer");
    eq(Money::parse(""),        Money::fromMicros(0),         "empty -> zero");
    eq(Money::parse("  7.25  "),Money::fromMicros(7250000),   "whitespace trimmed");
    eqs(Money::fromMicros(420).toString(6),   "0.000420", "toString(6)");
    eqs(Money::fromMicros(164520000).toString(2), "164.52", "toString(2)");
    eqs(Money::fromMicros(-2500000).toString(2),  "-2.50", "negative toString");
    eqs(Money::fromMicros(0).toString(2),         "0.00",  "zero toString");

    bool threw = false;
    try { Money::parse("12.3.4"); } catch (...) { threw = true; }
    ck(threw, "malformed input throws");

    // ---------------------------------------------------------
    section("the exact cases double got wrong (scripts/precision_demo.cpp)");
    {
        // 10,000 resistors at 0.00042 — accumulate vs multiply MUST agree.
        const Money price = Money::parse("0.00042");
        Money acc = Money::zero();
        for (int i = 0; i < 10000; ++i) acc += price;
        const Money mul = price.mulInt(10000);
        eq(acc, mul,                       "accumulate == multiply (double disagreed)");
        eq(mul, Money::parse("4.20"),      "10,000 x 0.00042 = 4.20 exactly");
        eqs(mul.toString(2), "4.20",       "displays as 4.20");
    }
    {
        // mulQty: price x quantity, both scale 6
        const Money price = Money::parse("0.00042");
        const Money qty   = Money::parse("10000");
        eq(price.mulQty(qty), Money::parse("4.20"), "mulQty 0.00042 x 10000");
    }
    {
        // The 7-line BOM from the demo
        struct L { const char* p; int q; };
        const L lines[] = {{"0.00042",10000},{"0.00135",4500},{"0.0007",33000},
                           {"0.01925",850},{"0.00008",120000},{"0.00317",2750},{"0.00061",9900}};
        Money sum = Money::zero();
        for (auto& l : lines) sum += Money::parse(l.p).mulInt(l.q);
        eqs(sum.toString(4), "74.0940", "7-line BOM sums exactly");
    }

    // ---------------------------------------------------------
    section("rounding — half away from zero");
    eq(Money::parse("2.345").roundTo(2),  Money::parse("2.35"),  "2.345 -> 2.35");
    eq(Money::parse("2.344").roundTo(2),  Money::parse("2.34"),  "2.344 -> 2.34");
    eq(Money::parse("-2.345").roundTo(2), Money::parse("-2.35"), "-2.345 -> -2.35 (away from zero)");
    eq(Money::parse("2.5").roundTo(0),    Money::parse("3"),     "2.5 -> 3");
    eq(Money::parse("-2.5").roundTo(0),   Money::parse("-3"),    "-2.5 -> -3");
    eq(Money::parse("0.000420").roundTo(4), Money::parse("0.0004"), "0.00042 -> 4dp");
    eq(Money::parse("1.234567").roundTo(6), Money::parse("1.234567"), "roundTo(SCALE) is identity");

    // ---------------------------------------------------------
    section("rounding policy: per line, then sum (docs/048 option A)");
    {
        // 10 lines of 0.0425 at 2dp. Option A: round each, then sum.
        Money sumOfRounded = Money::zero();
        Money exactSum     = Money::zero();
        for (int i = 0; i < 10; ++i) {
            const Money line = Money::parse("0.0425");
            sumOfRounded += line.roundTo(2);
            exactSum     += line;
        }
        eqs(sumOfRounded.toString(2), "0.40", "10 x 0.0425 rounded-then-summed = 0.40");
        eqs(exactSum.roundTo(2).toString(2), "0.43", "summed-then-rounded = 0.43");
        ck(sumOfRounded != exactSum.roundTo(2),
           "the two policies genuinely differ — option A is the chosen one");
    }
    {
        // The rent case from the demo: 300.00 prorated 17/31, +8% tax, x12
        const Money rate     = Money::parse("300");
        const Money prorated = rate.prorate(17, 31);
        const Money tax      = prorated.percent(Money::parse("8"));
        const Money lineTot  = (prorated + tax).roundTo(2);
        eqs(prorated.toString(6), "164.516129", "300 x 17/31");
        eqs(lineTot.toString(2),  "177.68",     "+8% tax, rounded per line");

        Money twelve = Money::zero();
        for (int i = 0; i < 12; ++i) twelve += lineTot;
        eqs(twelve.toString(2), "2132.16", "12 invoices foot to the printed total");
    }

    // ---------------------------------------------------------
    section("split — parts must sum back exactly");
    {
        const Money m = Money::parse("10.00");
        const auto parts = m.split(3);
        ck(parts.size() == 3, "split(3) yields 3 parts");
        Money back = Money::zero();
        for (auto& p : parts) back += p;
        eq(back, m, "3-way split sums back exactly (no lost micro)");
    }
    {
        const Money m = Money::parse("-0.000005");
        const auto parts = m.split(3);
        Money back = Money::zero();
        for (auto& p : parts) back += p;
        eq(back, m, "negative split sums back exactly");
    }

    // ---------------------------------------------------------
    section("currency guard");
    {
        const Money myr = Money::fromMicros(100000000, MYR);
        const Money usd = Money::fromMicros(100000000, USD);
        bool caught = false;
        try { (void)(myr + usd); } catch (const std::exception&) { caught = true; }
        ck(caught, "adding MYR to USD throws instead of silently succeeding");

        caught = false;
        try { (void)(myr < usd); } catch (const std::exception&) { caught = true; }
        ck(caught, "comparing different currencies throws");

        const Money a = Money::fromMicros(100, MYR), b = Money::fromMicros(200, MYR);
        ck((a + b).currencyId() == MYR, "same-currency add keeps the currency");
    }

    // ---------------------------------------------------------
    section("currency conversion (docs/048 §4.3 convention)");
    {
        // 1 USD = 4.70 MYR
        const int64_t usdRate = Money::parse("4.70").micros();
        const Money   hundred = Money::parse("100").micros() == 100000000
                              ? Money::fromMicros(100000000, USD) : Money::zero(USD);
        const Money   inMyr   = hundred.convertTo(MYR, usdRate);
        eqs(inMyr.toString(2), "470.00", "100 USD @ 4.70 = 470.00 MYR");
        ck(inMyr.currencyId() == MYR, "converted amount carries the target currency");
    }
    {
        // Settlement: bank paid 448.50 MYR for a 100 USD invoice (docs/048 §4.6)
        const Money invoiceUsd = Money::fromMicros(Money::parse("100").micros(), USD);
        const Money receivedMyr= Money::fromMicros(Money::parse("448.50").micros(), MYR);
        const int64_t implied  = invoiceUsd.impliedRate(receivedMyr);
        eqs(Money::fromMicros(implied).toString(6), "4.485000", "implied rate derived from MYR received");

        const Money bookedMyr = invoiceUsd.convertTo(MYR, Money::parse("4.70").micros());
        const Money fxDiff    = receivedMyr - bookedMyr;
        eqs(bookedMyr.toString(2), "470.00", "AR booked at snapshot rate");
        eqs(fxDiff.toString(2),    "-21.50", "realised FX loss = 21.50 MYR");
    }

    // ---------------------------------------------------------
    section("currency rounding step");
    {
        Currency chf; chf.id = 3; chf.decimalPlaces = 2; chf.roundingStep = 50000; // 0.05
        eqs(Money::parse("1.02").roundToCurrency(chf).toString(2), "1.00", "1.02 -> 1.00 at 0.05 step");
        eqs(Money::parse("1.03").roundToCurrency(chf).toString(2), "1.05", "1.03 -> 1.05 at 0.05 step");

        Currency jpy; jpy.id = 4; jpy.decimalPlaces = 0; jpy.roundingStep = 1000000; // 1.0
        eqs(Money::parse("123.6").roundToCurrency(jpy).toString(0), "124", "JPY rounds to whole yen");

        Currency myr; myr.id = MYR; myr.decimalPlaces = 2; myr.roundingStep = 10000;
        eqs(Money::parse("164.516129").roundToCurrency(myr).toString(2), "164.52", "MYR 2dp");
    }

    // ---------------------------------------------------------
    section("db / json boundaries");
    {
        const Money m = Money::parse("0.00042");
        eq(Money::fromDb(std::to_string(m.toDb())), m, "toDb -> fromDb round-trips");
        eq(Money::fromDb("-420"), Money::fromMicros(-420), "negative BIGINT");
        eq(Money::fromDb(""),     Money::zero(),           "NULL/empty -> zero");
        // JSON is lossy by design, but must survive a display-precision value
        eq(Money::fromJson(Money::parse("164.52").toJson()), Money::parse("164.52"),
           "164.52 survives the JSON boundary");
        eq(Money::fromJson(Money::parse("0.00042").toJson()), Money::parse("0.00042"),
           "0.00042 survives the JSON boundary");
    }

    // ---------------------------------------------------------
    section("overflow guards");
    {
        bool caught = false;
        try {
            Money::fromMicros(std::numeric_limits<int64_t>::max()).mulInt(2);
        } catch (const std::exception&) { caught = true; }
        ck(caught, "int64 overflow throws rather than wrapping");

        // A large but legitimate figure must NOT throw: 1 billion at scale 6
        const Money big = Money::parse("1000000000");
        eqs(big.toString(2), "1000000000.00", "1 billion is comfortably representable");

        caught = false;
        try { (void)Money::parse("1").prorate(1, 0); } catch (const std::exception&) { caught = true; }
        ck(caught, "prorate by zero throws");
    }

}

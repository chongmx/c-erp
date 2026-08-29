// ============================================================
// tests/test_tax.cpp — unit tests for core/TaxEngine (P3)
//
//   g++ -std=c++20 -I core -I core/infrastructure -o /tmp/test_tax \
//       tests/test_tax.cpp core/TaxEngine.cpp core/Money.cpp && /tmp/test_tax
// ============================================================
#include "TaxEngine.hpp"
#include "TestHarness.hpp"

#include <cstdio>
#include <string>

using namespace odoo::core;
using erptest::section;

static void eqs(const std::string& got, const std::string& want, const std::string& what) {
    erptest::check(got == want,
        got == want ? what : what + "\n            got  '" + got + "'  want '" + want + "'");
}
static void ck(bool c, const std::string& what) {
    erptest::check(c, what);
}

static TaxDef pct(int id, const char* name, const char* rate, bool incl = false, int seq = 0) {
    TaxDef t; t.id = id; t.name = name; t.rate = Money::parse(rate);
    t.amountType = "percent"; t.priceInclude = incl; t.sequence = seq;
    return t;
}

ERP_TEST(Tax, all) {
    const Money one = Money::parse("1");
    const Money zero = Money::zero();

    // ---------------------------------------------------------
    section("no tax");
    {
        auto r = TaxEngine::compute(Money::parse("100"), one, zero, {}, 2);
        eqs(r.baseExcluded.toString(2),  "100.00", "base");
        eqs(r.totalTax.toString(2),      "0.00",   "tax");
        eqs(r.totalIncluded.toString(2), "100.00", "total");
    }

    // ---------------------------------------------------------
    section("exclusive tax — the case that used to work");
    {
        auto r = TaxEngine::compute(Money::parse("100"), one, zero, {pct(1,"SST 8%","8")}, 2);
        eqs(r.baseExcluded.toString(2),  "100.00", "base excludes tax");
        eqs(r.totalTax.toString(2),      "8.00",   "tax = 8%");
        eqs(r.totalIncluded.toString(2), "108.00", "total");
        ck(r.components.size() == 1,               "one component");
        eqs(r.components[0].amount.toString(2), "8.00", "component amount");
    }

    // ---------------------------------------------------------
    section("INCLUSIVE tax — the case that silently produced ZERO tax");
    {
        // 108.00 gross, 8% included -> base 100.00, tax 8.00
        auto r = TaxEngine::compute(Money::parse("108"), one, zero,
                                    {pct(1,"SST 8% incl","8",true)}, 2);
        eqs(r.baseExcluded.toString(2),  "100.00", "base backed out of the gross");
        eqs(r.totalTax.toString(2),      "8.00",   "tax is NOT zero (the old bug)");
        eqs(r.totalIncluded.toString(2), "108.00", "total equals the quoted gross");
        ck(r.baseExcluded + r.totalTax == r.totalIncluded, "base + tax == total exactly");
    }

    // ---------------------------------------------------------
    section("inclusive: base+tax must ALWAYS equal the quoted gross");
    {
        // Values chosen so base x rate does not land on a clean cent —
        // this is where multiply-instead-of-subtract would drift.
        const char* grosses[] = {"0.01","0.07","1.03","9.99","12.34","99.95",
                                 "100.01","777.77","1234.56","0.05"};
        int bad = 0;
        for (const char* g : grosses) {
            auto r = TaxEngine::compute(Money::parse(g), one, zero,
                                        {pct(1,"T","8",true)}, 2);
            if (r.totalIncluded != Money::parse(g))          ++bad;
            if (r.baseExcluded + r.totalTax != r.totalIncluded) ++bad;
        }
        ck(bad == 0, "10 awkward gross values all reconcile exactly");
    }

    // ---------------------------------------------------------
    section("quantity and discount");
    {
        auto r = TaxEngine::compute(Money::parse("10"), Money::parse("3"), zero,
                                    {pct(1,"T","8")}, 2);
        eqs(r.baseExcluded.toString(2), "30.00", "10 x 3");
        eqs(r.totalTax.toString(2),     "2.40",  "8% of 30");

        auto d = TaxEngine::compute(Money::parse("100"), one, Money::parse("10"),
                                    {pct(1,"T","8")}, 2);
        eqs(d.baseExcluded.toString(2), "90.00", "100 less 10%");
        eqs(d.totalTax.toString(2),     "7.20",  "8% of 90");
    }

    // ---------------------------------------------------------
    section("component prices at 5 dp (the resistor case)");
    {
        // 10,000 resistors at 0.00042, 8% tax, line precision 5
        auto r = TaxEngine::compute(Money::parse("0.00042"), Money::parse("10000"),
                                    zero, {pct(1,"T","8")}, 5);
        eqs(r.baseExcluded.toString(5), "4.20000", "10,000 x 0.00042 exactly");
        eqs(r.totalTax.toString(5),     "0.33600", "8% of 4.20");
        eqs(r.totalIncluded.toString(5),"4.53600", "total");
    }

    // ---------------------------------------------------------
    section("multiple exclusive taxes");
    {
        auto r = TaxEngine::compute(Money::parse("100"), one, zero,
                                    {pct(1,"A","6",false,1), pct(2,"B","4",false,2)}, 2);
        eqs(r.baseExcluded.toString(2),  "100.00", "base");
        eqs(r.totalTax.toString(2),      "10.00",  "6% + 4%");
        eqs(r.totalIncluded.toString(2), "110.00", "total");
        ck(r.components.size() == 2, "two components");
        // Both apply to the SAME base — not compound. Stated so the
        // behaviour is a decision rather than an accident.
        eqs(r.components[0].amount.toString(2), "6.00", "A on the net base");
        eqs(r.components[1].amount.toString(2), "4.00", "B on the net base, not on A");
    }

    // ---------------------------------------------------------
    section("multiple INCLUSIVE taxes — components must sum exactly");
    {
        auto r = TaxEngine::compute(Money::parse("110"), one, zero,
                                    {pct(1,"A","6",true,1), pct(2,"B","4",true,2)}, 2);
        eqs(r.totalIncluded.toString(2), "110.00", "total equals the quoted gross");
        eqs(r.baseExcluded.toString(2),  "100.00", "base backed out of 10% combined");
        Money sum = Money::zero();
        for (auto& c : r.components) sum += c.amount;
        ck(sum == r.totalTax, "components sum to the total tax exactly");
        ck(r.baseExcluded + r.totalTax == r.totalIncluded, "base + tax == gross");
    }

    // ---------------------------------------------------------
    section("rounding policy: round per line, then sum (docs/048 option A)");
    {
        // 3 lines of 0.0425 at 2 dp with no tax: each rounds to 0.04,
        // summing to 0.12 — which is what the printed column will show.
        Money summed = Money::zero();
        for (int i = 0; i < 3; ++i) {
            auto r = TaxEngine::compute(Money::parse("0.0425"), one, zero, {}, 2);
            summed += r.totalIncluded;
        }
        eqs(summed.toString(2), "0.12", "3 x round(0.0425) = 0.12, matching the printed lines");
    }

    // ---------------------------------------------------------
    section("fixed-amount tax");
    {
        auto r = TaxEngine::compute(Money::parse("100"), Money::parse("2"), zero,
                                    {[]{ auto t = pct(1,"Levy","1.50"); t.amountType="fixed"; return t; }()}, 2);
        eqs(r.baseExcluded.toString(2), "200.00", "100 x 2");
        eqs(r.totalTax.toString(2),     "3.00",   "1.50 per unit x 2");
    }

    // ---------------------------------------------------------
    section("zero and negative amounts (credit notes)");
    {
        auto z = TaxEngine::compute(zero, one, zero, {pct(1,"T","8")}, 2);
        eqs(z.totalIncluded.toString(2), "0.00", "zero line stays zero");

        auto n = TaxEngine::compute(Money::parse("-100"), one, zero, {pct(1,"T","8")}, 2);
        eqs(n.baseExcluded.toString(2),  "-100.00", "credit note base");
        eqs(n.totalTax.toString(2),      "-8.00",   "tax is credited too");
        eqs(n.totalIncluded.toString(2), "-108.00", "total");
        ck(n.baseExcluded + n.totalTax == n.totalIncluded, "reconciles when negative");
    }

    // ---------------------------------------------------------
    section("currency-tagged lines (regression: this used to throw)");
    {
        // Money enforces currency on every operation. The engine originally
        // built its intermediates with the default currency 0, so any line in
        // a non-base currency threw "cannot add amounts in different
        // currencies" and no USD invoice could be computed at all.
        constexpr int USD = 2;
        const Money hundredUsd = Money::fromMicros(Money::parse("100").micros(), USD);
        auto r = TaxEngine::compute(hundredUsd, one, zero, {pct(1,"T","8")}, 2);
        eqs(r.baseExcluded.toString(2),  "100.00", "USD exclusive base");
        eqs(r.totalTax.toString(2),      "8.00",   "USD exclusive tax");
        ck(r.baseExcluded.currencyId()  == USD, "base keeps the line currency");
        ck(r.totalTax.currencyId()      == USD, "tax keeps the line currency");
        ck(r.totalIncluded.currencyId() == USD, "total keeps the line currency");

        const Money gross108 = Money::fromMicros(Money::parse("108").micros(), USD);
        auto ri = TaxEngine::compute(gross108, one, zero, {pct(1,"T","8",true)}, 2);
        eqs(ri.baseExcluded.toString(2), "100.00", "USD inclusive base");
        eqs(ri.totalTax.toString(2),     "8.00",   "USD inclusive tax");
        ck(ri.totalIncluded == gross108, "USD inclusive total equals the quoted gross");
    }

    // ---------------------------------------------------------
    section("invariant sweep: base + tax == total, always");
    {
        const char* prices[] = {"0.01","0.33","1.11","7.77","19.99","123.45","9999.99"};
        const char* qtys[]   = {"1","3","7","11.5"};
        const char* rates[]  = {"0","6","8","10","15.5"};
        int bad = 0, n = 0;
        for (const char* p : prices)
          for (const char* q : qtys)
            for (const char* rt : rates)
              for (bool incl : {false, true}) {
                  auto r = TaxEngine::compute(Money::parse(p), Money::parse(q), zero,
                                              {pct(1,"T",rt,incl)}, 2);
                  ++n;
                  if (r.baseExcluded + r.totalTax != r.totalIncluded) ++bad;
              }
        std::printf("      %d combinations checked\n", n);
        ck(bad == 0, "every combination reconciles exactly");
    }

}

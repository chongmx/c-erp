// ============================================================
// tests/TestHarness.hpp — P7, the ERP_TEST harness (docs/033 §3)
//
// Before this, each test file carried its own main(), its own failure
// counter and its own hand-written g++ command in a comment. Nothing
// built them, so "run the tests" meant remembering three compiler
// invocations — and a test that stopped compiling stayed broken
// silently, because nothing in CI ever tried.
//
// A case registers itself at static-init time:
//
//     ERP_TEST(Money, rounding) {
//         ASSERT_EQ(Money::parse("1.005").roundTo(2), Money::parse("1.01"));
//     }
//
// Two failure styles are supported deliberately:
//
//   * ASSERT_*  throws — the case stops at the first failure. Right when
//     later assertions would be meaningless or would crash.
//   * check()/CHECK  records and continues — the case reports every
//     failure in one run. Right for table-driven sweeps, where knowing
//     that 3 of 280 combinations fail is far more useful than knowing
//     the first one does.
//
// Both feed the same counters, so a mixed suite still reports one total.
// ============================================================
#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace erptest {

struct TestCase {
    std::string           name;
    std::function<void()> fn;
};

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry r;
        return r;
    }

    void add(const std::string& name, std::function<void()> fn) {
        cases_.push_back({name, std::move(fn)});
    }

    // Soft check: records and keeps going.
    void record(bool ok, const std::string& what) {
        ++checks_;
        if (!ok) {
            ++softFailures_;
            std::printf("    FAIL  %s\n", what.c_str());
        }
    }

    void note(const char* s) { std::printf("\n  ### %s\n", s); }

    std::size_t checkCount() const { return checks_; }

    /**
     * Runs every registered case.
     *
     * @param filter substring; empty runs all. Lets a single suite be run
     *               while iterating on it: `erp_tests Money`.
     * @returns number of failed cases.
     */
    int runAll(const std::string& filter = "") {
        int failed = 0, ran = 0, skipped = 0;

        for (const auto& c : cases_) {
            if (!filter.empty() && c.name.find(filter) == std::string::npos) {
                ++skipped;
                continue;
            }
            ++ran;
            const std::size_t softBefore = softFailures_;
            std::printf("\n[ RUN      ] %s\n", c.name.c_str());

            std::string err;
            try {
                c.fn();
            } catch (const std::exception& ex) {
                err = ex.what();
            } catch (...) {
                err = "unknown exception";
            }

            const bool softFailed = softFailures_ > softBefore;
            if (!err.empty()) {
                // An assertion threw, or the case itself blew up. Either way
                // the remaining assertions in it did not run — say so, rather
                // than reporting a count that looks complete.
                std::printf("    ABORTED  %s\n", err.c_str());
                std::printf("[   FAILED ] %s\n", c.name.c_str());
                ++failed;
            } else if (softFailed) {
                std::printf("[   FAILED ] %s  (%zu check%s failed)\n", c.name.c_str(),
                            softFailures_ - softBefore,
                            (softFailures_ - softBefore) == 1 ? "" : "s");
                ++failed;
            } else {
                std::printf("[       OK ] %s\n", c.name.c_str());
            }
        }

        std::printf("\n============================================\n");
        std::printf("  %d case%s run, %zu assertion%s\n",
                    ran, ran == 1 ? "" : "s",
                    checks_, checks_ == 1 ? "" : "s");
        if (skipped) std::printf("  %d case%s skipped by filter\n",
                                 skipped, skipped == 1 ? "" : "s");
        if (failed) std::printf("  *** %d CASE%s FAILED ***\n", failed, failed == 1 ? "" : "S");
        else        std::printf("  All tests passed.\n");
        std::printf("============================================\n");
        return failed;
    }

private:
    std::vector<TestCase> cases_;
    std::size_t           checks_       = 0;
    std::size_t           softFailures_ = 0;
};

inline void check(bool ok, const std::string& what) {
    TestRegistry::instance().record(ok, what);
}
inline void section(const char* s) { TestRegistry::instance().note(s); }

} // namespace erptest

// ------------------------------------------------------------
// Registration. The function is declared, registered by a static
// initialiser, then defined by the trailing brace at the call site — so a
// case is one block of code with no separate wiring to forget.
// ------------------------------------------------------------
#define ERP_TEST(suite, name)                                                  \
    static void erptest_##suite##_##name();                                    \
    static const bool erptest_reg_##suite##_##name = ([] {                     \
        ::erptest::TestRegistry::instance().add(                               \
            #suite "::" #name, [] { erptest_##suite##_##name(); });            \
        return true;                                                           \
    }());                                                                      \
    static void erptest_##suite##_##name()

// ------------------------------------------------------------
// Hard assertions — throw, aborting the case.
// ------------------------------------------------------------
#define ERPT_STR_(x) #x

#define ASSERT_TRUE(x)                                                         \
    do {                                                                       \
        ::erptest::TestRegistry::instance().record(true, "");                  \
        if (!(x))                                                              \
            throw std::runtime_error(std::string("ASSERT_TRUE(" ERPT_STR_(x)   \
                ") at " __FILE__ ":") + std::to_string(__LINE__));             \
    } while (0)

#define ASSERT_EQ(a, b)                                                        \
    do {                                                                       \
        ::erptest::TestRegistry::instance().record(true, "");                  \
        if (!((a) == (b)))                                                     \
            throw std::runtime_error(std::string("ASSERT_EQ(" ERPT_STR_(a)     \
                ", " ERPT_STR_(b) ") at " __FILE__ ":") +                      \
                std::to_string(__LINE__));                                     \
    } while (0)

#define ASSERT_THROW(expr, ex)                                                 \
    do {                                                                       \
        ::erptest::TestRegistry::instance().record(true, "");                  \
        bool erpt_caught = false;                                              \
        try { (void)(expr); } catch (const ex&) { erpt_caught = true; }        \
        if (!erpt_caught)                                                      \
            throw std::runtime_error("ASSERT_THROW: expected " ERPT_STR_(ex)   \
                " at " __FILE__ ":" + std::to_string(__LINE__));               \
    } while (0)

// Soft check — records and continues. `CHECK` is the spelling used inside
// sweeps where one failure should not hide the other 279.
#define CHECK(cond, what) ::erptest::check((cond), (what))

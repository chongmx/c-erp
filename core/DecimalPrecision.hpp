#pragma once
// ============================================================
// core/DecimalPrecision.hpp
//
// User-configurable DISPLAY precision. (P2 — docs/048 §2)
//
// Storage is always Money::SCALE (6) and is not configurable. These
// settings govern how many decimals are RENDERED and where rounding
// happens — a different concern entirely, and the one the user asked to
// control ("invoice 2 dp, internal stock 4 dp, resistor invoices 4 dp").
//
// Precedence (docs/048 §2.3):
//   line amount   = document.line_precision ?? digits("Account")
//   unit price    = digits("Product Price")
//   quantity      = digits("Product UoM")
//   TOTAL         = currency.decimal_places      <- never overridden
//
// The last rule matters: a 4 dp line precision affects the lines, never
// the amount due, because the total has to be payable in the currency.
//
// Lifecycle mirrors RuleEngine / AuditService: initialize() once at boot,
// then instance(). Values are cached for 300 s and can be invalidated
// explicitly when Settings writes them.
// ============================================================
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace odoo::infrastructure { class DbConnection; }

namespace odoo::core {

class DecimalPrecision {
public:
    // Well-known names, seeded by migration 901.
    static constexpr const char* kAccount      = "Account";
    static constexpr const char* kProductPrice = "Product Price";
    static constexpr const char* kProductUom   = "Product UoM";
    static constexpr const char* kDiscount     = "Discount";
    static constexpr const char* kStock        = "Stock";

    static void              initialize(std::shared_ptr<infrastructure::DbConnection> db);
    static DecimalPrecision& instance();
    static bool              ready();

    /**
     * @brief Decimals configured for `name`, or `fallback` if unknown.
     *
     * Never throws and never blocks on a DB error — precision is a display
     * concern, and a formatting lookup must not be able to fail a request.
     */
    int digits(const std::string& name, int fallback = 2) const;

    /// Drop the cache. Called after Settings writes a new value.
    void invalidate();

private:
    explicit DecimalPrecision(std::shared_ptr<infrastructure::DbConnection> db);

    void loadIfNeeded_() const;

    std::shared_ptr<infrastructure::DbConnection> db_;

    mutable std::mutex mutex_;
    mutable bool       loaded_ = false;
    mutable std::vector<std::pair<std::string,int>> values_;

    static std::once_flag                  s_once_;
    static std::unique_ptr<DecimalPrecision> s_instance_;
};

} // namespace odoo::core

#pragma once
// ============================================================
// core/IrSequence.hpp
//
// Configurable document numbering. (P4 — docs/045)
//
// WHAT IT REPLACES
//   Raw PostgreSQL sequences created inline in ensureSchema_()
//   (sale_order_seq, stock_in/out/int_seq, purchase_order_seq) plus
//   hand-built prefixes like `"SO/" << year << "/" << setw(4) << n`.
//   That gave no prefix/padding configuration, no per-company numbering,
//   no yearly reset, and — critically — no gap control.
//
// WHY GAPS MATTER
//   `nextval()` is non-transactional by design: it does NOT roll back.
//   If a document insert fails after taking a number, that number is
//   burned. For internal references nobody cares; for TAX INVOICES most
//   jurisdictions require a sequential, gapless series, and a burned
//   number is something an auditor will ask about.
//
//   So this offers both, explicitly:
//     nextByCode(txn, code)  — increments inside the CALLER'S transaction.
//                              Rolls back with it, so no gap. Serialises
//                              concurrent writers on the row lock.
//     nextByCode(code)       — own short transaction. Higher concurrency,
//                              may leave a gap if the caller later aborts.
//
//   Use the txn form for anything an auditor reads. The document insert
//   and the number allocation have to share a transaction or the
//   guarantee is void.
//
// CONCURRENCY
//   SELECT ... FOR UPDATE takes a row lock, so two sessions allocating
//   from the same sequence serialise rather than colliding. That is the
//   cost of gaplessness and it is the right trade for document numbers,
//   which are low-frequency.
//
// PREFIX PLACEHOLDERS (the reference ERP convention)
//   %(year)s %(y)s %(month)s %(day)s  — e.g. "INV/%(year)s/"
// ============================================================
#include <memory>
#include <optional>
#include <string>

// pqxx::work is a TYPE ALIAS for transaction<...>, so it cannot be
// forward-declared. transaction_base is its real base class and is a
// plain class — declaring that keeps <pqxx/pqxx> out of this header
// (PERF-E) while still accepting a pqxx::work by reference.
namespace pqxx { class transaction_base; }
namespace cerp::infrastructure { class DbConnection; }

namespace cerp::core {

class IrSequence {
public:
    static void        initialize(std::shared_ptr<infrastructure::DbConnection> db);
    static IrSequence& instance();
    static bool        ready();

    /**
     * @brief Next number for `code`, allocated inside the caller's transaction.
     *
     * GAPLESS: the increment commits or rolls back with the caller's work.
     * Use this for invoices and anything else that must not skip a number.
     *
     * @throws ValidationError if no active sequence exists for the code.
     */
    std::string nextByCode(pqxx::transaction_base& txn,
                           const std::string& code,
                           int companyId = 0);

    /**
     * @brief Next number for `code` in its own transaction.
     *
     * Releases the row lock immediately, so concurrent callers block for
     * less time — but a caller that later aborts leaves a gap. Fine for
     * internal references (pickings, order drafts); not for tax documents.
     */
    std::string nextByCode(const std::string& code, int companyId = 0);

    /// Preview the next value without consuming it (for UI display).
    std::optional<std::string> peek(const std::string& code, int companyId = 0);

    /// True if a sequence is configured. Lets callers fall back gracefully.
    bool has(const std::string& code, int companyId = 0);

private:
    explicit IrSequence(std::shared_ptr<infrastructure::DbConnection> db);

    /// Shared implementation; `consume` distinguishes next() from peek().
    std::string next_(pqxx::transaction_base& txn, const std::string& code,
                      int companyId, bool consume);

    /// Expand %(year)s etc. and apply padding.
    static std::string format_(const std::string& prefix,
                               const std::string& suffix,
                               long long number, int padding);

    /// Current period key for a reset policy: "" | "2026" | "2026-08".
    static std::string periodKey_(const std::string& resetPolicy);

    std::shared_ptr<infrastructure::DbConnection> db_;

    static std::unique_ptr<IrSequence> s_instance_;
};

} // namespace cerp::core

// ============================================================
// core/IrSequence.cpp — see IrSequence.hpp
// ============================================================
#include "IrSequence.hpp"
#include "infrastructure/DbConnection.hpp"
#include "infrastructure/Errors.hpp"

#include <pqxx/pqxx>
#include <trantor/utils/Logger.h>

#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace cerp::core {

using infrastructure::ValidationError;

std::unique_ptr<IrSequence> IrSequence::s_instance_;

IrSequence::IrSequence(std::shared_ptr<infrastructure::DbConnection> db)
    : db_(std::move(db)) {}

void IrSequence::initialize(std::shared_ptr<infrastructure::DbConnection> db) {
    if (!s_instance_) s_instance_.reset(new IrSequence(std::move(db)));
}

IrSequence& IrSequence::instance() {
    if (!s_instance_)
        throw std::runtime_error("IrSequence::instance() before initialize()");
    return *s_instance_;
}

bool IrSequence::ready() { return s_instance_ != nullptr; }


// ── helpers ───────────────────────────────────────────────────

std::string IrSequence::periodKey_(const std::string& resetPolicy) {
    if (resetPolicy != "yearly" && resetPolicy != "monthly") return "";
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf,
                  resetPolicy == "yearly" ? "%Y" : "%Y-%m", &tm);
    return buf;
}

std::string IrSequence::format_(const std::string& prefix,
                                const std::string& suffix,
                                long long number, int padding) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char y4[8], y2[4], mo[4], dy[4];
    std::strftime(y4, sizeof y4, "%Y", &tm);
    std::strftime(y2, sizeof y2, "%y", &tm);
    std::strftime(mo, sizeof mo, "%m", &tm);
    std::strftime(dy, sizeof dy, "%d", &tm);

    auto expand = [&](std::string s) {
        const std::pair<const char*, const char*> subs[] = {
            {"%(year)s",  y4}, {"%(y)s", y2}, {"%(month)s", mo}, {"%(day)s", dy},
        };
        for (const auto& [tok, val] : subs) {
            std::string::size_type p;
            while ((p = s.find(tok)) != std::string::npos)
                s.replace(p, std::strlen(tok), val);
        }
        return s;
    };

    std::ostringstream num;
    if (padding > 0) num << std::setfill('0') << std::setw(padding);
    num << number;

    return expand(prefix) + num.str() + expand(suffix);
}


// ── core allocation ───────────────────────────────────────────

std::string IrSequence::next_(pqxx::transaction_base& txn, const std::string& code,
                              int companyId, bool consume) {
    // FOR UPDATE is the whole mechanism: it serialises concurrent allocators
    // on this row, which is what makes the series gapless and duplicate-free.
    // company_id IS NOT DISTINCT FROM $2 so a NULL (company-agnostic) sequence
    // matches a companyId of 0 without a second query.
    pqxx::params p;
    p.append(code);
    if (companyId > 0) p.append(companyId); else p.append(nullptr);

    auto rows = txn.exec(
        "SELECT id, prefix, suffix, padding, number_next, number_increment, "
        "       reset_policy, last_reset_period "
        "  FROM ir_sequence "
        " WHERE code = $1 "
        "   AND company_id IS NOT DISTINCT FROM $2 "
        "   AND active "
        " FOR UPDATE",
        p);

    if (rows.empty()) {
        // Fall back to the company-agnostic sequence before giving up.
        rows = txn.exec(
            "SELECT id, prefix, suffix, padding, number_next, number_increment, "
            "       reset_policy, last_reset_period "
            "  FROM ir_sequence WHERE code = $1 AND company_id IS NULL AND active "
            " FOR UPDATE",
            pqxx::params{code});
    }
    if (rows.empty())
        throw ValidationError("No active sequence configured for '" + code + "'.");

    const auto& r        = rows[0];
    const int   id       = r["id"].as<int>();
    std::string prefix   = r["prefix"].is_null() ? "" : r["prefix"].c_str();
    std::string suffix   = r["suffix"].is_null() ? "" : r["suffix"].c_str();
    const int   padding  = r["padding"].as<int>(5);
    long long   nextNum  = r["number_next"].as<long long>(1);
    const int   step     = r["number_increment"].as<int>(1);
    const std::string policy   = r["reset_policy"].is_null() ? "never" : r["reset_policy"].c_str();
    const std::string lastPeriod = r["last_reset_period"].is_null() ? "" : r["last_reset_period"].c_str();

    // Period rollover: a yearly sequence restarts at 1 on 1 January. Detected
    // by comparing the stored period key rather than by a scheduled job, so it
    // cannot be missed if the server was down at midnight.
    const std::string period = periodKey_(policy);
    const bool rolled = !period.empty() && period != lastPeriod;
    if (rolled) nextNum = 1;

    const std::string value = format_(prefix, suffix, nextNum, padding);

    if (consume) {
        txn.exec(
            "UPDATE ir_sequence "
            "   SET number_next = $1, last_reset_period = $2, write_date = now() "
            " WHERE id = $3",
            pqxx::params{nextNum + step, period, id});
    }
    return value;
}

std::string IrSequence::nextByCode(pqxx::transaction_base& txn, const std::string& code,
                                   int companyId) {
    return next_(txn, code, companyId, /*consume=*/true);
}

std::string IrSequence::nextByCode(const std::string& code, int companyId) {
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};
    const std::string v = next_(txn, code, companyId, true);
    txn.commit();
    return v;
}

std::optional<std::string> IrSequence::peek(const std::string& code, int companyId) {
    try {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        const std::string v = next_(txn, code, companyId, /*consume=*/false);
        txn.commit();
        return v;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool IrSequence::has(const std::string& code, int companyId) {
    try {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto rows = txn.exec(
            "SELECT 1 FROM ir_sequence "
            " WHERE code = $1 AND active "
            "   AND (company_id IS NULL OR company_id = $2) LIMIT 1",
            pqxx::params{code, companyId});
        txn.commit();
        return !rows.empty();
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace cerp::core

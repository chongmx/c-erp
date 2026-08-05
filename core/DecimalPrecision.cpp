// ============================================================
// core/DecimalPrecision.cpp — see DecimalPrecision.hpp
// ============================================================
#include "DecimalPrecision.hpp"
#include "infrastructure/DbConnection.hpp"

#include <pqxx/pqxx>
#include <trantor/utils/Logger.h>

namespace odoo::core {

std::once_flag                           DecimalPrecision::s_once_;
std::unique_ptr<DecimalPrecision>        DecimalPrecision::s_instance_;

DecimalPrecision::DecimalPrecision(std::shared_ptr<infrastructure::DbConnection> db)
    : db_(std::move(db)) {}

void DecimalPrecision::initialize(std::shared_ptr<infrastructure::DbConnection> db) {
    std::call_once(s_once_, [&] {
        s_instance_.reset(new DecimalPrecision(std::move(db)));
    });
}

DecimalPrecision& DecimalPrecision::instance() {
    if (!s_instance_)
        throw std::runtime_error("DecimalPrecision::instance() before initialize()");
    return *s_instance_;
}

bool DecimalPrecision::ready() { return s_instance_ != nullptr; }

void DecimalPrecision::invalidate() {
    std::lock_guard lk{mutex_};
    loaded_ = false;
    values_.clear();
}

void DecimalPrecision::loadIfNeeded_() const {
    if (loaded_) return;
    try {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto rows = txn.exec("SELECT name, digits FROM decimal_precision");
        txn.commit();

        values_.clear();
        values_.reserve(rows.size());
        for (const auto& r : rows)
            values_.emplace_back(r[0].c_str(), r[1].as<int>(2));
        loaded_ = true;
    } catch (const std::exception& ex) {
        // A formatting lookup must never fail a request. Log once and let
        // callers fall back to their default; the next call retries.
        LOG_ERROR << "[precision] load failed, using fallbacks: " << ex.what();
    }
}

int DecimalPrecision::digits(const std::string& name, int fallback) const {
    std::lock_guard lk{mutex_};
    loadIfNeeded_();
    for (const auto& [n, d] : values_)
        if (n == name) return d;
    return fallback;
}

} // namespace odoo::core

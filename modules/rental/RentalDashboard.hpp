#pragma once
// =============================================================
// modules/rental/RentalDashboard.hpp — one endpoint, one payload
//
// GET /rental/dashboard returns everything the dashboard draws, from a
// handful of aggregate queries, cached 60 s.
//
// Deliberately NOT assembled from a dozen search_read calls in the
// browser (docs/040 §3.4). That is the fastest route to a four-second
// paint that hammers the connection pool, and it makes every panel a
// separate failure.
//
// The cashflow series comes from RentalForecast, so the dashboard and
// the standalone /rental/cashflow endpoint can never disagree — there is
// one implementation of the projection.
// =============================================================
#include <memory>

#include <nlohmann/json.hpp>

namespace cerp::infrastructure { class DbConnection; }

namespace cerp::modules::rental {

class RentalDashboard {
public:
    /**
     * @param months forecast horizon for the cashflow panel
     * @param fresh  bypass the cache (the Refresh button)
     */
    static nlohmann::json build(std::shared_ptr<infrastructure::DbConnection> db,
                                int  months = 12,
                                bool fresh  = false);
};

} // namespace cerp::modules::rental

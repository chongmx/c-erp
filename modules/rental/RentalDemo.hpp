#pragma once
// =============================================================
// modules/rental/RentalDemo.hpp — demo facility, create and remove
//
// The SINGLE implementation. scripts/seed_rental_demo.sh calls these
// endpoints rather than carrying its own copy of the SQL: two versions
// of "what the demo data is" would drift the first time a column
// changed, and the shell copy is the one nobody would remember to
// update.
//
// SAFETY. clear() is destructive, so what counts as demo data is defined
// once, narrowly, and identically for both operations:
//
//   units      site = 'Demo Warehouse'
//   contracts  name LIKE 'DEMO/%'
//   expenses   name IN (a fixed list)
//
// Nothing else is ever touched. status() reports the exact counts before
// anything is removed, so the UI can show what is about to go and the
// operator is never guessing.
// =============================================================
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace odoo::infrastructure { class DbConnection; }

namespace odoo::modules::rental {

class RentalDemo {
public:
    /// How much demo data exists right now, per table.
    static nlohmann::json status(std::shared_ptr<infrastructure::DbConnection> db);

    /// Create the demo facility. Idempotent — re-running adds nothing.
    static nlohmann::json seed(std::shared_ptr<infrastructure::DbConnection> db);

    /// Remove it. Returns what was actually deleted.
    static nlohmann::json clear(std::shared_ptr<infrastructure::DbConnection> db);
};

} // namespace odoo::modules::rental

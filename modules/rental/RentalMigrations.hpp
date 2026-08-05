#pragma once
// ============================================================
// modules/rental/RentalMigrations.hpp
//
// Schema for the rental module (docs/054 §2 phase 1).
//
// Version range 800–899. Note that this is numerically BELOW the P2–P7
// migrations (900–1010) which are already applied in production. That is
// safe here and was verified rather than assumed: MigrationRunner builds
// a std::set<int> of applied versions and skips by set membership
// (core/infrastructure/MigrationRunner.cpp:36-57), so a lower version
// registered later still runs. Under a "current version" high-water mark
// it would silently never run, and the rental tables would simply not
// exist.
//
//   800  rental_unit_type
//   801  rental_unit
//   802  rental_contract
//   803  rental_contract_line
//   804  rental_invoice_link      <- carries the anti-double-billing UNIQUE
//   805  rental_expense_category
//   806  rental_expense
//   807  rental_event
//   808  seed: unit types + expense categories
//   809  ir_sequence: contract numbering
//   810  ir_cron: daily billing + recurring expense generation
//
// MONEY COLUMNS are BIGINT micro-units throughout (P2). Never NUMERIC.
// ============================================================
#include <string>

namespace odoo::infrastructure { class MigrationRunner; }

namespace odoo::modules::rental {

/// Register every rental migration on the runner.
void registerRentalMigrations(odoo::infrastructure::MigrationRunner& runner);

} // namespace odoo::modules::rental

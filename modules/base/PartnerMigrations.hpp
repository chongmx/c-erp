#pragma once
// ============================================================
// modules/base/PartnerMigrations.hpp — the partner hierarchy (docs/130)
// ============================================================
namespace cerp::infrastructure { class MigrationRunner; }

namespace cerp::modules::base {

/// Register the res.partner hierarchy migrations (versions 10-12).
/// Range 1-99 is reserved for core/base — see MigrationRunner.hpp.
void registerPartnerMigrations(cerp::infrastructure::MigrationRunner& runner);

} // namespace cerp::modules::base

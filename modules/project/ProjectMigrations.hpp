#pragma once
// ============================================================
// modules/project/ProjectMigrations.hpp
//
// The issue tracker on top of project.task (docs/architecture/modules.md,
// "project"). The base tables are still created by ProjectModule::
// ensureSchema_(); these migrations run after it and add what turns a task
// into a ticket.
//
// Version range 1100–1199.
//
//   1100  ticket keys (CERP-12), issue type, reporter, priority levels
//   1101  labels and watchers
// ============================================================

namespace cerp::infrastructure { class MigrationRunner; }

namespace cerp::modules::project {

void registerProjectMigrations(cerp::infrastructure::MigrationRunner& runner);

} // namespace cerp::modules::project

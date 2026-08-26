#pragma once
// =============================================================
// modules/project/ProjectModule.hpp
//
// docs/100 — Projects, tasks and timesheets.
//
// Models:
//   project.project    (project_project)    — a project
//   project.task.type  (project_task_type)  — a kanban stage
//   project.task       (project_task)       — a task
//   project.timesheet  (project_timesheet)  — hours logged against a task
//
// ViewModels:
//   ProjectViewModel        — CRUD + stats
//   ProjectTaskTypeViewModel— CRUD
//   ProjectTaskViewModel    — CRUD + board + move_stage
//   ProjectTimesheetViewModel — CRUD + grid + set_cell + summary
//
// Menus:
//   id=130 Project app tile
//   id=137 Task Board, 138 Projects, 139 Tasks,
//   id=140 Timesheets, 141 Timesheet Entries, 142 Stages
//   (131/132 belong to ReportModule — see verify_menu_ids.sh)
//
// Actions:
//   id=108 Task Board, 109 Projects, 110 Tasks,
//   id=111 Timesheets, 112 Timesheet Entries, 113 Stages
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::project {

class ProjectModule : public core::IModule {
public:
    explicit ProjectModule(core::ModelFactory&     models,
                           core::ServiceFactory&   services,
                           core::ViewModelFactory& viewModels,
                           core::ViewFactory&      views);

    static constexpr const char* staticModuleName() { return "project"; }
    std::string              moduleName()   const override;
    std::string              version()      const override;
    std::vector<std::string> dependencies() const override;

    void registerModels()     override;
    void registerServices()   override;
    void registerViewModels() override;
    void registerViews()      override;
    void registerRoutes()     override;
    void initialize()         override;

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    void ensureSchema_();
    void seedStages_();    ///< the default stage vocabulary
    void seedMenus_();
};

} // namespace odoo::modules::project

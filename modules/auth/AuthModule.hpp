#pragma once
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::auth {

// ================================================================
// AuthModule
// ================================================================
/**
 * @brief Registers res.users, res.groups, res.company and AuthService.
 *
 * Also creates the required tables and seeds default groups if they
 * do not yet exist (idempotent — safe to call on every boot).
 *
 * Depends on: "base"   (res.partner must exist first)
 *
 * Usage in main():
 * @code
 *   container->addModule<odoo::modules::auth::AuthModule>();
 * @endcode
 */
class AuthModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "auth"; }

    explicit AuthModule(core::ModelFactory&     modelFactory,
                        core::ServiceFactory&   serviceFactory,
                        core::ViewModelFactory& viewModelFactory,
                        core::ViewFactory&      viewFactory);

    std::string              moduleName()   const override;
    std::string              version()      const override;
    std::vector<std::string> dependencies() const override;

    void registerModels()     override;
    void registerServices()   override;
    void registerViews()      override;
    void registerViewModels() override;
    void registerRoutes()     override;
    void initialize()         override;

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    // Inner classes defined in AuthModule.cpp
    class CompanyViewModel;
    class GroupsViewModel;

    void ensureSchema_();
    void seedGroups_();
    void seedGroupPermissions_();
    void seedAdminUser_();
};

} // namespace odoo::modules::auth

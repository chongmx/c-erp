#pragma once
// =============================================================
// modules/auth/AuthSignupModule.hpp
//
// Phase 14 — auth_signup
//
// Adds self-service registration and password reset endpoints:
//   POST /web/signup          — create a new user account
//   POST /web/reset_password  — initiate/complete password reset
//
// Both endpoints check the ir_config_parameter table for
//   auth_signup.allow       = True/False
//   auth_signup.reset_pwd   = True/False
//
// No email is sent (stub) — token is returned in the response
// so the frontend can complete the reset flow without SMTP.
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::auth {

// ================================================================
// AuthSignupModule
// ================================================================
class AuthSignupModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "auth_signup"; }

    explicit AuthSignupModule(core::ModelFactory&     modelFactory,
                              core::ServiceFactory&   serviceFactory,
                              core::ViewModelFactory& viewModelFactory,
                              core::ViewFactory&      viewFactory);

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
    std::shared_ptr<infrastructure::DbConnection> db_;
    bool devMode_ = false;

    void createUser_(const std::string& login,
                     const std::string& password,
                     const std::string& name);

    void storeResetToken_(const std::string& login, const std::string& token);

    void completeReset_(const std::string& login,
                        const std::string& token,
                        const std::string& newPassword);

    bool configBool_(const std::string& key);

    static std::string generateToken_();
};

} // namespace odoo::modules::auth

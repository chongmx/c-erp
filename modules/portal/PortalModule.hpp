#pragma once
// =============================================================
// modules/portal/PortalModule.hpp  — declaration only
//
// Customer-facing portal for the storage rental ERP.
//
// Features:
//   - Separate portal session management (cookie: portal_sid, 8h TTL)
//   - Per-IP login rate limiting (10 attempts / 5-minute window)
//   - PBKDF2-SHA512 password hashing (same format as AuthService)
//   - PortalPartnerViewModel ("portal.partner") for admin use via JSON-RPC
//   - 10 HTTP routes under /portal/...
//   - DB schema migrations (idempotent)
//   - File upload directory: data/payment_proofs/
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include "DbConnection.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::portal {

using namespace odoo::core;
using namespace odoo::infrastructure;

// Forward declarations for types used in private members
class PortalSessionManager;
class PortalLoginRateLimiter;

// ================================================================
// PortalModule
// ================================================================

/**
 * @brief Customer-facing portal module.
 *
 * Registers:
 *   - PortalPartnerViewModel as "portal.partner"
 *   - 10 HTTP routes under /portal/...
 *   - DB schema (idempotent ALTER TABLE / CREATE TABLE)
 *   - Upload directory data/payment_proofs/
 */
class PortalModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "portal"; }

    explicit PortalModule(core::ModelFactory&     modelFactory,
                          core::ServiceFactory&   serviceFactory,
                          core::ViewModelFactory& viewModelFactory,
                          core::ViewFactory&      viewFactory);

    std::string              moduleName()   const override;
    std::string              version()      const override;
    std::vector<std::string> dependencies() const override;

    void registerModels()      override;
    void registerServices()    override;
    void registerViews()       override;
    void registerViewModels()  override;
    void registerRoutes()      override;
    void initialize()          override;

private:
    std::shared_ptr<infrastructure::DbConnection> db_;
    core::ViewModelFactory&                       viewModels_;
    std::shared_ptr<PortalSessionManager>         portalSessions_;
    std::shared_ptr<PortalLoginRateLimiter>       rateLimiter_;
    bool                                          devMode_       = false;
    bool                                          secureCookies_ = false;

    void ensureSchema_();
    void ensureUploadDir_();
    void seedMenu_();
};

} // namespace odoo::modules::portal

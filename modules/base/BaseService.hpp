#pragma once
#include "interfaces/IService.hpp"
#include "infrastructure/DbConnection.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

namespace odoo::core {

// ============================================================
// BaseService
// ============================================================
/**
 * @brief Convenience base for business-logic services.
 *
 * Holds a shared DbConnection and exposes it to derived classes.
 * Implements the IService lifecycle hooks as no-ops so concrete
 * services only override what they need.
 *
 * Derived services must implement serviceName() — everything else
 * is optional.
 *
 * @code
 *   class PartnerService : public BaseService {
 *   public:
 *       explicit PartnerService(std::shared_ptr<infrastructure::DbConnection> db)
 *           : BaseService(db) {}
 *
 *       std::string serviceName() const override { return "partner"; }
 *
 *       std::shared_ptr<ResPartner> findByEmail(const std::string& email) { ... }
 *   };
 * @endcode
 */
class BaseService : public IService {
public:
    explicit BaseService(std::shared_ptr<infrastructure::DbConnection> db)
        : db_(std::move(db)) {}

    // IService lifecycle — default no-ops; override as needed
    void initialize() override {}
    void shutdown()   override {}

    nlohmann::json healthCheck() const override {
        return {{"service", serviceName()}, {"status", "ok"}};
    }

protected:
    std::shared_ptr<infrastructure::DbConnection> db_;
};

} // namespace odoo::core
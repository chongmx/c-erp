#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Forward declaration — keeps pqxx out of this header
namespace odoo::infrastructure { class MigrationRunner; }

namespace odoo::core {

// ============================================================
// IModule
// ============================================================
/**
 * @brief Interface every business module must satisfy.
 *
 * A module is a self-contained unit of business functionality that wires
 * its models, services, view-models, views, and HTTP routes into the
 * shared factories and router during application boot.
 *
 * Boot sequence (called by ModuleFactory::bootAll() in order):
 *   1. registerModels()     — populate ModelFactory
 *   2. registerServices()   — populate ServiceFactory
 *   3. registerViewModels() — populate ViewModelFactory
 *   4. registerViews()      — populate ViewFactory
 *   5. registerRoutes()     — add HTTP routes to HttpService
 *
 * After all modules have booted, IService::initialize() is called on every
 * registered service so cross-module wiring can complete safely.
 *
 * Modules are always Singletons — constructed and booted exactly once.
 *
 * Dependency declaration:
 *   Override dependencies() to return the names of modules that must be
 *   booted before this one. ModuleFactory::bootAll() will topological-sort
 *   the boot order accordingly.
 *
 * Example — BaseModule:
 * @code
 *   class BaseModule : public IModule {
 *   public:
 *       BaseModule(ModelFactory& mf, ServiceFactory& sf,
 *                  ViewModelFactory& vmf, ViewFactory& vf)
 *           : mf_(mf), sf_(sf), vmf_(vmf), vf_(vf) {}
 *
 *       std::string moduleName() const override { return "base"; }
 *
 *       void registerModels() override {
 *           mf_.registerModel<ResPartner>("res.partner");
 *           mf_.registerModel<ResUsers>("res.users");
 *       }
 *       // ...
 *   };
 * @endcode
 */
class IModule {
public:
    virtual ~IModule() = default;

    // ----------------------------------------------------------
    // Identity
    // ----------------------------------------------------------

    /**
     * @brief Technical module name, e.g. "base", "account", "sale".
     * Must be unique across all registered modules.
     * Must match the key used at ModuleFactory registration.
     */
    virtual std::string moduleName() const = 0;

    /**
     * @brief Human-readable display name, e.g. "Base", "Accounting".
     * Default: same as moduleName().
     */
    virtual std::string displayName() const { return moduleName(); }

    /**
     * @brief Semantic version string, e.g. "17.0.1.0.0".
     * Format follows Odoo convention: <odoo_major>.<odoo_minor>.<major>.<minor>.<patch>
     */
    virtual std::string version() const { return "17.0.1.0.0"; }

    // ----------------------------------------------------------
    // Dependency graph
    // ----------------------------------------------------------

    /**
     * @brief Module names that must be booted before this module.
     *
     * ModuleFactory::bootAll() performs a topological sort using this list.
     * Circular dependencies cause a std::runtime_error at boot time.
     *
     * Default: no dependencies (suitable for "base").
     */
    virtual std::vector<std::string> dependencies() const { return {}; }

    // ----------------------------------------------------------
    // Boot sequence
    // ----------------------------------------------------------

    /**
     * @brief Register ORM model creators with ModelFactory.
     *
     * Called first so services can rely on models being available.
     * Typical body:
     * @code
     *   modelFactory_.registerModel<ResPartner>("res.partner");
     * @endcode
     */
    virtual void registerModels() = 0;

    /**
     * @brief Register service creators with ServiceFactory.
     *
     * Called after registerModels() so services can receive model instances.
     * Typical body:
     * @code
     *   serviceFactory_.registerService<AuthService>("auth");
     * @endcode
     */
    virtual void registerServices() = 0;

    /**
     * @brief Register ViewModel creators with ViewModelFactory.
     *
     * Called after registerServices() so view-models can capture service
     * Singleton pointers at construction time.
     * Typical body:
     * @code
     *   viewModelFactory_.registerViewModel<PartnerViewModel>(
     *       "res.partner",
     *       [&]{ return std::make_shared<PartnerViewModel>(
     *                partnerService_, viewFactory_); });
     * @endcode
     */
    virtual void registerViewModels() = 0;

    /**
     * @brief Register view serializers with ViewFactory.
     *
     * Called after registerViewModels(). Views are stateless singletons;
     * construction is trivial.
     * Typical body:
     * @code
     *   viewFactory_.registerView<PartnerFormView>("res.partner.form");
     *   viewFactory_.registerView<PartnerListView>("res.partner.list");
     * @endcode
     */
    virtual void registerViews() = 0;

    /**
     * @brief Register HTTP routes with the application router.
     *
     * Called last so all components are already available.
     * Most modules have no custom routes (JSON-RPC is handled centrally by
     * JsonRpcDispatcher). Override only for module-specific REST endpoints,
     * file download handlers, etc.
     *
     * Default: no-op.
     */
    virtual void registerRoutes() {}

    /**
     * @brief Register SQL migrations with the MigrationRunner.
     *
     * Called by Container::runMigrations_() after all modules have
     * finished their register*() sequence but BEFORE initialize().
     * Use for any DDL that must be versioned and tracked (column renames,
     * type changes, new indexes, data migrations).
     *
     * Version numbering ranges (globally unique integers):
     *   1-99    core / base          500-599 mrp
     *   100-199 account              600-699 portal / report
     *   200-299 sale                 700-799 auth / auth_signup
     *   300-399 purchase
     *   400-499 stock
     *
     * Example:
     * @code
     *   void SaleModule::registerMigrations(MigrationRunner& r) {
     *       r.registerMigration({200, "add_sale_note_column",
     *           "ALTER TABLE sale_order ADD COLUMN IF NOT EXISTS note TEXT"});
     *   }
     * @endcode
     *
     * Default: no-op (module has no versioned migrations).
     */
    virtual void registerMigrations(odoo::infrastructure::MigrationRunner& /*runner*/) {}

    /**
     * @brief Post-boot initialization hook.
     *
     * Called by Container::initializeModules_() after ALL modules have
     * finished their register*() sequence and after all migrations have
     * been applied. Use for DDL (CREATE TABLE IF NOT EXISTS),
     * data seeding, and cross-module wiring that requires other modules
     * to already be registered.
     *
     * Execution order matches registration order, so a module that
     * depends on "base" can safely reference res_partner here.
     *
     * Default: no-op.
     */
    virtual void initialize() {}

    // ----------------------------------------------------------
    // Metadata / manifest
    // ----------------------------------------------------------

    /**
     * @brief Return a JSON manifest describing this module.
     *
     * Mirrors Odoo's __manifest__.py structure. Used by admin UIs and
     * diagnostic endpoints. Default implementation builds from the virtual
     * accessors above; concrete modules may override to add author, website,
     * icon, category, etc.
     *
     * @returns JSON object compatible with Odoo's module manifest format.
     */
    virtual nlohmann::json manifest() const {
        return {
            {"name",         moduleName()},
            {"summary",      displayName()},
            {"version",      version()},
            {"depends",      dependencies()},
            {"installable",  true},
            {"auto_install", false},
        };
    }
};

} // namespace odoo::core
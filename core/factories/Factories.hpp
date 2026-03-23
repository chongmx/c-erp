#pragma once
#include "BaseFactory.hpp"
#include "interfaces/IModel.hpp"
#include "interfaces/IService.hpp"
#include "interfaces/IViewModel.hpp"
#include "interfaces/IView.hpp"
#include "interfaces/IModule.hpp"
#include <memory>
#include <string>
#include <vector>

// Forward declaration — avoids pulling in libpqxx headers everywhere.
namespace odoo::infrastructure { class DbConnection; }

namespace odoo::core {

// ============================================================
// ModelFactory
// ============================================================
/**
 * @brief Creates ORM model instances keyed by Odoo model name.
 *
 * Keys follow Odoo dot-notation: "res.partner", "account.move", etc.
 * Models are Transient by default — each call returns a fresh blank record.
 * Singleton lifetime is available for read-only prototype/meta uses.
 *
 * Registration (inside IModule::registerModels()):
 * @code
 *   modelFactory.registerCreator("res.partner",
 *       [db]{ return std::make_shared<ResPartner>(db); });
 * @endcode
 *
 * Usage:
 * @code
 *   auto partner = modelFactory.create("res.partner");                    // Transient
 *   auto proto   = modelFactory.create("res.partner", Lifetime::Singleton); // cached
 * @endcode
 */
class ModelFactory : public BaseFactory<IModel> {
public:
    explicit ModelFactory(std::shared_ptr<infrastructure::DbConnection> db)
        : db_(std::move(db)) {}

    using BaseFactory<IModel>::registerCreator;
    using BaseFactory<IModel>::registerSingleton;
    using BaseFactory<IModel>::overrideCreator;

    /**
     * @brief Convenience: register a model type that takes a DbConnection.
     *
     * @tparam TModel  Concrete model type. Must be constructible as TModel(db).
     * @param  key     Odoo model name, e.g. "res.partner".
     */
    template<typename TModel>
    void registerModel(const std::string& key) {
        auto db = db_;
        registerCreator(key, [db] {
            return std::make_shared<TModel>(db);
        });
    }

    std::shared_ptr<infrastructure::DbConnection> db() const { return db_; }

private:
    std::shared_ptr<infrastructure::DbConnection> db_;
};


// ============================================================
// ServiceFactory
// ============================================================
/**
 * @brief Creates business-logic service instances.
 *
 * Services are almost always Singletons — stateless, shared across requests.
 * Transient services are useful in integration tests that need isolation.
 *
 * Keys are short service names: "auth", "partner", "accounting".
 *
 * Registration (inside IModule::registerServices()):
 * @code
 *   serviceFactory.registerCreator("auth",
 *       [db]{ return std::make_shared<AuthService>(db); });
 * @endcode
 *
 * Usage:
 * @code
 *   auto auth = serviceFactory.create("auth", Lifetime::Singleton);
 * @endcode
 */
class ServiceFactory : public BaseFactory<IService> {
public:
    explicit ServiceFactory(std::shared_ptr<infrastructure::DbConnection> db,
                            bool devMode       = false,
                            bool secureCookies = false)
        : db_(std::move(db))
        , devMode_(devMode)
        , secureCookies_(secureCookies)
    {}

    using BaseFactory<IService>::registerCreator;
    using BaseFactory<IService>::registerSingleton;
    using BaseFactory<IService>::overrideCreator;

    /**
     * @brief Convenience: register a service type that takes a DbConnection.
     *
     * @tparam TService  Concrete service type. Must be constructible as TService(db).
     * @param  key       Short service name, e.g. "auth".
     * @param  lifetime  Default lookup lifetime (usually Singleton).
     */
    template<typename TService>
    void registerService(const std::string& key,
                         Lifetime           defaultLifetime = Lifetime::Singleton) {
        auto db = db_;
        registerCreator(key, [db] {
            return std::make_shared<TService>(db);
        });
        // Pre-warm the singleton cache if requested.
        if (defaultLifetime == Lifetime::Singleton) {
            create(key, Lifetime::Singleton);
        }
    }

    std::shared_ptr<infrastructure::DbConnection> db()            const { return db_; }
    bool                                          devMode()       const { return devMode_; }
    bool                                          secureCookies() const { return secureCookies_; }

private:
    std::shared_ptr<infrastructure::DbConnection> db_;
    bool devMode_       = false;
    bool secureCookies_ = false;
};


// ============================================================
// ViewModelFactory
// ============================================================
/**
 * @brief Creates ViewModel instances keyed by Odoo model name.
 *
 * The JSON-RPC dispatcher resolves: model="res.partner" → PartnerViewModel.
 * ViewModels are Transient by default (one per request, no cross-request state).
 * Use Singleton only for read-only ViewModels that hold no request context.
 *
 * Registration (inside IModule::registerViewModels()):
 * @code
 *   viewModelFactory.registerCreator("res.partner",
 *       [&svcFactory]{ return std::make_shared<PartnerViewModel>(
 *           svcFactory.create("partner", Lifetime::Singleton)); });
 * @endcode
 *
 * Usage (inside JsonRpcDispatcher):
 * @code
 *   auto vm = viewModelFactory.create(model, Lifetime::Transient);
 *   return vm->callKw(method, args, kwargs);
 * @endcode
 */
class ViewModelFactory : public BaseFactory<IViewModel> {
public:
    ViewModelFactory() = default;

    using BaseFactory<IViewModel>::registerCreator;
    using BaseFactory<IViewModel>::registerSingleton;
    using BaseFactory<IViewModel>::overrideCreator;

    /**
     * @brief Convenience typed registration.
     *
     * @tparam TViewModel  Concrete type. Must be default-constructible or
     *                     constructed inside the supplied creator lambda.
     * @param  modelName   Odoo model name, e.g. "res.partner".
     * @param  creator     Lambda returning std::shared_ptr<TViewModel>.
     */
    template<typename TViewModel>
    void registerViewModel(const std::string&                          modelName,
                           std::function<std::shared_ptr<TViewModel>()> creator) {
        registerCreator(modelName, [creator = std::move(creator)]()
                                       -> std::shared_ptr<IViewModel> {
            return creator();
        });
    }
};


// ============================================================
// ViewFactory
// ============================================================
/**
 * @brief Creates JSON view serializers keyed by "<model>.<viewType>".
 *
 * Views produce JSON that describes field layout and record data for the
 * OWL/JS frontend. They operate entirely on nlohmann::json — there is no
 * templated IView<TModel>, so no type erasure layer is needed.
 *
 * Key format: "<model_name>.<view_type>"
 *   e.g. "res.partner.form", "res.partner.list", "account.move.kanban"
 *
 * Views are always Singletons — they are stateless shape descriptors.
 *
 * Registration (inside IModule::registerViews()):
 * @code
 *   viewFactory.registerCreator("res.partner.form",
 *       []{ return std::make_shared<PartnerFormView>(); });
 *   viewFactory.registerCreator("res.partner.list",
 *       []{ return std::make_shared<PartnerListView>(); });
 * @endcode
 *
 * Usage (inside ViewModel or JsonRpcDispatcher):
 * @code
 *   auto view = viewFactory.getView("res.partner", "list");
 *   return view->renderList(records);
 * @endcode
 */
class ViewFactory : public BaseFactory<IView> {
public:
    ViewFactory() = default;

    using BaseFactory<IView>::registerCreator;
    using BaseFactory<IView>::registerSingleton;
    using BaseFactory<IView>::overrideCreator;

    /**
     * @brief Convenience typed registration — always Singleton.
     *
     * @tparam TView     Concrete view type deriving from IView.
     * @param  key       "<model>.<viewType>", e.g. "res.partner.form".
     * @param  creator   Lambda returning std::shared_ptr<TView> (or default-constructs).
     */
    template<typename TView>
    void registerView(const std::string&                       key,
                      std::function<std::shared_ptr<TView>()>  creator = nullptr) {
        if (creator) {
            registerCreator(key, [c = std::move(creator)]() -> std::shared_ptr<IView> {
                return c();
            });
        } else {
            registerCreator(key, []() -> std::shared_ptr<IView> {
                return std::make_shared<TView>();
            });
        }
    }

    /**
     * @brief Resolve a view by model name + view type, with "form" fallback.
     *
     * @param modelName  Odoo model name, e.g. "res.partner".
     * @param viewType   "form" | "list" | "kanban" | "tree" | ... (default "form").
     * @returns          Shared singleton view instance.
     * @throws std::runtime_error if neither key nor form fallback is registered.
     */
    std::shared_ptr<IView> getView(const std::string& modelName,
                                   const std::string& viewType = "form") {
        const std::string key      = modelName + "." + viewType;
        const std::string fallback = modelName + ".form";

        if (has(key))      return create(key,      Lifetime::Singleton);
        if (has(fallback)) return create(fallback, Lifetime::Singleton);

        throw std::runtime_error(
            "ViewFactory: no view registered for '" + key +
            "' and no form fallback for '" + modelName + "'");
    }

    /**
     * @brief Check whether a specific view is registered (without throwing).
     */
    bool hasView(const std::string& modelName,
                 const std::string& viewType = "form") const {
        return has(modelName + "." + viewType);
    }
};


// ============================================================
// ModuleFactory
// ============================================================
/**
 * @brief Boots business modules in dependency order.
 *
 * Modules are always Singletons — constructed and booted once at startup.
 * Each module's register* methods wire their components into the shared
 * factories held by Container.
 *
 * Registration (in main() / Application::boot()):
 * @code
 *   moduleFactory.registerCreator("base",
 *       [&]{ return std::make_shared<BaseModule>(modelFactory,
 *                                                serviceFactory,
 *                                                viewModelFactory,
 *                                                viewFactory); });
 * @endcode
 *
 * Boot sequence:
 * @code
 *   moduleFactory.bootAll();
 * @endcode
 */
class ModuleFactory : public BaseFactory<IModule> {
public:
    ModuleFactory() = default;

    using BaseFactory<IModule>::registerCreator;
    using BaseFactory<IModule>::registerSingleton;

    /**
     * @brief Boot all registered modules.
     *
     * Calls the full register* sequence on each module in registration order.
     * TODO: topological sort by IModule::dependencies() for multi-module graphs.
     *
     * @throws std::runtime_error propagated from any module's register* call.
     */
    void bootAll() {
        for (const auto& name : registeredNames()) {
            auto mod = create(name, Lifetime::Singleton);
            mod->registerModels();
            mod->registerServices();
            mod->registerViewModels();
            mod->registerViews();
            mod->registerRoutes();
        }
    }

    /**
     * @brief Boot a single named module (useful for incremental hot-load).
     * @throws std::runtime_error if name not registered.
     */
    void bootOne(const std::string& name) {
        auto mod = create(name, Lifetime::Singleton);
        mod->registerModels();
        mod->registerServices();
        mod->registerViewModels();
        mod->registerViews();
        mod->registerRoutes();
    }
};

} // namespace odoo::core
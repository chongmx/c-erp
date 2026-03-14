#pragma once
#include "interfaces/IViewModel.hpp"
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace odoo::core {

// ============================================================
// REGISTER_METHOD macro
// ============================================================
/**
 * @brief Register a ViewModel handler method in the dispatch table.
 *
 * Must be called inside the derived class constructor, after the
 * BaseViewModel constructor runs.
 *
 * Handler signature:
 * @code
 *   nlohmann::json handleSearchRead(const CallKwArgs& call);
 * @endcode
 *
 * Usage:
 * @code
 *   PartnerViewModel(...) : ... {
 *       REGISTER_METHOD("search_read", handleSearchRead)
 *       REGISTER_METHOD("read",        handleRead)
 *       REGISTER_METHOD("create",      handleCreate)
 *   }
 * @endcode
 *
 * The macro captures `this` and binds the member function pointer so
 * the dispatch table stores a plain `std::function<json(const CallKwArgs&)>`.
 */
#define REGISTER_METHOD(method_name, handler)                              \
    registerMethod_(method_name,                                           \
        [this](const ::odoo::core::CallKwArgs& call) -> nlohmann::json {  \
            return this->handler(call);                                    \
        });


// ============================================================
// BaseViewModel
// ============================================================
/**
 * @brief Convenience base providing a string-keyed method dispatch table.
 *
 * Concrete ViewModels:
 *   1. Inherit from BaseViewModel
 *   2. Implement modelName() — must match ViewModelFactory key
 *   3. Call REGISTER_METHOD("method_name", handlerMethod) in constructor
 *   4. Implement each handler as:
 *      @code
 *        nlohmann::json handleSearchRead(const CallKwArgs& call) { ... }
 *      @endcode
 *
 * callKw() dispatches to the registered handler or throws
 * std::runtime_error for unknown methods.
 *
 * supportedMethods() is implemented automatically from the registration table.
 *
 * Handler access to call data (via CallKwArgs convenience accessors):
 * @code
 *   auto ids    = call.ids();        // args[0] as vector<int>
 *   auto domain = call.domain();     // args[0] as json domain
 *   auto fields = call.fields();     // kwargs["fields"]
 *   int  limit  = call.limit();      // kwargs["limit"]
 *   int  offset = call.offset();     // kwargs["offset"]
 *   auto order  = call.order();      // kwargs["order"]
 *   int  uid    = call.kwargs.value("context", json{}).value("uid", 0);
 * @endcode
 */
class BaseViewModel : public IViewModel {
public:
    // ----------------------------------------------------------
    // IViewModel
    // ----------------------------------------------------------

    nlohmann::json callKw(const CallKwArgs& call) override {
        auto it = dispatch_.find(call.method);
        if (it == dispatch_.end())
            throw std::runtime_error(
                "ViewModel '" + modelName() +
                "': unknown method '" + call.method + "'");
        return it->second(call);
    }

    std::vector<std::string> supportedMethods() const override {
        std::vector<std::string> methods;
        methods.reserve(dispatch_.size());
        for (const auto& [k, _] : dispatch_) methods.push_back(k);
        return methods;
    }

    bool supportsMethod(const std::string& method) const override {
        return dispatch_.count(method) > 0;
    }

protected:
    using Handler = std::function<nlohmann::json(const CallKwArgs&)>;

    /**
     * @brief Register a handler for a method name.
     * Called by the REGISTER_METHOD macro from derived constructors.
     */
    void registerMethod_(const std::string& method, Handler handler) {
        dispatch_[method] = std::move(handler);
    }

private:
    std::unordered_map<std::string, Handler> dispatch_;
};

} // namespace odoo::core
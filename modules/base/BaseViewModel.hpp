#pragma once
#include "interfaces/IViewModel.hpp"
#include "UserContext.hpp"
#include "AuditService.hpp"
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cerp::core {

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
        [this](const ::cerp::core::CallKwArgs& call) -> nlohmann::json {  \
            return this->handler(call);                                    \
        });


// ============================================================
// REGISTER_MUTATOR macro  (P6 / ARCH-1)
// ============================================================
/**
 * @brief Register a create / write / unlink handler with audit applied
 *        automatically.
 *
 * WHY THIS EXISTS
 *   S-35 (record rules), S-37 (audit), S-38 (CSV rules) and S-47
 *   (identity audit) were four instances of ONE defect: behaviour wired
 *   into GenericViewModel is silently absent from hand-written
 *   ViewModels. Each was retrofitted case by case, and each retrofit
 *   missed ViewModels written earlier. Four occurrences is enough to fix
 *   the pattern rather than the instances.
 *
 * WHAT IT GUARANTEES
 *   The audit entry is written after the handler succeeds, with the
 *   right model name and the acting uid, without the author having to
 *   remember. A handler that throws writes no audit row — correct, since
 *   nothing changed.
 *
 * WHAT IT DOES NOT GUARANTEE
 *   Record-rule enforcement. That needs the model prototype
 *   (proto.setUserContext()), which BaseViewModel has no access to —
 *   GenericViewModel does it because it owns TModel. ViewModels built on
 *   hand-written SQL still have to merge the rule domain themselves.
 *   The boot check below reports which ones those are rather than
 *   letting the gap stay invisible.
 *
 * Usage — identical in shape to REGISTER_METHOD:
 * @code
 *   REGISTER_MUTATOR("write",  handleWrite)
 *   REGISTER_MUTATOR("create", handleCreate)
 *   REGISTER_MUTATOR("unlink", handleUnlink)
 * @endcode
 */
#define REGISTER_MUTATOR(method_name, handler)                             \
    registerMutator_(method_name,                                          \
        [this](const ::cerp::core::CallKwArgs& call) -> nlohmann::json {  \
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

    /**
     * @brief Mutating methods registered WITHOUT the audited path. (P6)
     *
     * Any create/write/unlink that went through REGISTER_METHOD instead of
     * REGISTER_MUTATOR. Container checks this at boot and refuses to start
     * unless the ViewModel is on a named allowlist — so the S-35/37/38/47
     * class of defect cannot be reintroduced silently by the next module.
     */
    std::vector<std::string> unguardedMutators() const override {
        static const char* kMutating[] = {"create", "write", "unlink"};
        std::vector<std::string> out;
        for (const char* m : kMutating)
            if (dispatch_.count(m) && !guardedMutators_.count(m))
                out.emplace_back(m);
        return out;
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

    /**
     * @brief Register a mutating handler, wrapping it with audit. (P6)
     * Called by REGISTER_MUTATOR; see the macro for the rationale.
     */
    void registerMutator_(const std::string& method, Handler handler) {
        guardedMutators_.insert(method);
        dispatch_[method] = [this, method, h = std::move(handler)]
                            (const CallKwArgs& call) -> nlohmann::json {
            const UserContext ctx = extractContext_(call);
            // Capture ids BEFORE the call: unlink destroys them, and a write
            // handler is free to mutate what it was given.
            const std::vector<int> idsBefore = call.ids();

            nlohmann::json result = h(call);

            // A throwing handler never reaches here, so a failed operation
            // writes no audit row.
            if (method == "create") {
                int newId = 0;
                if (result.is_number_integer())      newId = result.get<int>();
                else if (result.is_array() && !result.empty()
                         && result[0].is_number_integer())
                                                     newId = result[0].get<int>();
                audit_("create", newId, ctx);
            } else {
                audit_(method, idsBefore, ctx);
            }
            return result;
        };
    }

    /**
     * @brief Extract the calling user's context from call.kwargs["context"].
     *
     * Populated by JsonRpcDispatcher for every authenticated JSON-RPC call.
     * Pass the result to proto.setUserContext() before any model CRUD/search
     * call so that record rules (S-30) are enforced automatically.
     *
     * Handlers that only read schema metadata (fieldsGet, default_get) do NOT
     * need to call this — record rules are irrelevant for those.
     */
    static UserContext extractContext_(const CallKwArgs& call) {
        UserContext ctx;
        const auto& kw = call.kwargs;
        if (!kw.contains("context") || !kw["context"].is_object()) return ctx;
        const auto& c = kw["context"];
        if (c.contains("uid")        && c["uid"].is_number_integer())
            ctx.uid       = c["uid"].get<int>();
        if (c.contains("company_id") && c["company_id"].is_number_integer())
            ctx.companyId = c["company_id"].get<int>();
        if (c.contains("partner_id") && c["partner_id"].is_number_integer())
            ctx.partnerId = c["partner_id"].get<int>();
        if (c.contains("is_admin")   && c["is_admin"].is_boolean())
            ctx.isAdmin   = c["is_admin"].get<bool>();
        if (c.contains("group_ids")  && c["group_ids"].is_array())
            for (const auto& g : c["group_ids"])
                if (g.is_number_integer()) ctx.groupIds.push_back(g.get<int>());
        // docs/094 — the companies this user may act for. The dispatcher fills
        // this from the session, never from the client body: it arrives in the
        // same context object a caller could try to forge, and the dispatcher
        // overwrites every one of these keys before the model sees them.
        if (c.contains("allowed_company_ids") && c["allowed_company_ids"].is_array())
            for (const auto& a : c["allowed_company_ids"])
                if (a.is_number_integer()) ctx.allowedCompanyIds.push_back(a.get<int>());
        return ctx;
    }

    /**
     * @brief Record a mutation in audit_log. (S-37 / S-47)
     *
     * GenericViewModel audits from its own handlers, so models on the generic
     * path are covered automatically. Custom ViewModels are not, and four
     * separate findings (S-35 record rules, S-37 audit, S-38 CSV rules,
     * S-47 identity/privilege audit) have all been the same defect:
     * cross-cutting behaviour wired into GenericViewModel and silently absent
     * from hand-written ViewModels.
     *
     * This helper lives here so a custom ViewModel joins the audited path with
     * one line per handler and cannot get the model name or the readiness
     * check subtly wrong:
     *
     * @code
     *   nlohmann::json handleWrite(const CallKwArgs& call) {
     *       const auto ctx = extractContext_(call);
     *       ... perform the write ...
     *       audit_("write", call.ids(), ctx);
     *       return true;
     *   }
     * @endcode
     *
     * Never throws: audit failure must not break the operation being audited.
     * modelName() is virtual, so the correct model is recorded automatically.
     */
    void audit_(const std::string&      operation,
                const std::vector<int>& recordIds,
                const UserContext&      ctx) const {
        if (recordIds.empty()) return;
        if (!infrastructure::AuditService::ready()) return;
        try {
            infrastructure::AuditService::instance().log(
                modelName(), operation, recordIds, ctx.uid);
        } catch (...) {
            // AuditService::log() already swallows and logs its own errors;
            // this is belt-and-braces so no audit path can ever propagate.
        }
    }

    /// Convenience overload for single-record operations (e.g. create).
    void audit_(const std::string& operation, int recordId,
                const UserContext& ctx) const {
        if (recordId > 0) audit_(operation, std::vector<int>{recordId}, ctx);
    }

private:
    std::unordered_map<std::string, Handler> dispatch_;
    std::unordered_set<std::string>          guardedMutators_;   // P6
};

} // namespace cerp::core
#pragma once
// =============================================================
// modules/base/GenericViewModel.hpp
//
// Reusable CRTP ViewModel template for simple CRUD models.
// Provides: search_read, read, create, write, unlink,
//           fields_get, search_count, search.
//
// S-30: Before every model call, extractContext_() reads
// uid/companyId/groupIds/isAdmin from call.kwargs["context"]
// (injected by JsonRpcDispatcher) and passes them to
// proto.setUserContext() so record-rule filtering is applied
// automatically.
//
// Audit: create/write/unlink handlers call AuditService::log()
// when the service is ready, recording every mutation with the
// acting uid, model, operation, and affected ids.
//
// Usage:
//   class MyViewModel : public GenericViewModel<MyModel> {
//   public:
//       explicit MyViewModel(std::shared_ptr<infrastructure::DbConnection> db)
//           : GenericViewModel(std::move(db)) {}
//   };
// =============================================================
#include "BaseViewModel.hpp"
#include "DbConnection.hpp"
#include "AuditService.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

namespace cerp::core {

template<typename TModel>
class GenericViewModel : public BaseViewModel {
public:
    explicit GenericViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : db_(std::move(db))
    {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_METHOD("web_read",        handleRead)
        REGISTER_MUTATOR("create",          handleCreate)
        REGISTER_MUTATOR("write",           handleWrite)
        REGISTER_MUTATOR("unlink",          handleUnlink)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
        REGISTER_METHOD("search",          handleSearch)
        REGISTER_METHOD("default_get",     handleDefaultGet)
        REGISTER_METHOD("read_group",      handleReadGroup)      // docs/095
        REGISTER_METHOD("web_read_group",  handleReadGroup)
    }

    std::string modelName() const override { return TModel::MODEL_NAME; }

protected:
    std::shared_ptr<infrastructure::DbConnection> db_;

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        TModel proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.searchRead(call.domain(), call.fields(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "id ASC");
    }
    nlohmann::json handleRead(const CallKwArgs& call) {
        TModel proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.read(call.ids(), call.fields());
    }
    nlohmann::json handleCreate(const CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("create: args[0] must be a dict");
        TModel proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        // No AuditService::log() here. REGISTER_MUTATOR already audits —
        // registerMutator_ calls audit_("create", newId, ctx) around the
        // handler — so logging again produced TWO rows for every create.
        //
        // This is the P6 leftover (docs/050): the registrations were
        // converted to REGISTER_MUTATOR but the manual log() calls inside
        // the handlers were not removed. It was fixed in the individual
        // modules and missed here, in the template that backs most models.
        return proto.create(v);
    }
    nlohmann::json handleWrite(const CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        TModel proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        // Audited by REGISTER_MUTATOR — see handleCreate.
        return proto.write(call.ids(), v);
    }
    nlohmann::json handleUnlink(const CallKwArgs& call) {
        TModel proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        // Audited by REGISTER_MUTATOR, which captures the ids BEFORE the
        // handler runs — necessary for unlink, since afterwards there is
        // nothing left to read them from.
        return proto.unlink(call.ids());
    }
    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        TModel proto(db_);
        return proto.fieldsGet(call.fields());  // no rule filter needed for metadata
    }
    /**
     * docs/095 — grouped aggregation.
     *
     * the reference ERP's call shape is read_group(domain, fields, groupby, ...), passed
     * positionally in args, but the OWL client also sends the same three in
     * kwargs. Both are accepted here because both turn up in practice and a
     * silently-empty group list is a miserable thing to debug.
     */
    nlohmann::json handleReadGroup(const CallKwArgs& call) {
        auto pick = [&](int argIdx, const char* kw) -> nlohmann::json {
            const auto a = call.arg(argIdx);
            if (!a.is_null() && !(a.is_array() && a.empty())) return a;
            if (call.kwargs.contains(kw)) return call.kwargs[kw];
            return a;
        };
        const nlohmann::json domain  = pick(0, "domain");
        const nlohmann::json fields  = pick(1, "fields");
        const nlohmann::json groupby = pick(2, "groupby");

        int limit  = call.limit();
        int offset = call.offset();
        std::string order;
        if (call.kwargs.contains("orderby") && call.kwargs["orderby"].is_string())
            order = call.kwargs["orderby"].get<std::string>();
        else if (call.kwargs.contains("order") && call.kwargs["order"].is_string())
            order = call.kwargs["order"].get<std::string>();

        TModel proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.readGroup(domain, fields, groupby, limit, offset, order);
    }

    nlohmann::json handleSearchCount(const CallKwArgs& call) {
        TModel proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.searchCount(call.domain());
    }
    // default_get — returns {} by default; override in derived class for pre-filled forms
    nlohmann::json handleDefaultGet(const CallKwArgs& /*call*/) {
        return nlohmann::json::object();
    }
    nlohmann::json handleSearch(const CallKwArgs& call) {
        TModel proto(db_);
        proto.setUserContext(extractContext_(call));
        auto ids = proto.search(call.domain(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "id ASC");
        nlohmann::json arr = nlohmann::json::array();
        for (int id : ids) arr.push_back(id);
        return arr;
    }
};

} // namespace cerp::core

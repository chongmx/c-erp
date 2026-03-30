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
// Usage:
//   class MyViewModel : public GenericViewModel<MyModel> {
//   public:
//       explicit MyViewModel(std::shared_ptr<infrastructure::DbConnection> db)
//           : GenericViewModel(std::move(db)) {}
//   };
// =============================================================
#include "BaseViewModel.hpp"
#include "DbConnection.hpp"
#include "UserContext.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

namespace odoo::core {

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
        REGISTER_METHOD("create",          handleCreate)
        REGISTER_METHOD("write",           handleWrite)
        REGISTER_METHOD("unlink",          handleUnlink)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
        REGISTER_METHOD("search",          handleSearch)
        REGISTER_METHOD("default_get",     handleDefaultGet)
    }

    std::string modelName() const override { return TModel::MODEL_NAME; }

protected:
    std::shared_ptr<infrastructure::DbConnection> db_;

    // S-30: Extract UserContext from call.kwargs["context"] (populated by
    // JsonRpcDispatcher) and apply it to the model proto so record rules fire.
    static UserContext extractContext_(const CallKwArgs& call) {
        UserContext ctx;
        const auto& kw = call.kwargs;
        if (!kw.contains("context") || !kw["context"].is_object())
            return ctx;
        const auto& c = kw["context"];
        if (c.contains("uid") && c["uid"].is_number_integer())
            ctx.uid = c["uid"].get<int>();
        if (c.contains("company_id") && c["company_id"].is_number_integer())
            ctx.companyId = c["company_id"].get<int>();
        if (c.contains("partner_id") && c["partner_id"].is_number_integer())
            ctx.partnerId = c["partner_id"].get<int>();
        if (c.contains("is_admin") && c["is_admin"].is_boolean())
            ctx.isAdmin = c["is_admin"].get<bool>();
        if (c.contains("group_ids") && c["group_ids"].is_array())
            for (const auto& g : c["group_ids"])
                if (g.is_number_integer()) ctx.groupIds.push_back(g.get<int>());
        return ctx;
    }

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
        proto.setUserContext(extractContext_(call));
        return proto.create(v);
    }
    nlohmann::json handleWrite(const CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        TModel proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.write(call.ids(), v);
    }
    nlohmann::json handleUnlink(const CallKwArgs& call) {
        TModel proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.unlink(call.ids());
    }
    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        TModel proto(db_);
        return proto.fieldsGet(call.fields());  // no rule filter needed for metadata
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

} // namespace odoo::core

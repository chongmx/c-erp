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
        const auto newId = proto.create(v);
        if (infrastructure::AuditService::ready()) {
            // proto.create() returns int directly; convert to json happens on return
            int id = 0;
            if constexpr (std::is_integral_v<std::decay_t<decltype(newId)>>) {
                id = static_cast<int>(newId);
            } else if (newId.is_number_integer()) {
                id = newId.template get<int>();
            }
            if (id > 0)
                infrastructure::AuditService::instance().log(
                    TModel::MODEL_NAME, "create", {id}, ctx.uid);
        }
        return newId;
    }
    nlohmann::json handleWrite(const CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        TModel proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const auto result = proto.write(call.ids(), v);
        if (infrastructure::AuditService::ready() && !call.ids().empty())
            infrastructure::AuditService::instance().log(
                TModel::MODEL_NAME, "write", call.ids(), ctx.uid);
        return result;
    }
    nlohmann::json handleUnlink(const CallKwArgs& call) {
        TModel proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const auto ids = call.ids();  // capture before unlink
        const auto result = proto.unlink(ids);
        if (infrastructure::AuditService::ready() && !ids.empty())
            infrastructure::AuditService::instance().log(
                TModel::MODEL_NAME, "unlink", ids, ctx.uid);
        return result;
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

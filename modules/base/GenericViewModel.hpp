#pragma once
// =============================================================
// modules/base/GenericViewModel.hpp
//
// Reusable CRTP ViewModel template for simple CRUD models.
// Provides: search_read, read, create, write, unlink,
//           fields_get, search_count, search.
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
        return proto.searchRead(call.domain(), call.fields(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "id ASC");
    }
    nlohmann::json handleRead(const CallKwArgs& call) {
        TModel proto(db_);
        return proto.read(call.ids(), call.fields());
    }
    nlohmann::json handleCreate(const CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("create: args[0] must be a dict");
        TModel proto(db_);
        return proto.create(v);
    }
    nlohmann::json handleWrite(const CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        TModel proto(db_);
        return proto.write(call.ids(), v);
    }
    nlohmann::json handleUnlink(const CallKwArgs& call) {
        TModel proto(db_);
        return proto.unlink(call.ids());
    }
    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        TModel proto(db_);
        return proto.fieldsGet(call.fields());
    }
    nlohmann::json handleSearchCount(const CallKwArgs& call) {
        TModel proto(db_);
        return proto.searchCount(call.domain());
    }
    // default_get — returns {} by default; override in derived class for pre-filled forms
    nlohmann::json handleDefaultGet(const CallKwArgs& /*call*/) {
        return nlohmann::json::object();
    }
    nlohmann::json handleSearch(const CallKwArgs& call) {
        TModel proto(db_);
        auto ids = proto.search(call.domain(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "id ASC");
        nlohmann::json arr = nlohmann::json::array();
        for (int id : ids) arr.push_back(id);
        return arr;
    }
};

} // namespace odoo::core

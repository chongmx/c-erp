// =============================================================
// modules/ir/IrModule.cpp  — full implementation
// =============================================================
#include "IrModule.hpp"
#include "IModule.hpp"
#include "Factories.hpp"
#include "BaseModel.hpp"
#include "BaseViewModel.hpp"
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include "TtlCache.hpp"
#include "RuleEngine.hpp"
#include "AuditService.hpp"
#include "DecimalPrecision.hpp"
#include "IrSequence.hpp"
#include "IrCron.hpp"
#include "Money.hpp"
#include "CacheInvalidation.hpp"
#include "MigrationRunner.hpp"
#include "CsvParser.hpp"
#include "Errors.hpp"
#include "SessionManager.hpp"
#include "Filestore.hpp"
#include "IrModelData.hpp"
#include <drogon/drogon.h>
#include <drogon/MultiPart.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

namespace odoo::modules::ir {

using namespace odoo::infrastructure;
using namespace odoo::core;

// ================================================================
// 1. MODELS
// ================================================================

class IrUiMenu : public core::BaseModel<IrUiMenu> {
public:
    ODOO_MODEL("ir.ui.menu", "ir_ui_menu")

    std::string name;
    int         parentId  = 0;
    int         sequence  = 10;
    int         actionId  = 0;
    std::string webIcon;
    bool        active    = true;

    explicit IrUiMenu(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<IrUiMenu>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",       core::FieldType::Char,    "Menu",     true});
        fieldRegistry_.add({"parent_id",  core::FieldType::Many2one,"Parent",   false, false, true, false, "ir.ui.menu"});
        fieldRegistry_.add({"sequence",   core::FieldType::Integer, "Sequence"});
        fieldRegistry_.add({"action_id",  core::FieldType::Many2one,"Action",   false, false, true, false, "ir.actions.act_window"});
        fieldRegistry_.add({"web_icon",   core::FieldType::Char,    "Icon"});
        fieldRegistry_.add({"active",     core::FieldType::Boolean, "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]      = name;
        j["parent_id"] = parentId > 0 ? nlohmann::json(parentId) : nlohmann::json(false);
        j["sequence"]  = sequence;
        j["action_id"] = actionId > 0 ? nlohmann::json(actionId) : nlohmann::json(false);
        j["web_icon"]  = webIcon;
        j["active"]    = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")      && j["name"].is_string())          name     = j["name"].get<std::string>();
        if (j.contains("sequence")  && j["sequence"].is_number())      sequence = j["sequence"].get<int>();
        if (j.contains("action_id") && j["action_id"].is_number())     actionId = j["action_id"].get<int>();
        if (j.contains("parent_id") && j["parent_id"].is_number())     parentId = j["parent_id"].get<int>();
        if (j.contains("active")    && j["active"].is_boolean())       active   = j["active"].get<bool>();
        if (j.contains("web_icon")  && j["web_icon"].is_string())      webIcon  = j["web_icon"].get<std::string>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Menu name is required");
        return e;
    }
};

// ----------------------------------------------------------------
// IrActWindow
// ----------------------------------------------------------------
class IrActWindow : public core::BaseModel<IrActWindow> {
public:
    ODOO_MODEL("ir.actions.act_window", "ir_act_window")

    std::string name;
    std::string resModel;
    std::string viewMode = "list,form";
    std::string domain;
    std::string context = "{}";
    std::string target  = "current";
    std::string path;
    std::string help;

    explicit IrActWindow(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<IrActWindow>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",       core::FieldType::Char, "Action Name", true});
        fieldRegistry_.add({"res_model",  core::FieldType::Char, "Model",       true});
        fieldRegistry_.add({"view_mode",  core::FieldType::Char, "View Mode"});
        fieldRegistry_.add({"domain",     core::FieldType::Char, "Domain"});
        fieldRegistry_.add({"context",    core::FieldType::Char, "Context"});
        fieldRegistry_.add({"target",     core::FieldType::Char, "Target"});
        fieldRegistry_.add({"path",       core::FieldType::Char, "URL Path"});
        fieldRegistry_.add({"help",       core::FieldType::Char, "Help"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]      = name;
        j["res_model"] = resModel;
        j["view_mode"] = viewMode;
        j["domain"]    = domain.empty() ? nlohmann::json(false) : nlohmann::json(domain);
        j["context"]   = context;
        j["target"]    = target;
        j["path"]      = path.empty()   ? nlohmann::json(false) : nlohmann::json(path);
        j["help"]      = help;
        j["type"]      = "ir.actions.act_window";
        nlohmann::json views = nlohmann::json::array();
        std::string mode = viewMode;
        std::string::size_type pos = 0, end;
        while ((end = mode.find(',', pos)) != std::string::npos) {
            views.push_back({false, mode.substr(pos, end - pos)});
            pos = end + 1;
        }
        views.push_back({false, mode.substr(pos)});
        j["views"] = views;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")      && j["name"].is_string())      name      = j["name"].get<std::string>();
        if (j.contains("res_model") && j["res_model"].is_string()) resModel  = j["res_model"].get<std::string>();
        if (j.contains("view_mode") && j["view_mode"].is_string()) viewMode  = j["view_mode"].get<std::string>();
        if (j.contains("domain")    && j["domain"].is_string())    domain    = j["domain"].get<std::string>();
        if (j.contains("context")   && j["context"].is_string())   context   = j["context"].get<std::string>();
        if (j.contains("target")    && j["target"].is_string())    target    = j["target"].get<std::string>();
        if (j.contains("path")      && j["path"].is_string())      path      = j["path"].get<std::string>();
        if (j.contains("help")      && j["help"].is_string())      help      = j["help"].get<std::string>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty())     e.push_back("Action name is required");
        if (resModel.empty()) e.push_back("Model is required");
        return e;
    }
};

// ----------------------------------------------------------------
// IrConfigParameter
// ----------------------------------------------------------------
class IrConfigParameter : public core::BaseModel<IrConfigParameter> {
public:
    ODOO_MODEL("ir.config.parameter", "ir_config_parameter")

    std::string key;
    std::string value;

    explicit IrConfigParameter(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<IrConfigParameter>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"key",   core::FieldType::Char, "Key",   true});
        fieldRegistry_.add({"value", core::FieldType::Char, "Value", false});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["key"]   = key;
        j["value"] = value;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("key")   && j["key"].is_string())   key   = j["key"].get<std::string>();
        if (j.contains("value") && j["value"].is_string()) value = j["value"].get<std::string>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (key.empty()) e.push_back("Key is required");
        return e;
    }
};

// ----------------------------------------------------------------
// DecimalPrecisionModel — decimal.precision  (P2, docs/048 §2.1)
//
// User-configurable DISPLAY precision. Storage is always Money::SCALE
// and is not affected by these values; they govern rendering and the
// rounding boundary only.
// ----------------------------------------------------------------
class DecimalPrecisionModel : public core::BaseModel<DecimalPrecisionModel> {
public:
    ODOO_MODEL("decimal.precision", "decimal_precision")

    std::string name;
    int         digits = 2;

    explicit DecimalPrecisionModel(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<DecimalPrecisionModel>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",   core::FieldType::Char,    "Usage",    true, true});
        fieldRegistry_.add({"digits", core::FieldType::Integer, "Decimals", true});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]   = name;
        j["digits"] = digits;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")   && j["name"].is_string())          name   = j["name"].get<std::string>();
        if (j.contains("digits") && j["digits"].is_number_integer()) digits = j["digits"].get<int>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty())               e.push_back("Usage is required");
        // Mirrors the DB CHECK constraint. Enforced here too so the user gets
        // a clear message instead of a raw constraint violation, and because
        // Money::SCALE is the hard ceiling — more decimals than the storage
        // scale cannot be represented.
        if (digits < 0 || digits > core::Money::SCALE)
            e.push_back("Decimals must be between 0 and " +
                        std::to_string(core::Money::SCALE));
        return e;
    }
};

// ================================================================
// 2. VIEWMODELS
// ================================================================

// P2: writing a precision changes what fields_get reports, so both the
// dispatcher's fields_get cache and DecimalPrecision's own cache must be
// dropped — otherwise the change is invisible until the next restart.
class DecimalPrecisionViewModel : public core::GenericViewModel<DecimalPrecisionModel> {
public:
    explicit DecimalPrecisionViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : core::GenericViewModel<DecimalPrecisionModel>(std::move(db))
    {
        // Re-register over the generic handlers so the invalidation runs after
        // the write succeeds. Deliberately no `create`/`unlink`: the five rows
        // are seeded by migration 901 and adding or removing usages would
        // silently detach fields whose precisionName no longer resolves.
        REGISTER_MUTATOR("write", handleWriteAndInvalidate)
    }

    nlohmann::json handleWriteAndInvalidate(const core::CallKwArgs& call) {
        // Validate before the write. BaseModel::write() does NOT call
        // validate(), so without this the only guard is the DB CHECK
        // constraint — whose pqxx error is gated behind devMode by SEC-28 and
        // reaches the user as "An internal error occurred". A settings screen
        // has to say what is actually wrong.
        const auto vals = call.arg(1);
        if (vals.is_object() && vals.contains("digits")) {
            if (!vals["digits"].is_number_integer())
                throw infrastructure::ValidationError("Decimals must be a whole number.");
            const int d = vals["digits"].get<int>();
            if (d < 0 || d > core::Money::SCALE)
                throw infrastructure::ValidationError(
                    "Decimals must be between 0 and " +
                    std::to_string(core::Money::SCALE) +
                    ". Values are stored at " + std::to_string(core::Money::SCALE) +
                    " decimal places, so more than that cannot be represented.");
        }

        auto result = this->handleWrite(call);
        if (core::DecimalPrecision::ready())
            core::DecimalPrecision::instance().invalidate();
        core::CacheInvalidation::fieldsGet();
        return result;
    }
};

class IrMenuViewModel : public core::BaseViewModel {
public:
    explicit IrMenuViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : db_(std::move(db))
    {
        REGISTER_METHOD("load_menus",  handleLoadMenus)
        REGISTER_METHOD("search_read", handleSearchRead)
        REGISTER_METHOD("read",        handleRead)
        REGISTER_METHOD("fields_get",  handleFieldsGet)
    }

    std::string modelName() const override { return "ir.ui.menu"; }
    static void invalidateMenuCache() { menuCache_.invalidateAll(); }

private:
    std::shared_ptr<infrastructure::DbConnection> db_;
    static inline infrastructure::TtlCache<std::string, nlohmann::json> menuCache_;

    nlohmann::json handleLoadMenus(const core::CallKwArgs& /*call*/) {
        if (auto cached = menuCache_.get("menus")) return *cached;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto rows = txn.exec(
            "SELECT id, name, parent_id, sequence, action_id, web_icon "
            "FROM ir_ui_menu WHERE active = TRUE "
            "ORDER BY sequence, id");

        std::map<int, std::vector<int>> children;
        std::vector<nlohmann::json> menuRows;

        for (const auto& r : rows) {
            int id  = r["id"].as<int>();
            int pid = r["parent_id"].is_null() ? 0 : r["parent_id"].as<int>();
            children[pid].push_back(id);
            nlohmann::json m;
            m["id"]        = id;
            m["name"]      = std::string(r["name"].c_str());
            m["parent_id"] = pid;
            m["sequence"]  = r["sequence"].as<int>();
            m["action_id"] = r["action_id"].is_null() ? 0 : r["action_id"].as<int>();
            m["web_icon"]  = r["web_icon"].is_null() ? "" : std::string(r["web_icon"].c_str());
            menuRows.push_back(std::move(m));
        }

        std::map<int, int> appOf;
        std::function<void(int, int)> setApp = [&](int appId, int menuId) {
            appOf[menuId] = appId;
            if (children.count(menuId))
                for (int c : children[menuId]) setApp(appId, c);
        };
        for (int rootId : children[0]) setApp(rootId, rootId);

        nlohmann::json result = nlohmann::json::object();
        for (const auto& m : menuRows) {
            int id       = m["id"].get<int>();
            int actionId = m["action_id"].get<int>();
            nlohmann::json entry;
            entry["id"]                     = id;
            entry["name"]                   = m["name"];
            entry["app_id"]                 = appOf.count(id) ? nlohmann::json(appOf[id]) : nlohmann::json(false);
            entry["action_model"]           = actionId > 0 ? nlohmann::json("ir.actions.act_window") : nlohmann::json(false);
            entry["action_id"]              = actionId > 0 ? nlohmann::json(actionId) : nlohmann::json(false);
            entry["web_icon"]               = m["web_icon"].get<std::string>().empty()
                                              ? nlohmann::json(false)
                                              : nlohmann::json(m["web_icon"].get<std::string>());
            entry["web_icon_data"]          = false;
            entry["web_icon_data_mimetype"] = false;
            entry["xmlid"]                  = "";
            entry["action_path"]            = false;
            entry["children"]               = children.count(id)
                                              ? nlohmann::json(children[id])
                                              : nlohmann::json::array();
            result[std::to_string(id)]      = std::move(entry);
        }

        nlohmann::json rootChildren = nlohmann::json::array();
        if (children.count(0))
            for (int c : children[0]) rootChildren.push_back(c);
        result["root"] = {{"id", false}, {"name", "root"}, {"children", rootChildren}};

        menuCache_.set("menus", result, 60);
        return result;
    }

    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        IrUiMenu proto(db_);
        return proto.searchRead(call.domain(), call.fields(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "sequence ASC, id ASC");
    }

    nlohmann::json handleRead(const core::CallKwArgs& call) {
        IrUiMenu proto(db_);
        return proto.read(call.ids(), call.fields());
    }

    nlohmann::json handleFieldsGet(const core::CallKwArgs& call) {
        IrUiMenu proto(db_);
        return proto.fieldsGet(call.fields());
    }
};

// ----------------------------------------------------------------
// IrActWindowViewModel
// ----------------------------------------------------------------
class IrActWindowViewModel : public core::BaseViewModel {
public:
    explicit IrActWindowViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : db_(std::move(db))
    {
        REGISTER_METHOD("search_read", handleSearchRead)
        REGISTER_METHOD("read",        handleRead)
        REGISTER_METHOD("fields_get",  handleFieldsGet)
        REGISTER_METHOD("load",        handleLoad)
    }

    std::string modelName() const override { return "ir.actions.act_window"; }
    static void invalidateActionCache() { actionCache_.invalidateAll(); }

    nlohmann::json loadById(int actionId) {
        const std::string key = "action:" + std::to_string(actionId);
        if (auto cached = actionCache_.get(key)) return *cached;

        IrActWindow proto(db_);
        auto rows = proto.read({actionId}, {});
        if (rows.empty() || (rows.is_array() && rows.empty()))
            throw std::runtime_error("Action not found: " + std::to_string(actionId));

        nlohmann::json row = rows.is_array() ? rows[0] : rows;
        nlohmann::json act = row;
        act["type"]               = "ir.actions.act_window";
        act["display_name"]       = row.value("name", std::string{});
        act["xml_id"]             = false;
        act["binding_model_id"]   = false;
        act["binding_type"]       = "action";
        act["binding_view_types"] = "list,form";

        std::string viewMode = row.value("view_mode", std::string{"list,form"});
        nlohmann::json viewsArr = nlohmann::json::array();
        std::string::size_type pos = 0, end;
        while ((end = viewMode.find(',', pos)) != std::string::npos) {
            viewsArr.push_back(nlohmann::json::array({false, viewMode.substr(pos, end - pos)}));
            pos = end + 1;
        }
        viewsArr.push_back(nlohmann::json::array({false, viewMode.substr(pos)}));
        act["views"] = viewsArr;

        actionCache_.set(key, act, 60);
        return act;
    }

private:
    std::shared_ptr<infrastructure::DbConnection> db_;
    static inline infrastructure::TtlCache<std::string, nlohmann::json> actionCache_;

    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        IrActWindow proto(db_);
        return proto.searchRead(call.domain(), call.fields(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "id ASC");
    }

    nlohmann::json handleRead(const core::CallKwArgs& call) {
        IrActWindow proto(db_);
        return proto.read(call.ids(), call.fields());
    }

    nlohmann::json handleFieldsGet(const core::CallKwArgs& call) {
        IrActWindow proto(db_);
        return proto.fieldsGet(call.fields());
    }

    nlohmann::json handleLoad(const core::CallKwArgs& call) {
        const int id = call.arg(0).is_number_integer()
                       ? call.arg(0).get<int>()
                       : call.kwargs.value("action_id", 0);
        return loadById(id);
    }
};

// ----------------------------------------------------------------
// IrModelDataModel — ir.model.data (external identifiers)
// ----------------------------------------------------------------
class IrModelDataModel : public core::BaseModel<IrModelDataModel> {
public:
    ODOO_MODEL("ir.model.data", "ir_model_data")

    std::string module, name, model;
    int         resId    = 0;
    bool        noupdate = false;

    explicit IrModelDataModel(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<IrModelDataModel>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"module",   core::FieldType::Char,    "Module", true});
        fieldRegistry_.add({"name",     core::FieldType::Char,    "External Name", true});
        fieldRegistry_.add({"model",    core::FieldType::Char,    "Model", true});
        fieldRegistry_.add({"res_id",   core::FieldType::Integer, "Record ID"});
        fieldRegistry_.add({"noupdate", core::FieldType::Boolean, "Non Updatable"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["module"] = module; j["name"] = name; j["model"] = model;
        j["res_id"] = resId;  j["noupdate"] = noupdate;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("module")   && j["module"].is_string())   module   = j["module"].get<std::string>();
        if (j.contains("name")     && j["name"].is_string())     name     = j["name"].get<std::string>();
        if (j.contains("model")    && j["model"].is_string())    model    = j["model"].get<std::string>();
        if (j.contains("res_id")   && j["res_id"].is_number())   resId    = j["res_id"].get<int>();
        if (j.contains("noupdate") && j["noupdate"].is_boolean())noupdate = j["noupdate"].get<bool>();
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (module.empty()) e.push_back("Module is required");
        if (name.empty())   e.push_back("External name is required");
        if (model.empty())  e.push_back("Model is required");
        return e;
    }
};

// ----------------------------------------------------------------
// IrAttachmentModel — ir.attachment (file metadata)
//
// This model is the METADATA row only. The bytes are in the filestore,
// and the binary crosses the wire through the dedicated
// /web/attachment/upload and /web/content/{id} routes, never as a field
// on this model — a base64 blob on every search_read would defeat the
// point of storing it out of the row.
// ----------------------------------------------------------------
class IrAttachmentModel : public core::BaseModel<IrAttachmentModel> {
public:
    ODOO_MODEL("ir.attachment", "ir_attachment")

    std::string name, description, resModel, resField, type = "binary",
                url, mimetype = "application/octet-stream", checksum, storeFname;
    int       resId = 0, companyId = 1, createUid = 0;
    long long fileSize = 0;
    bool      isPublic = false;

    explicit IrAttachmentModel(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<IrAttachmentModel>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",        core::FieldType::Char,    "Name", true});
        fieldRegistry_.add({"description", core::FieldType::Text,    "Description"});
        fieldRegistry_.add({"res_model",   core::FieldType::Char,    "Resource Model"});
        fieldRegistry_.add({"res_id",      core::FieldType::Integer, "Resource ID"});
        fieldRegistry_.add({"res_field",   core::FieldType::Char,    "Resource Field"});
        fieldRegistry_.add({"type",        core::FieldType::Char,    "Type"});
        fieldRegistry_.add({"url",         core::FieldType::Char,    "URL"});
        fieldRegistry_.add({"mimetype",    core::FieldType::Char,    "Mime Type"});
        fieldRegistry_.add({"file_size",   core::FieldType::Integer, "File Size"});
        fieldRegistry_.add({"checksum",    core::FieldType::Char,    "Checksum"});
        fieldRegistry_.add({"store_fname", core::FieldType::Char,    "Stored Filename"});
        fieldRegistry_.add({"public",      core::FieldType::Boolean, "Is Public"});
        fieldRegistry_.add({"company_id",  core::FieldType::Many2one,"Company", false, false, true, false, "res.company"});
        fieldRegistry_.add({"create_uid",  core::FieldType::Integer, "Created By"});
        // file_size is a genuine byte count, not money, so NOT markScaled.
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"]        = name;
        j["description"] = description;
        j["res_model"]   = resModel.empty()  ? nlohmann::json(nullptr) : nlohmann::json(resModel);
        j["res_id"]      = resId > 0 ? nlohmann::json(resId) : nlohmann::json(nullptr);
        j["res_field"]   = resField.empty()   ? nlohmann::json(nullptr) : nlohmann::json(resField);
        j["type"]        = type;
        j["url"]         = url.empty() ? nlohmann::json(nullptr) : nlohmann::json(url);
        j["mimetype"]    = mimetype;
        j["file_size"]   = fileSize;
        j["checksum"]    = checksum;
        j["store_fname"] = storeFname;
        j["public"]      = isPublic;
        j["company_id"]  = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["create_uid"]  = createUid;
    }
    void deserializeFields(const nlohmann::json& j) override {
        auto s = [&](const char* k, std::string& v){ if (j.contains(k) && j[k].is_string()) v = j[k].get<std::string>(); };
        s("name", name); s("description", description); s("res_model", resModel);
        s("res_field", resField); s("type", type); s("url", url);
        s("mimetype", mimetype); s("checksum", checksum); s("store_fname", storeFname);
        if (j.contains("res_id")     && j["res_id"].is_number())     resId     = j["res_id"].get<int>();
        if (j.contains("file_size")  && j["file_size"].is_number())  fileSize  = j["file_size"].get<long long>();
        if (j.contains("public")     && j["public"].is_boolean())    isPublic  = j["public"].get<bool>();
        if (j.contains("company_id"))                                companyId = m2oToId_(j["company_id"]);
        if (j.contains("create_uid") && j["create_uid"].is_number()) createUid = j["create_uid"].get<int>();
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Name is required");
        return e;
    }
private:
    static int m2oToId_(const nlohmann::json& v) {
        if (v.is_number_integer()) return v.get<int>();
        if (v.is_array() && !v.empty() && v[0].is_number_integer()) return v[0].get<int>();
        return 0;
    }
};

// ----------------------------------------------------------------
// IrAttachmentViewModel — CRUD, plus the two things a plain
// GenericViewModel got wrong for a file:
//
//   1. unlink deleted the metadata row and left the BLOB on disk forever.
//      Filestore::gc() existed and nothing ever called it. Because storage is
//      content-addressed, two attachments can share one blob, so the blob may
//      only go when the last row referencing it does — which is exactly the
//      remainingRefs argument gc() takes.
//   2. the list had no download URL and a raw byte count, so every caller
//      had to rebuild both.
// ----------------------------------------------------------------
class IrAttachmentViewModel : public core::GenericViewModel<IrAttachmentModel> {
public:
    explicit IrAttachmentViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : core::GenericViewModel<IrAttachmentModel>(std::move(db))
    {
        REGISTER_METHOD("search_read",     handleListForRecord)
        REGISTER_METHOD("web_search_read", handleListForRecord)
        REGISTER_MUTATOR("unlink",          handleUnlinkWithGc)
    }
    std::string modelName() const override { return "ir.attachment"; }

private:
    // The panel's list: newest first, with the URL and a human size, so the
    // client neither builds paths nor formats bytes.
    nlohmann::json handleListForRecord(const core::CallKwArgs& call) {
        std::string resModel;
        int resId = 0;
        const auto& dom = call.domain();
        if (dom.is_array()) {
            for (const auto& leaf : dom) {
                if (!leaf.is_array() || leaf.size() != 3 || !leaf[0].is_string()) continue;
                const std::string f = leaf[0].get<std::string>();
                if (f == "res_model" && leaf[2].is_string())         resModel = leaf[2].get<std::string>();
                else if (f == "res_id" && leaf[2].is_number_integer()) resId   = leaf[2].get<int>();
            }
        }

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        std::string sql =
            "SELECT a.id, a.name, a.description, a.res_model, a.res_id, a.mimetype, "
            "       a.file_size, a.checksum, a.create_uid, "
            "       to_char(a.create_date,'YYYY-MM-DD HH24:MI') AS created, "
            "       COALESCE(u.login,'') AS created_by "
            "FROM ir_attachment a LEFT JOIN res_users u ON u.id = a.create_uid "
            "WHERE a.type = 'binary'";
        pqxx::params p;
        int n = 0;
        if (!resModel.empty()) { sql += " AND a.res_model = $" + std::to_string(++n); p.append(resModel); }
        if (resId > 0)         { sql += " AND a.res_id    = $" + std::to_string(++n); p.append(resId); }
        sql += " ORDER BY a.id DESC LIMIT " + std::to_string(call.limit() > 0 ? call.limit() : 80);
        auto res = n ? txn.exec(sql, p) : txn.exec(sql);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            const long long bytes = row["file_size"].as<long long>(0);
            char human[32];
            if (bytes >= 1024LL * 1024)      std::snprintf(human, sizeof human, "%.1f MB", bytes / 1048576.0);
            else if (bytes >= 1024)          std::snprintf(human, sizeof human, "%.0f KB", bytes / 1024.0);
            else                             std::snprintf(human, sizeof human, "%lld B", bytes);
            nlohmann::json j;
            j["id"]          = row["id"].as<int>();
            j["name"]        = row["name"].is_null() ? "" : row["name"].c_str();
            j["description"] = row["description"].is_null() ? "" : row["description"].c_str();
            j["res_model"]   = row["res_model"].is_null() ? nlohmann::json(false)
                                                          : nlohmann::json(row["res_model"].c_str());
            j["res_id"]      = row["res_id"].is_null() ? nlohmann::json(false)
                                                       : nlohmann::json(row["res_id"].as<int>());
            j["mimetype"]    = row["mimetype"].is_null() ? "" : row["mimetype"].c_str();
            j["file_size"]   = bytes;
            j["size_human"]  = human;
            j["checksum"]    = row["checksum"].is_null() ? "" : row["checksum"].c_str();
            j["created"]     = row["created"].is_null() ? "" : row["created"].c_str();
            j["created_by"]  = row["created_by"].is_null() ? "" : row["created_by"].c_str();
            j["url"]         = "/web/content/" + std::to_string(row["id"].as<int>());
            arr.push_back(std::move(j));
        }
        return arr;
    }

    // Delete the rows, then release any blob that nothing references any more.
    nlohmann::json handleUnlinkWithGc(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        std::vector<std::string> blobs;
        {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            std::string in;
            for (size_t i = 0; i < ids.size(); ++i) { if (i) in += ","; in += std::to_string(ids[i]); }
            // Read the blob paths BEFORE the delete — after it, there is
            // nothing left to look them up from.
            for (const auto& r : txn.exec(
                    "SELECT COALESCE(store_fname,'') FROM ir_attachment WHERE id IN (" + in + ")"))
                if (!r[0].is_null() && *r[0].c_str()) blobs.emplace_back(r[0].c_str());
            txn.commit();
        }

        auto res = core::GenericViewModel<IrAttachmentModel>::handleUnlink(call);

        // One count per distinct blob, after the delete: dedup means another
        // attachment may still point at it, and gc() is a no-op when it does.
        std::sort(blobs.begin(), blobs.end());
        blobs.erase(std::unique(blobs.begin(), blobs.end()), blobs.end());
        if (!blobs.empty()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            for (const auto& sf : blobs) {
                const long long refs = txn.exec(
                    "SELECT count(*) FROM ir_attachment WHERE store_fname = $1",
                    pqxx::params{sf})[0][0].as<long long>(0);
                core::Filestore::gc(sf, refs);
            }
            txn.commit();
        }
        return res;
    }
};

// ----------------------------------------------------------------
// IrModelViewModel
// ----------------------------------------------------------------
class IrModelViewModel : public core::BaseViewModel {
public:
    explicit IrModelViewModel(std::shared_ptr<core::ModelFactory> models)
        : models_(std::move(models))
    {
        REGISTER_METHOD("search_read", handleSearchRead)
        REGISTER_METHOD("fields_get",  handleFieldsGet)
        REGISTER_METHOD("read",        handleSearchRead)
    }

    std::string modelName() const override { return "ir.model"; }

private:
    std::shared_ptr<core::ModelFactory> models_;

    nlohmann::json handleSearchRead(const core::CallKwArgs& /*call*/) {
        nlohmann::json arr = nlohmann::json::array();
        int seq = 1;
        for (const auto& name : models_->registeredNames()) {
            nlohmann::json obj;
            obj["id"]    = seq++;
            obj["model"] = name;
            obj["name"]  = name;
            arr.push_back(std::move(obj));
        }
        return arr;
    }

    nlohmann::json handleFieldsGet(const core::CallKwArgs& /*call*/) {
        return {
            {"model", {{"type","char"}, {"string","Model Name"}, {"required",true}}},
            {"name",  {{"type","char"}, {"string","Description"}}},
        };
    }
};

// ================================================================
// 3. AUDIT LOG MODEL + VIEWMODEL (read-only)
// ================================================================

class AuditLog : public core::BaseModel<AuditLog> {
public:
    ODOO_MODEL("audit.log", "audit_log")

    std::string model;
    std::string operation;
    std::string recordIds;  // stored as PostgreSQL int[] literal
    int         uid       = 0;

    explicit AuditLog(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AuditLog>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"model",      core::FieldType::Char,    "Model",     true});
        fieldRegistry_.add({"operation",  core::FieldType::Char,    "Operation", true});
        fieldRegistry_.add({"record_ids", core::FieldType::Char,    "Record IDs"});
        fieldRegistry_.add({"uid",        core::FieldType::Integer, "User ID"});
        fieldRegistry_.add({"created_at", core::FieldType::Datetime,"Created At"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["model"]      = model;
        j["operation"]  = operation;
        j["record_ids"] = recordIds;
        j["uid"]        = uid;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("model")      && j["model"].is_string())     model      = j["model"].get<std::string>();
        if (j.contains("operation")  && j["operation"].is_string()) operation  = j["operation"].get<std::string>();
        if (j.contains("record_ids") && j["record_ids"].is_string())recordIds  = j["record_ids"].get<std::string>();
        if (j.contains("uid")        && j["uid"].is_number())       uid        = j["uid"].get<int>();
    }

    std::vector<std::string> validate() const override { return {}; }
};

// Read-only ViewModel: allow search_read, read, fields_get only
class AuditLogViewModel : public core::BaseViewModel {
public:
    explicit AuditLogViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : db_(std::move(db))
    {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
    }

    std::string modelName() const override { return AuditLog::MODEL_NAME; }

private:
    std::shared_ptr<infrastructure::DbConnection> db_;

    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        AuditLog proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.searchRead(call.domain(), call.fields(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "id DESC");
    }
    nlohmann::json handleRead(const core::CallKwArgs& call) {
        AuditLog proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.read(call.ids(), call.fields());
    }
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call) {
        AuditLog proto(db_);
        return proto.fieldsGet(call.fields());
    }
    nlohmann::json handleSearchCount(const core::CallKwArgs& call) {
        AuditLog proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.searchCount(call.domain());
    }
};

// ================================================================
// 4. MODULE
// ================================================================

IrModule::IrModule(core::ModelFactory&     modelFactory,
                   core::ServiceFactory&   serviceFactory,
                   core::ViewModelFactory& viewModelFactory,
                   core::ViewFactory&      /*viewFactory*/)
    : models_    (modelFactory)
    , services_  (serviceFactory)
    , viewModels_(viewModelFactory)
{}

std::string              IrModule::moduleName()   const { return "ir"; }
std::string              IrModule::version()      const { return "19.0.1.0.0"; }
std::vector<std::string> IrModule::dependencies() const { return {"auth"}; }

void IrModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("ir.ui.menu", [db]{
        return std::make_shared<IrUiMenu>(db);
    });
    models_.registerCreator("ir.actions.act_window", [db]{
        return std::make_shared<IrActWindow>(db);
    });
    models_.registerCreator("ir.config.parameter", [db]{
        return std::make_shared<IrConfigParameter>(db);
    });
    models_.registerCreator("decimal.precision", [db]{      // P2
        return std::make_shared<DecimalPrecisionModel>(db);
    });
    models_.registerCreator("ir.model.data", [db]{
        return std::make_shared<IrModelDataModel>(db);
    });
    models_.registerCreator("ir.attachment", [db]{
        return std::make_shared<IrAttachmentModel>(db);
    });
    models_.registerCreator("audit.log", [db]{
        return std::make_shared<AuditLog>(db);
    });
}

void IrModule::registerServices() {}
void IrModule::registerViews()    {}

// ---------------------------------------------------------------
// CSV import/export static helpers
// ---------------------------------------------------------------

std::string IrModule::buildExportFilename_(const std::string& model) {
    // Replace dots with underscores, append date suffix
    std::string safe = model;
    for (char& c : safe) if (c == '.') c = '_';
    const auto t  = std::time(nullptr);
    const auto tm = *std::gmtime(&t);
    std::ostringstream oss;
    oss << safe << "_" << std::put_time(&tm, "%Y-%m-%d") << ".csv";
    return oss.str();
}

std::vector<std::string> IrModule::splitFields_(const std::string& csv) {
    std::vector<std::string> fields;
    if (csv.empty()) return fields;
    std::istringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        // Trim whitespace
        const std::size_t a = token.find_first_not_of(" \t");
        if (a != std::string::npos) {
            const std::size_t b = token.find_last_not_of(" \t");
            fields.push_back(token.substr(a, b - a + 1));
        }
    }
    return fields;
}

// ---------------------------------------------------------------
// registerRoutes — GET /web/export/{model}  POST /web/import/{model}
// ---------------------------------------------------------------
void IrModule::registerRoutes() {
    auto db       = services_.db();
    auto sessions = services_.sessions();
    bool devMode  = services_.devMode();

    // Non-owning shared_ptr to ModelFactory — safe because Container outlives routes
    auto modelsPtr = std::shared_ptr<core::ModelFactory>(&models_, [](auto*){});

    // Returns the authenticated session, or nullopt if unauthenticated.
    // Used by both routes to enforce auth AND to build the UserContext for
    // record-rule evaluation (S-38: CSV routes must obey ir.rule restrictions).
    auto getSession = [sessions](const drogon::HttpRequestPtr& req)
            -> std::optional<infrastructure::Session> {
        if (!sessions) return std::nullopt;
        const std::string sid = req->getCookie(infrastructure::SessionManager::cookieName());
        if (sid.empty()) return std::nullopt;
        auto s = sessions->get(sid);
        if (!s.has_value() || !s->isAuthenticated()) return std::nullopt;
        return s;
    };

    // ── GET /web/export/{model} ────────────────────────────────
    // Query params: fields (comma-sep), limit (default 1000, max 1000)
    // Response: text/csv attachment
    drogon::app().registerHandler(
        "/web/export/{1}",
        [db, modelsPtr, getSession, devMode](
            const drogon::HttpRequestPtr&                      req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& modelName)
        {
            const auto sessionOpt = getSession(req);
            if (!sessionOpt) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k401Unauthorized);
                r->setBody("Unauthorized");
                cb(r); return;
            }

            // Build UserContext from the session so ir.rule restrictions apply (S-38)
            core::UserContext userCtx;
            userCtx.uid       = sessionOpt->uid;
            userCtx.companyId = sessionOpt->companyId;
            userCtx.partnerId = sessionOpt->partnerId;
            userCtx.isAdmin   = sessionOpt->isAdmin;
            userCtx.groupIds  = sessionOpt->groupIds;

            try {
                // Look up model
                if (!modelsPtr->has(modelName))
                    throw std::runtime_error("Unknown model: " + modelName);

                auto proto = modelsPtr->create(modelName, core::Lifetime::Transient);
                proto->setUserContext(userCtx);

                // Parse and validate requested fields (SEC-29)
                const std::string fieldsParam = req->getParameter("fields");
                const std::vector<std::string> requestedFields =
                    fieldsParam.empty() ? std::vector<std::string>{} :
                    IrModule::splitFields_(fieldsParam);

                const auto allFields = proto->fieldsGet();
                std::vector<std::string> validFields;
                if (requestedFields.empty()) {
                    // Default: all stored, non-computed fields
                    for (const auto& [fname, fmeta] : allFields.items()) {
                        if (fmeta.value("store", false) &&
                            !fmeta.value("compute", false) &&
                            fmeta.value("type", "") != "one2many" &&
                            fmeta.value("type", "") != "many2many")
                            validFields.push_back(fname);
                    }
                } else {
                    for (const auto& f : requestedFields) {
                        if (allFields.contains(f))
                            validFields.push_back(f);
                        // silently skip unknown field names (SEC-29 compliant)
                    }
                }
                if (validFields.empty()) {
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setStatusCode(drogon::k400BadRequest);
                    r->setBody("No valid fields specified");
                    cb(r); return;
                }

                // Pagination cap (PERF-F)
                int limit = 1000;
                const std::string limitParam = req->getParameter("limit");
                if (!limitParam.empty()) {
                    try { limit = std::min(1000, std::stoi(limitParam)); } catch (...) {}
                }

                const auto rows = proto->searchRead(
                    nlohmann::json::array(), validFields, limit, 0, "id ASC");

                // Build CSV: header row + data rows
                std::vector<std::vector<std::string>> csvRows;
                csvRows.reserve(rows.size() + 1);

                // Header
                std::vector<std::string> header;
                header.reserve(validFields.size());
                for (const auto& f : validFields) header.push_back(f);
                csvRows.push_back(std::move(header));

                // Data
                for (const auto& rec : rows) {
                    std::vector<std::string> row;
                    row.reserve(validFields.size());
                    for (const auto& f : validFields) {
                        if (!rec.contains(f) || rec[f].is_null()) {
                            row.push_back("");
                        } else if (rec[f].is_string()) {
                            row.push_back(rec[f].get<std::string>());
                        } else {
                            row.push_back(rec[f].dump());
                        }
                    }
                    csvRows.push_back(std::move(row));
                }

                const std::string csv = infrastructure::buildCsv(csvRows);
                const std::string filename = IrModule::buildExportFilename_(modelName);

                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                resp->setContentTypeString("text/csv; charset=utf-8");
                resp->addHeader("Content-Disposition",
                    "attachment; filename=\"" + filename + "\"");
                resp->setBody(csv);
                cb(resp);

            } catch (const PoolExhaustedException& ex) {
                LOG_ERROR << "[ir/export] pool: " << ex.what();
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k503ServiceUnavailable);
                r->setBody("The server is temporarily overloaded. Please retry.");
                cb(r);
            } catch (const std::exception& ex) {
                LOG_ERROR << "[ir/export] " << ex.what();
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k400BadRequest);
                r->setBody(devMode ? ex.what() : "Export failed");
                cb(r);
            }
        },
        {drogon::Get}
    );

    // ── POST /web/import/{model} ───────────────────────────────
    // Body: multipart/form-data, field "file" containing CSV content
    // Response: {"imported": N, "errors": [{"row": R, "message": "..."}]}
    drogon::app().registerHandler(
        "/web/import/{1}",
        [db, modelsPtr, getSession, devMode](
            const drogon::HttpRequestPtr&                      req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& modelName)
        {
            const auto sessionOpt = getSession(req);
            if (!sessionOpt) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k401Unauthorized);
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(nlohmann::json{{"error", "Unauthorized"}}.dump());
                cb(r); return;
            }

            // Build UserContext so ir.rule restrictions apply to imported records (S-38)
            core::UserContext userCtx;
            userCtx.uid       = sessionOpt->uid;
            userCtx.companyId = sessionOpt->companyId;
            userCtx.partnerId = sessionOpt->partnerId;
            userCtx.isAdmin   = sessionOpt->isAdmin;
            userCtx.groupIds  = sessionOpt->groupIds;

            auto jsonResp = [&cb](int code, const nlohmann::json& body) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(body.dump());
                cb(r);
            };

            try {
                if (!modelsPtr->has(modelName)) {
                    jsonResp(400, {{"error", "Unknown model: " + modelName}});
                    return;
                }

                // SEC-16: enforce 5 MB upload limit
                static constexpr std::size_t kMaxUploadBytes = 5 * 1024 * 1024;
                if (req->getBody().size() > kMaxUploadBytes) {
                    jsonResp(413, {{"error", "File too large (max 5 MB)"}});
                    return;
                }

                // Parse multipart to extract "file" field
                drogon::MultiPartParser mp;
                if (mp.parse(req) != 0) {
                    jsonResp(400, {{"error", "Invalid multipart request"}});
                    return;
                }

                std::string csvContent;
                const auto& files = mp.getFiles();
                if (!files.empty()) {
                    csvContent = std::string{files[0].fileContent()};
                } else {
                    // Accept raw body as fallback (Content-Type: text/csv)
                    csvContent = std::string(req->getBody());
                }

                if (csvContent.empty()) {
                    jsonResp(400, {{"error", "No file content received"}});
                    return;
                }

                // SEC-16: size check on extracted content too
                if (csvContent.size() > kMaxUploadBytes) {
                    jsonResp(413, {{"error", "File too large (max 5 MB)"}});
                    return;
                }

                const auto csvRows = infrastructure::parseCsv(csvContent);
                if (csvRows.empty()) {
                    jsonResp(400, {{"error", "CSV file is empty"}});
                    return;
                }

                // Row 0 = headers; validate each against FieldRegistry (SEC-29)
                const auto& headers = csvRows[0];
                if (headers.empty()) {
                    jsonResp(400, {{"error", "CSV has no header row"}});
                    return;
                }

                // Get field metadata for validation
                auto proto = modelsPtr->create(modelName, core::Lifetime::Transient);
                const auto allFields = proto->fieldsGet();

                std::vector<std::string> validHeaders;
                validHeaders.reserve(headers.size());
                for (const auto& h : headers) {
                    // Skip unknown headers silently (SEC-29)
                    if (allFields.contains(h) && h != "id")
                        validHeaders.push_back(h);
                    else
                        validHeaders.push_back("");  // placeholder = skip this column
                }

                int imported = 0;
                nlohmann::json errors = nlohmann::json::array();

                for (std::size_t rowIdx = 1; rowIdx < csvRows.size(); ++rowIdx) {
                    const auto& row = csvRows[rowIdx];

                    // Skip blank rows
                    bool allEmpty = true;
                    for (const auto& cell : row) if (!cell.empty()) { allEmpty = false; break; }
                    if (allEmpty) continue;

                    nlohmann::json values = nlohmann::json::object();
                    for (std::size_t col = 0;
                         col < headers.size() && col < row.size(); ++col)
                    {
                        if (validHeaders[col].empty()) continue;
                        values[validHeaders[col]] = row[col];
                    }

                    try {
                        auto inst = modelsPtr->create(modelName, core::Lifetime::Transient);
                        inst->setUserContext(userCtx);
                        inst->create(values);
                        ++imported;
                    } catch (const std::exception& ex) {
                        errors.push_back({
                            {"row",     static_cast<int>(rowIdx + 1)},
                            // SEC-28: gate SQL details behind devMode
                            {"message", devMode ? ex.what() : "Invalid data"}
                        });
                    }
                }

                jsonResp(200, {
                    {"imported", imported},
                    {"errors",   errors}
                });

            } catch (const PoolExhaustedException& ex) {
                LOG_ERROR << "[ir/import] pool: " << ex.what();
                jsonResp(503, {{"error", "The server is temporarily overloaded. Please retry."}});
            } catch (const std::exception& ex) {
                LOG_ERROR << "[ir/import] " << ex.what();
                jsonResp(500, {{"error", devMode ? ex.what() : "Import failed"}});
            }
        },
        {drogon::Post}
    );

    // ── POST /web/attachment/upload ────────────────────────────
    //
    // multipart/form-data: one file, plus optional form fields res_model,
    // res_id, name, description. Returns the new ir.attachment id.
    //
    // Storage is content-addressed (Filestore): the request filename never
    // reaches a path, so there is no traversal surface. A size cap and a
    // mime/extension allowlist bound what can be stored.
    drogon::app().registerHandler(
        "/web/attachment/upload",
        [db, modelsPtr, getSession, devMode](
            const drogon::HttpRequestPtr&                      req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            auto jsonResp = [&cb](int code, const nlohmann::json& body) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(body.dump());
                cb(r);
            };
            const auto sess = getSession(req);
            if (!sess) { jsonResp(401, {{"error", "Not authenticated"}}); return; }

            try {
                drogon::MultiPartParser parser;
                if (parser.parse(req) != 0 || parser.getFiles().empty()) {
                    jsonResp(400, {{"error", "No file in the upload"}});
                    return;
                }
                const auto& file  = parser.getFiles()[0];
                const std::string content(file.fileContent());

                // Size cap: 25 MB. A datasheet is a few MB; this stops a
                // single request filling the disk.
                constexpr long long kMaxBytes = 25LL * 1024 * 1024;
                if (static_cast<long long>(content.size()) > kMaxBytes) {
                    jsonResp(413, {{"error", "File exceeds the 25 MB limit"}});
                    return;
                }
                if (content.empty()) {
                    jsonResp(400, {{"error", "Empty file"}});
                    return;
                }

                // Basename only, then extension allowlist. SEC-16/SEC-19,
                // the same guard the portal proof upload uses.
                std::string base = file.getFileName();
                if (auto p = base.find_last_of("/\\"); p != std::string::npos)
                    base = base.substr(p + 1);
                std::string lower = base;
                for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                auto ends = [&](const char* ext){
                    const std::string e(ext);
                    return lower.size() > e.size() &&
                           lower.compare(lower.size() - e.size(), e.size(), e) == 0;
                };
                // Datasheets and the usual attachments. Deliberately no
                // executable/script types.
                struct Ext { const char* e; const char* mime; };
                static const std::vector<Ext> kAllowed = {
                    {".pdf","application/pdf"}, {".png","image/png"},
                    {".jpg","image/jpeg"}, {".jpeg","image/jpeg"},
                    {".gif","image/gif"}, {".svg","image/svg+xml"},
                    {".csv","text/csv"}, {".txt","text/plain"},
                    {".xlsx","application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
                    {".docx","application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
                    {".zip","application/zip"},
                };
                std::string mime;
                for (const auto& a : kAllowed) if (ends(a.e)) { mime = a.mime; break; }
                if (base.empty() || mime.empty()) {
                    jsonResp(400, {{"error",
                        "File type not allowed (pdf, png, jpg, gif, svg, csv, txt, xlsx, docx, zip)"}});
                    return;
                }

                // res_model, if given, must be a real model — no filtering
                // of arbitrary strings into the DB.
                std::string resModel = parser.getParameter<std::string>("res_model");
                int resId = 0;
                if (auto s = parser.getParameter<std::string>("res_id"); !s.empty()) {
                    try { resId = std::stoi(s); } catch (...) { resId = 0; }
                }
                std::string dispName = parser.getParameter<std::string>("name");
                if (dispName.empty()) dispName = base;
                std::string descr = parser.getParameter<std::string>("description");

                const auto stored = core::Filestore::put(content);

                // Validate res_model against the in-memory model registry,
                // not a DB table — models are registered at boot, there is
                // no ir_model table. An unknown model is dropped (the file
                // is still stored) rather than persisted as a dangling link.
                if (!resModel.empty() && !(modelsPtr && modelsPtr->has(resModel)))
                    resModel.clear();

                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                pqxx::params p;
                p.append(dispName); p.append(descr);
                if (resModel.empty()) p.append(nullptr); else p.append(resModel);
                if (resId > 0) p.append(resId); else p.append(nullptr);
                p.append(mime);
                p.append(stored.size);
                p.append(stored.checksum);
                p.append(stored.storeFname);
                p.append(sess->uid);
                auto ins = txn.exec(
                    "INSERT INTO ir_attachment "
                    "(name, description, res_model, res_id, type, mimetype, "
                    " file_size, checksum, store_fname, create_uid) "
                    "VALUES ($1,$2,$3,$4,'binary',$5,$6,$7,$8,$9) RETURNING id", p);
                const int attId = ins[0][0].as<int>();
                txn.commit();

                jsonResp(200, {{"id", attId}, {"name", dispName},
                               {"mimetype", mime}, {"file_size", stored.size},
                               {"checksum", stored.checksum}});
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[ir/attachment] pool: " << e.what();
                jsonResp(503, {{"error", "The server is temporarily overloaded. Please retry."}});
            } catch (const std::exception& e) {
                LOG_ERROR << "[ir/attachment/upload] " << e.what();
                jsonResp(500, {{"error", devMode ? e.what() : "Upload failed"}});
            }
        },
        {drogon::Post}
    );

    // ── GET /web/content/{id} ──────────────────────────────────
    //
    // Streams the file. `?download=1` forces an attachment disposition;
    // otherwise inline (so a PDF datasheet opens in the browser).
    drogon::app().registerHandler(
        "/web/content/{1}",
        [db, getSession, devMode](
            const drogon::HttpRequestPtr&                      req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& idStr)
        {
            auto err = [&cb](int code, const std::string& msg) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                r->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                r->setBody(msg);
                cb(r);
            };
            const auto sess = getSession(req);
            if (!sess) { err(401, "Not authenticated"); return; }

            int attId = 0;
            try { attId = std::stoi(idStr); } catch (...) { err(400, "Invalid id"); return; }

            try {
                std::string name, mime, storeFname;
                {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    auto r = txn.exec(
                        "SELECT name, mimetype, COALESCE(store_fname,'') "
                        "  FROM ir_attachment WHERE id = $1 AND type = 'binary'",
                        pqxx::params{attId});
                    txn.commit();
                    if (r.empty()) { err(404, "Not found"); return; }
                    name       = r[0][0].c_str();
                    mime       = r[0][1].c_str();
                    storeFname = r[0][2].c_str();
                }

                const std::string bytes = core::Filestore::get(storeFname);
                if (bytes.empty()) { err(404, "File missing from store"); return; }

                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                resp->setContentTypeString(mime);

                // S-39: the stored name is DB data — a CR/LF or quote in it
                // would break out of the header. Charset-restrict it.
                std::string safe;
                for (char c : name)
                    if (std::isalnum(static_cast<unsigned char>(c)) ||
                        c == '.' || c == '_' || c == '-' || c == ' ')
                        safe += c;
                if (safe.empty()) safe = "attachment";
                const bool download = !req->getParameter("download").empty();
                resp->addHeader("Content-Disposition",
                    std::string(download ? "attachment" : "inline") +
                    "; filename=\"" + safe + "\"");
                resp->setBody(bytes);
                cb(resp);
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[ir/content] pool: " << e.what();
                err(503, "The server is temporarily overloaded. Please retry.");
            } catch (const std::exception& e) {
                LOG_ERROR << "[ir/content] " << e.what();
                err(500, devMode ? e.what() : "An internal error occurred");
            }
        },
        {drogon::Get}
    );
}

void IrModule::registerViewModels() {
    auto db  = services_.db();
    auto& mf = models_;

    viewModels_.registerCreator("ir.ui.menu", [db]{
        return std::make_shared<IrMenuViewModel>(db);
    });
    viewModels_.registerCreator("ir.actions.act_window", [db]{
        return std::make_shared<IrActWindowViewModel>(db);
    });
    viewModels_.registerCreator("ir.model", [&mf]{
        return std::make_shared<IrModelViewModel>(
            std::shared_ptr<core::ModelFactory>(&mf, [](auto*){}));
    });
    viewModels_.registerCreator("decimal.precision", [db]{
        return std::make_shared<DecimalPrecisionViewModel>(db);
    });
    viewModels_.registerCreator("ir.config.parameter", [db]{
        return std::make_shared<core::GenericViewModel<IrConfigParameter>>(db);
    });
    viewModels_.registerCreator("ir.model.data", [db]{
        return std::make_shared<core::GenericViewModel<IrModelDataModel>>(db);
    });
    viewModels_.registerCreator("ir.attachment", [db]{
        return std::make_shared<IrAttachmentViewModel>(db);
    });
    viewModels_.registerCreator("audit.log", [db]{
        return std::make_shared<AuditLogViewModel>(db);
    });
}

void IrModule::registerMigrations(infrastructure::MigrationRunner& runner) {
    // v1: audit_log table for audit trail (P0 Feature 5)
    runner.registerMigration({1, "create_audit_log",
        R"(
            CREATE TABLE IF NOT EXISTS audit_log (
                id          SERIAL  PRIMARY KEY,
                model       VARCHAR NOT NULL,
                operation   VARCHAR NOT NULL,
                record_ids  INTEGER[] NOT NULL DEFAULT '{}',
                uid         INTEGER NOT NULL DEFAULT 0,
                created_at  TIMESTAMP NOT NULL DEFAULT now()
            );
            CREATE INDEX IF NOT EXISTS audit_log_model_idx     ON audit_log (model);
            CREATE INDEX IF NOT EXISTS audit_log_uid_idx       ON audit_log (uid);
            CREATE INDEX IF NOT EXISTS audit_log_created_idx   ON audit_log (created_at DESC);
        )"
    });

    // --------------------------------------------------------
    // 1020 — the customer-invoice sequence
    //
    // One continuous series for every out_invoice / out_refund, whatever
    // journal posts it: INV000001, INV000002, … So prefix "INV", padding
    // 6, reset 'never' (a global counter, not a per-year one). Both
    // AccountModule::handleActionPost and RentalBilling draw from this
    // code; neither creates it, so the definition lives in exactly one
    // place.
    //
    // ON CONFLICT keeps a re-run and a hand-edited prefix from fighting:
    // if an operator later changes the prefix or padding in the UI, this
    // migration will not stamp it back.
    runner.registerMigration({1020, "invoice_sequence_INV", R"SQL(
        INSERT INTO ir_sequence (code, name, prefix, padding, reset_policy, number_next)
        VALUES ('account.move.INV', 'Customer Invoice', 'INV', 6, 'never', 1)
        ON CONFLICT (code) WHERE company_id IS NULL DO NOTHING;
    )SQL"});

    // --------------------------------------------------------
    // 1021 — the reverse-invoice (credit note) sequence
    //
    // A customer credit note (out_refund) is a "reverse invoice": its own
    // continuous series with prefix "RINV" (RINV000001, …), separate from INV
    // so a credit note is recognisable on sight. AccountModule::handleActionPost
    // draws out_refund numbers from here instead of the INV series.
    runner.registerMigration({1021, "reverse_invoice_sequence_RINV", R"SQL(
        INSERT INTO ir_sequence (code, name, prefix, padding, reset_policy, number_next)
        VALUES ('account.move.RINV', 'Reverse Invoice (Credit Note)', 'RINV', 6, 'never', 1)
        ON CONFLICT (code) WHERE company_id IS NULL DO NOTHING;
    )SQL"});

    // --------------------------------------------------------
    // 1030 — ir.model.data (external identifiers)
    //
    // Maps a stable "module.name" xml_id to a concrete (model, res_id).
    // Lets seed data and cross-references survive id renumbering, and is
    // the standard Odoo mechanism a lot of later features assume exists.
    //
    // noupdate: when true, a re-seed must NOT overwrite the row — the
    // record has been edited by hand and the seed is only a starting
    // point. IrModelData::ensure honours it.
    runner.registerMigration({1030, "ir_model_data", R"SQL(
        CREATE TABLE IF NOT EXISTS ir_model_data (
            id          SERIAL PRIMARY KEY,
            module      TEXT    NOT NULL,
            name        TEXT    NOT NULL,
            model       TEXT    NOT NULL,
            res_id      INTEGER NOT NULL,
            noupdate    BOOLEAN NOT NULL DEFAULT FALSE,
            create_date TIMESTAMP NOT NULL DEFAULT now(),
            write_date  TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT ir_model_data_xmlid_uniq UNIQUE (module, name)
        );
        -- The reverse lookup "what is the xml_id of this record" must be
        -- fast too, for export and for uninstall bookkeeping.
        CREATE INDEX IF NOT EXISTS ir_model_data_record_idx
            ON ir_model_data(model, res_id);
    )SQL"});

    // --------------------------------------------------------
    // 1040 — ir.attachment (files: datasheets, receipts, exports)
    //
    // Metadata lives here; the BYTES live on the filesystem under the
    // filestore, addressed by content hash. Same split Odoo uses, and the
    // same one the existing payment_proof table follows — chosen over a
    // DB bytea column so a 20 MB datasheet does not bloat every backup and
    // every SELECT of the row.
    //
    //   store_fname  filestore-relative path, `<h[:2]>/<sha256>`
    //   checksum     sha256 of the content — also the dedup key
    //   file_size    bytes, so a list view need not stat the file
    //   res_model/res_id  what this is attached to (nullable: a stray
    //                     upload not yet linked is still a valid row)
    //
    // res_field is carried for parity with Odoo (an attachment that backs
    // a specific binary field) but nothing writes it yet.
    runner.registerMigration({1040, "ir_attachment", R"SQL(
        CREATE TABLE IF NOT EXISTS ir_attachment (
            id           SERIAL PRIMARY KEY,
            name         TEXT    NOT NULL,
            description  TEXT    NOT NULL DEFAULT '',
            res_model    TEXT,
            res_id       INTEGER,
            res_field    TEXT,
            type         TEXT    NOT NULL DEFAULT 'binary',
            url          TEXT,
            mimetype     TEXT    NOT NULL DEFAULT 'application/octet-stream',
            file_size    BIGINT  NOT NULL DEFAULT 0,
            checksum     TEXT,
            store_fname  TEXT,
            public       BOOLEAN NOT NULL DEFAULT FALSE,
            company_id   INTEGER NOT NULL DEFAULT 1,
            create_uid   INTEGER NOT NULL DEFAULT 0,
            create_date  TIMESTAMP NOT NULL DEFAULT now(),
            write_date   TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT ir_attachment_type_chk CHECK (type IN ('binary','url'))
        );
        CREATE INDEX IF NOT EXISTS ir_attachment_res_idx
            ON ir_attachment(res_model, res_id);
        CREATE INDEX IF NOT EXISTS ir_attachment_checksum_idx
            ON ir_attachment(checksum);
    )SQL"});
}

void IrModule::initialize() {
    ensureSchema_();
    seedActions_();
    seedMenus_();
    seedConfigParams_();
    seedRules_();
    // S-30: start the rule engine so BaseModel can enforce record-level rules
    core::RuleEngine::initialize(services_.db());
    // Audit trail: initialize after schema is ready
    infrastructure::AuditService::initialize(services_.db());
    // P2: display precision, read lazily and cached (docs/048 §2.1).
    // Safe to initialize here even though migration 901 creates the table
    // later in the same boot — the first digits() call is lazy, and a
    // missing table falls back to the caller's default rather than throwing.
    core::DecimalPrecision::initialize(services_.db());
    core::IrSequence::initialize(services_.db());   // P4
    core::IrCron::initialize(services_.db());       // P5
}

void IrModule::ensureSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS ir_act_window (
            id        SERIAL  PRIMARY KEY,
            name      VARCHAR NOT NULL,
            res_model VARCHAR NOT NULL,
            view_mode VARCHAR NOT NULL DEFAULT 'list,form',
            domain    VARCHAR,
            context   VARCHAR NOT NULL DEFAULT '{}',
            target    VARCHAR NOT NULL DEFAULT 'current',
            path      VARCHAR UNIQUE,
            help      TEXT,
            active    BOOLEAN NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS ir_ui_menu (
            id        SERIAL  PRIMARY KEY,
            name      VARCHAR NOT NULL,
            parent_id INTEGER REFERENCES ir_ui_menu(id) ON DELETE CASCADE,
            sequence  INTEGER NOT NULL DEFAULT 10,
            action_id INTEGER REFERENCES ir_act_window(id) ON DELETE SET NULL,
            web_icon  VARCHAR,
            active    BOOLEAN NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS ir_config_parameter (
            id         SERIAL  PRIMARY KEY,
            key        VARCHAR NOT NULL UNIQUE,
            value      TEXT    NOT NULL DEFAULT '',
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");

    // S-30: Record-level authorization tables
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS ir_rule (
            id           SERIAL  PRIMARY KEY,
            name         VARCHAR(128) NOT NULL,
            model_name   VARCHAR(128) NOT NULL,
            domain_force JSONB   NOT NULL DEFAULT '[]',
            perm_read    BOOLEAN NOT NULL DEFAULT TRUE,
            perm_write   BOOLEAN NOT NULL DEFAULT TRUE,
            perm_create  BOOLEAN NOT NULL DEFAULT TRUE,
            perm_unlink  BOOLEAN NOT NULL DEFAULT TRUE,
            global       BOOLEAN NOT NULL DEFAULT TRUE,
            active       BOOLEAN NOT NULL DEFAULT FALSE,
            create_date  TIMESTAMP DEFAULT now(),
            write_date   TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE INDEX IF NOT EXISTS ir_rule_model_idx ON ir_rule (model_name)
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS ir_rule_group_rel (
            rule_id  INTEGER NOT NULL REFERENCES ir_rule(id) ON DELETE CASCADE,
            group_id INTEGER NOT NULL,
            PRIMARY KEY (rule_id, group_id)
        )
    )");

    txn.commit();
}

void IrModule::seedActions_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, path, context) VALUES
            (1, 'Contacts',  'res.partner', 'list,form', 'contacts',  '{}'),
            (2, 'Users',     'res.users',   'list,form', 'users',     '{}'),
            (3, 'Companies', 'res.company', 'list,form', 'companies', '{}')
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
    txn.commit();
}

void IrModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec("DELETE FROM ir_ui_menu WHERE id < 10");
    txn.exec("DELETE FROM ir_ui_menu WHERE id=33");

    // The app roots are structural: they must survive another module accidentally
    // seeding the same ir_ui_menu id. DO UPDATE (not DO NOTHING) so a database
    // whose root was overwritten self-heals on the next start — this happened to
    // the Settings root (id 30) and it removed the whole Settings app from the
    // home screen. See docs/089.
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id, web_icon) VALUES
            (10, 'Accounting', NULL, 10, NULL, 'accounting'),
            (20, 'Contacts',   NULL, 20, NULL, 'contacts'),
            (30, 'Settings',   NULL, 30, NULL, 'settings')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id,
                web_icon=EXCLUDED.web_icon
    )");

    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (21, 'Contacts', 20, 10, 1)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    // Same reasoning as the app roots above: restore these if something else
    // claimed the id (id 32 was overwritten by a Budgetary Positions menu).
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (31, 'Users',        30, 10, 2),
            (32, 'Companies',    30, 20, 3)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");
    txn.commit();
}

void IrModule::seedConfigParams_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        INSERT INTO ir_config_parameter (key, value) VALUES
            ('web.base.url',           'http://localhost:8069'),
            ('auth_signup.allow',      'True'),
            ('auth_signup.reset_pwd',  'True'),
            ('database.uuid',          gen_random_uuid()::text)
        ON CONFLICT (key) DO NOTHING
    )");
    txn.commit();
}

void IrModule::seedRules_() {
    // S-30: Seed example record rules.
    //
    // Rules are seeded with active=FALSE so existing behaviour is completely
    // unchanged on first upgrade.  An administrator can activate individual
    // rules by running:
    //   UPDATE ir_rule SET active=TRUE WHERE id = <id>;
    //
    // global=TRUE  → subtractive: all users (non-admin) must satisfy the rule.
    // global=FALSE → additive:    only users in ir_rule_group_rel are restricted;
    //                             add rows to ir_rule_group_rel to bind a rule to
    //                             specific group ids.
    //
    // Variable tokens in domain_force:
    //   "user.id"         → session user id
    //   "user.company_id" → session company id
    //   "user.partner_id" → session partner id
    //
    // Admin users (res_users.groups containing Administrator id=3) bypass ALL rules
    // regardless of active/global settings.

    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        INSERT INTO ir_rule
            (id, name, model_name, domain_force,
             perm_read, perm_write, perm_create, perm_unlink,
             global, active)
        VALUES
        -- sale.order: restrict non-admin users to orders assigned to them
        (1, 'Sale Order: Personal Orders',
            'sale.order',
            '[["user_id","=","user.id"]]',
            TRUE, TRUE, TRUE, TRUE,
            TRUE, FALSE),

        -- purchase.order: restrict to own purchase requests
        (2, 'Purchase Order: Personal RFQs',
            'purchase.order',
            '[["user_id","=","user.id"]]',
            TRUE, TRUE, TRUE, TRUE,
            TRUE, FALSE),

        -- account.move: restrict to invoices assigned to current user
        (3, 'Account Move: Own Invoices',
            'account.move',
            '[["invoice_user_id","=","user.id"]]',
            TRUE, TRUE, TRUE, TRUE,
            TRUE, FALSE),

        -- hr.employee: employees see only their own record
        (4, 'HR Employee: See Own Record',
            'hr.employee',
            '[["user_id","=","user.id"]]',
            TRUE, FALSE, FALSE, FALSE,
            TRUE, FALSE),

        -- stock.picking: restrict to pickings for user''s company
        (5, 'Stock Picking: Own Company',
            'stock.picking',
            '[["company_id","=","user.company_id"]]',
            TRUE, TRUE, TRUE, TRUE,
            TRUE, FALSE)

        ON CONFLICT (id) DO NOTHING
    )");

    txn.exec("SELECT setval('ir_rule_id_seq', (SELECT MAX(id) FROM ir_rule), true)");
    txn.commit();
}

} // namespace odoo::modules::ir

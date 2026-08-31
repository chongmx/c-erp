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
#include "HttpClient.hpp"
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
#include <set>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <cstdlib>

namespace cerp::modules::ir {

using namespace cerp::infrastructure;
using namespace cerp::core;

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
        // docs/106 — what the file IS, not what bytes it holds. A fabrication
        // package is a dozen files called top.gtl / bot.gbl; without this the
        // panel can only ever be a directory listing.
        fieldRegistry_.add({"document_type", core::FieldType::Char,  "Document Type"});
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
// docs/106 — classify an attachment from its filename.
//
// Auto-classification is the default because nobody labels sixteen Gerber layers
// by hand, and an unlabelled fabrication package is exactly the pile this feature
// exists to organise. An explicit document_type always overrides it, so the guess
// is a starting point rather than a verdict.
//
// Ambiguous extensions deliberately fall through to "document": a PDF may be a
// datasheet, an assembly drawing or a test report, and guessing between those is
// worse than leaving it for a person to say.
static std::string classifyDocument(const std::string& lowerName) {
    struct Rule { const char* ext; const char* type; };
    static const Rule kRules[] = {
        // fabrication
        {".gbr","gerber"}, {".ger","gerber"}, {".gtl","gerber"}, {".gbl","gerber"},
        {".gto","gerber"}, {".gbo","gerber"}, {".gts","gerber"}, {".gbs","gerber"},
        {".gm1","gerber"}, {".gko","gerber"}, {".gbp","gerber"}, {".gtp","gerber"},
        {".gpt","gerber"}, {".gpb","gerber"},
        {".drl","drill"},  {".xln","drill"},  {".drd","drill"},  {".tap","drill"},
        {".pos","placement"}, {".xy","placement"},
        // design source
        {".kicad_pcb","pcb-design"}, {".brd","pcb-design"},
        {".kicad_sch","schematic"},  {".sch","schematic"},
        {".net","netlist"},
        // mechanical
        {".step","3d-model"}, {".stp","3d-model"}, {".iges","3d-model"},
        {".igs","3d-model"},  {".stl","3d-model"}, {".3mf","3d-model"},
        {".dxf","drawing"},
        // generic
        {".png","image"}, {".jpg","image"}, {".jpeg","image"},
        {".gif","image"}, {".svg","image"},
        {".csv","data"},  {".xlsx","data"},
        {".zip","archive"},
    };
    auto ends = [&](const char* e) {
        const std::string x(e);
        return lowerName.size() > x.size() &&
               lowerName.compare(lowerName.size() - x.size(), x.size(), x) == 0;
    };
    // .kicad_pcb must be tested before .pcb-like suffixes; the table order does
    // that, and the first match wins.
    for (const auto& r : kRules) if (ends(r.ext)) return r.type;
    return "document";
}

/// The vocabulary the UI groups by. A value outside it is rejected rather than
/// stored, so a typo cannot quietly create a group of one.
static bool documentTypeAllowed(const std::string& t) {
    static const std::set<std::string> k = {
        "gerber","drill","placement","pcb-design","schematic","netlist",
        "3d-model","drawing","datasheet","specification","image","data",
        "archive","document","other"
    };
    return k.count(t) > 0;
}

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
                // (classifyDocument / documentTypeAllowed are defined above.)
                //
                // Datasheets, the usual attachments, and manufacturing data
                // (docs/106). Deliberately no executable or script types — the
                // allowlist is the control, so it is extended by naming formats
                // rather than by loosening the rule.
                //
                // Every entry below is inert data: Gerber, Excellon drill, IPC
                // pick-and-place, STEP/DXF/STL geometry and EDA project files
                // are read by fabrication tools, never executed by the server or
                // the browser. They are served as application/octet-stream so a
                // browser downloads them instead of trying to render them.
                struct Ext { const char* e; const char* mime; };
                static const std::vector<Ext> kAllowed = {
                    // documents and images
                    {".pdf","application/pdf"}, {".png","image/png"},
                    {".jpg","image/jpeg"}, {".jpeg","image/jpeg"},
                    {".gif","image/gif"}, {".svg","image/svg+xml"},
                    {".csv","text/csv"}, {".txt","text/plain"},
                    {".xlsx","application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
                    {".docx","application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
                    {".zip","application/zip"},
                    // Gerber — the fab data itself. Extended (.gbr/.ger) and the
                    // per-layer conventions Altium and KiCad emit.
                    {".gbr","application/octet-stream"}, {".ger","application/octet-stream"},
                    {".gbl","application/octet-stream"}, {".gtl","application/octet-stream"},
                    {".gbs","application/octet-stream"}, {".gts","application/octet-stream"},
                    {".gbo","application/octet-stream"}, {".gto","application/octet-stream"},
                    {".gm1","application/octet-stream"}, {".gko","application/octet-stream"},
                    {".gbp","application/octet-stream"}, {".gtp","application/octet-stream"},
                    {".gpt","application/octet-stream"}, {".gpb","application/octet-stream"},
                    // Excellon drill / route
                    {".drl","application/octet-stream"}, {".xln","application/octet-stream"},
                    {".drd","application/octet-stream"}, {".tap","application/octet-stream"},
                    // assembly / placement
                    {".pos","text/plain"}, {".xy","text/plain"},
                    // mechanical geometry
                    {".step","application/octet-stream"}, {".stp","application/octet-stream"},
                    {".iges","application/octet-stream"}, {".igs","application/octet-stream"},
                    {".stl","application/octet-stream"}, {".dxf","application/octet-stream"},
                    {".3mf","application/octet-stream"},
                    // EDA project files
                    {".kicad_pcb","application/octet-stream"},
                    {".kicad_sch","application/octet-stream"},
                    {".sch","application/octet-stream"}, {".brd","application/octet-stream"},
                    {".net","text/plain"},
                };
                std::string mime;
                for (const auto& a : kAllowed) if (ends(a.e)) { mime = a.mime; break; }
                if (base.empty() || mime.empty()) {
                    jsonResp(400, {{"error",
                        "File type not allowed. Documents: pdf, png, jpg, gif, svg, csv, txt, "
                        "xlsx, docx, zip. Manufacturing: gerber (gbr/ger/gtl/gbl/...), drill "
                        "(drl/xln), placement (pos/xy), geometry (step/stp/stl/dxf/iges), "
                        "EDA (kicad_pcb/kicad_sch/sch/brd/net)."}});
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
                // docs/106 — an explicit document_type wins; otherwise classify
                // from the filename. Nobody labels sixteen Gerber layers by hand.
                // parser.getParameter, not req->getParameter: this is a multipart
                // body, and req->getParameter only sees the query string — so the
                // override was silently ignored and every file fell back to the
                // guess. The neighbouring fields all read it the same way.
                std::string docType = parser.getParameter<std::string>("document_type");
                if (docType.empty() || !documentTypeAllowed(docType))
                    docType = classifyDocument(lower);
                p.append(docType);
                auto ins = txn.exec(
                    "INSERT INTO ir_attachment "
                    "(name, description, res_model, res_id, type, mimetype, "
                    " file_size, checksum, store_fname, create_uid, document_type) "
                    "VALUES ($1,$2,$3,$4,'binary',$5,$6,$7,$8,$9,$10) RETURNING id", p);
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


// ================================================================
// ir.ai.settings — AI agent configuration (docs/110)
//
// THE RULE THIS CLASS EXISTS TO ENFORCE: the API key never leaves the server
// through a normal read. `read` and `search_read` return `configured` and the
// last four characters, never the value. There is exactly one path that
// returns it -- reveal_for_setup -- and it is admin-only and audited, because
// the operator sometimes genuinely has to paste it into a systemd unit.
//
// Every method here is admin-only. A settings row holding a live credential is
// not something a portal user should be able to enumerate.
// ================================================================
class IrAiSettingsViewModel : public core::BaseViewModel {
public:
    explicit IrAiSettingsViewModel(std::shared_ptr<DbConnection> db)
        : db_(std::move(db))
    {
        REGISTER_METHOD("search_read",      handleRead)
        REGISTER_METHOD("web_search_read",  handleRead)
        REGISTER_METHOD("read",             handleRead)
        REGISTER_METHOD("web_read",         handleRead)
        REGISTER_METHOD("get",              handleGet)
        REGISTER_MUTATOR("save",            handleSave)
        REGISTER_MUTATOR("clear_key",       handleClearKey)
        REGISTER_METHOD("reveal_for_setup", handleReveal)
        REGISTER_METHOD("test_connection",  handleTest)
        REGISTER_METHOD("providers",        handleProviders)
        REGISTER_METHOD("ask",              handleAsk)
        REGISTER_METHOD("ask_help",         handleAskHelp)
        REGISTER_METHOD("map_bom_headers",  handleMapBomHeaders)
        REGISTER_METHOD("clean_bom_rows",   handleCleanBomRows)
        REGISTER_METHOD("prompts",          handlePrompts)
        REGISTER_MUTATOR("save_prompt",     handleSavePrompt)
        REGISTER_MUTATOR("reset_prompt",    handleResetPrompt)
        REGISTER_METHOD("status",           handleStatus)
        REGISTER_MUTATOR("save_provider",   handleSaveProvider)
        REGISTER_METHOD("fields_get",       handleFieldsGet)
    }

    std::string modelName() const override { return "ir.ai.settings"; }

private:
    std::shared_ptr<DbConnection> db_;

    void requireAdmin_(const core::CallKwArgs& call) {
        const auto ctx = extractContext_(call);
        if (!ctx.isAdmin)
            throw cerp::infrastructure::AccessDeniedError(
                "AI settings are administrator-only.");
    }

    // The only shape a client is ever given. `configured` answers "is a key
    // set" without answering "what is it", and the tail is enough for a human
    // to tell two keys apart.
    static nlohmann::json publicRow_(const pqxx::row& r) {
        const std::string key = r["api_key"].c_str();
        return {
            {"id",                r["id"].as<int>()},
            {"enabled",           r["enabled"].as<bool>(false)},
            {"provider",          r["provider"].c_str()},
            {"configured",        !key.empty()},
            {"key_tail",          key.size() >= 4 ? key.substr(key.size() - 4) : std::string{}},
            {"model",             r["model"].c_str()},
            {"max_output_tokens", r["max_output_tokens"].as<int>(0)},
            {"daily_call_cap",    r["daily_call_cap"].as<int>(0)},
            {"calls_today",       r["calls_today"].as<int>(0)},
            {"last_ok_at",        r["last_ok_at"].is_null()  ? "" : r["last_ok_at"].c_str()},
            {"last_error",        r["last_error"].c_str()},
            {"api_base_url",      r["api_base_url"].c_str()},
            {"workspace_id",      r["workspace_id"].c_str()},
            {"web_search",        r["web_search"].as<bool>(true)},
            {"max_candidates",    r["max_candidates"].as<int>(3)},
            {"tls_available",     cerp::infrastructure::HttpClient::tlsAvailable()},
        };
    }

    pqxx::row row_(pqxx::work& txn) {
        auto r = txn.exec("SELECT * FROM ir_ai_settings WHERE id=1");
        if (r.empty()) {
            // The singleton is created by ensureSchema_(); this only covers a
            // database where the row was deleted by hand. It deliberately does
            // NOT re-declare the schema — a second copy of the DDL here would
            // drift from ensureSchema_() and, having done so once already,
            // would quietly create the provider table without its newer
            // columns. There is one place that defines these tables.
            txn.exec("INSERT INTO ir_ai_settings (id) VALUES (1) ON CONFLICT (id) DO NOTHING");
            r = txn.exec("SELECT * FROM ir_ai_settings WHERE id=1");
        }
        return r[0];
    }

    nlohmann::json handleGet(const core::CallKwArgs& call) {
        requireAdmin_(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        return publicRow_(row_(txn));
    }

    nlohmann::json handleRead(const core::CallKwArgs& call) {
        requireAdmin_(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        nlohmann::json out = nlohmann::json::array();
        out.push_back(publicRow_(row_(txn)));
        return out;
    }

    nlohmann::json handleSave(const core::CallKwArgs& call) {
        requireAdmin_(call);
        const auto v = call.arg(0);
        if (!v.is_object())
            throw cerp::infrastructure::ValidationError("save: expected an object of values.");

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        if (v.contains("enabled") && v["enabled"].is_boolean())
            txn.exec("UPDATE ir_ai_settings SET enabled=$1, write_date=now() WHERE id=1",
                     pqxx::params{v["enabled"].get<bool>()});
        if (v.contains("provider") && v["provider"].is_string()) {
            const std::string p = v["provider"].get<std::string>();
            auto known = txn.exec("SELECT 1 FROM ir_ai_provider WHERE name=$1", pqxx::params{p});
            if (known.empty())
                throw cerp::infrastructure::ValidationError("Unknown provider '" + p + "'.");
            txn.exec("UPDATE ir_ai_settings SET provider=$1, write_date=now() WHERE id=1",
                     pqxx::params{p});
        }
        if (v.contains("model") && v["model"].is_string())
            txn.exec("UPDATE ir_ai_settings SET model=$1, write_date=now() WHERE id=1",
                     pqxx::params{v["model"].get<std::string>()});
        if (v.contains("max_output_tokens") && v["max_output_tokens"].is_number_integer())
            txn.exec("UPDATE ir_ai_settings SET max_output_tokens=$1, write_date=now() WHERE id=1",
                     pqxx::params{v["max_output_tokens"].get<int>()});
        if (v.contains("daily_call_cap") && v["daily_call_cap"].is_number_integer())
            txn.exec("UPDATE ir_ai_settings SET daily_call_cap=$1, write_date=now() WHERE id=1",
                     pqxx::params{v["daily_call_cap"].get<int>()});
        if (v.contains("workspace_id") && v["workspace_id"].is_string())
            txn.exec("UPDATE ir_ai_settings SET workspace_id=$1, write_date=now() WHERE id=1",
                     pqxx::params{v["workspace_id"].get<std::string>()});
        if (v.contains("web_search") && v["web_search"].is_boolean())
            txn.exec("UPDATE ir_ai_settings SET web_search=$1, write_date=now() WHERE id=1",
                     pqxx::params{v["web_search"].get<bool>()});
        if (v.contains("max_candidates") && v["max_candidates"].is_number_integer()) {
            const int n = v["max_candidates"].get<int>();
            if (n < 1 || n > 8)
                throw cerp::infrastructure::ValidationError(
                    "Candidates must be between 1 and 8.");
            txn.exec("UPDATE ir_ai_settings SET max_candidates=$1, write_date=now() WHERE id=1",
                     pqxx::params{n});
        }
        if (v.contains("api_base_url") && v["api_base_url"].is_string()) {
            const std::string u = v["api_base_url"].get<std::string>();
            // Only a scheme+host belongs here. A path would be silently
            // ignored by the client and look like a configuration that works.
            if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0)
                throw cerp::infrastructure::ValidationError(
                    "The base URL must start with http:// or https://.");
            txn.exec("UPDATE ir_ai_settings SET api_base_url=$1, write_date=now() WHERE id=1",
                     pqxx::params{u});
        }

        // The key is write-only: sent to be stored, never sent back. An empty
        // string means "leave it alone", so a save from a screen that has
        // never seen the key cannot wipe it.
        if (v.contains("api_key") && v["api_key"].is_string()) {
            const std::string k = v["api_key"].get<std::string>();
            if (!k.empty())
                txn.exec("UPDATE ir_ai_settings SET api_key=$1, last_error='', write_date=now() WHERE id=1",
                         pqxx::params{k});
        }

        auto out = publicRow_(row_(txn));
        txn.commit();
        if (AuditService::ready())
            AuditService::instance().log("ir.ai.settings", "save",
                                         std::vector<int>{1}, extractContext_(call).uid);
        return out;
    }

    nlohmann::json handleClearKey(const core::CallKwArgs& call) {
        requireAdmin_(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec("UPDATE ir_ai_settings SET api_key='', enabled=FALSE, write_date=now() WHERE id=1");
        auto out = publicRow_(row_(txn));
        txn.commit();
        if (AuditService::ready())
            AuditService::instance().log("ir.ai.settings", "clear_key",
                                         std::vector<int>{1}, extractContext_(call).uid);
        return out;
    }

    // The single exception to "the key never leaves the server", and the
    // reason it is separate, admin-only and audited: a credential that can be
    // revealed without a trace is one nobody can reason about afterwards.
    nlohmann::json handleReveal(const core::CallKwArgs& call) {
        requireAdmin_(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto r = row_(txn);
        const std::string key = r["api_key"].c_str();
        if (key.empty())
            throw cerp::infrastructure::ValidationError("No key is configured.");
        txn.commit();
        if (AuditService::ready())
            AuditService::instance().log("ir.ai.settings", "reveal_for_setup",
                                         std::vector<int>{1}, extractContext_(call).uid);
        LOG_INFO << "[ir.ai.settings] key revealed for setup by uid="
                 << extractContext_(call).uid;
        return {
            {"api_key", key},
            {"systemd", "Environment=\"ANTHROPIC_API_KEY=" + key + "\""},
            {"docker",  "-e ANTHROPIC_API_KEY=" + key},
            {"shell",   "export ANTHROPIC_API_KEY=" + key},
        };
    }

    // Step 2 of docs/110 §6. `mock` answers locally so the suite never needs a
    // key and never touches the network; `anthropic` reports what is missing
    // until the outbound client lands.
    nlohmann::json handleTest(const core::CallKwArgs& call) {
        requireAdmin_(call);
        std::string provider, key, base, path, model, style, wsid;
        {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto s0 = row_(txn);
            provider = s0["provider"].c_str();
            auto p = txn.exec("SELECT * FROM ir_ai_provider WHERE name=$1", pqxx::params{provider});
            if (p.empty())
                throw cerp::infrastructure::ValidationError("Provider '" + provider + "' is not configured.");
            key   = p[0]["api_key"].c_str();
            base  = p[0]["base_url"].c_str();
            path  = p[0]["path"].c_str();
            model = p[0]["model"].c_str();
            style = p[0]["auth_style"].c_str();
            wsid  = p[0]["workspace_id"].c_str();
        }   // the connection is released before the network call: it must not
            // be held for the length of an API round trip.

        if (style == "none")
            return {{"ok", true}, {"provider", provider},
                    {"detail", "Mock provider answered locally. No network call was made."}};
        if (key.empty())
            return {{"ok", false}, {"provider", provider},
                    {"detail", "No API key is configured for " + provider + "."}};
        if (!cerp::infrastructure::HttpClient::tlsAvailable() && base.rfind("https://", 0) == 0)
            return {{"ok", false}, {"provider", provider},
                    {"detail", "This build cannot make HTTPS calls (drogon has no TLS)."}};

        nlohmann::json payload;
        std::vector<std::pair<std::string, std::string>> headers;
        if (style == "anthropic") {
            payload = {{"model", model}, {"max_tokens", 16},
                       {"messages", nlohmann::json::array({
                           {{"role","user"},{"content","Reply with the single word: ok"}}})}};
            headers = {{"x-api-key", key}, {"anthropic-version", "2023-06-01"}};
            if (!wsid.empty()) headers.emplace_back("anthropic-workspace-id", wsid);
        } else {
            // OpenAI-compatible, which is what xAI speaks.
            payload = {{"model", model}, {"max_tokens", 16},
                       {"messages", nlohmann::json::array({
                           {{"role","user"},{"content","Reply with the single word: ok"}}})}};
            headers = {{"Authorization", "Bearer " + key}};
        }

        auto res = cerp::infrastructure::HttpClient::postJson(base, path, payload.dump(), headers, 25.0);

        bool ok = res.ok;
        std::string detail;
        if (res.ok) {
            // Report the model the service ACTUALLY used: xAI resolves aliases
            // server-side, so asking for grok-3 can answer as grok-4.3, and
            // silently reporting the requested name would be a small lie.
            std::string used = model;
            try {
                auto j = nlohmann::json::parse(res.body, nullptr, false);
                if (!j.is_discarded() && j.contains("model") && j["model"].is_string())
                    used = j["model"].get<std::string>();
            } catch (...) {}
            detail = "Connected. " + used + " answered.";
        } else if (res.status == 401 || res.status == 403) {
            detail = "The service rejected the key (HTTP " + std::to_string(res.status) + ").";
        } else if (res.status == 429) {
            detail = "Rate limited (HTTP 429). The key is valid."; ok = true;
        } else if (res.status > 0) {
            detail = "The service replied " + std::to_string(res.status) + ".";
            try {
                auto j = nlohmann::json::parse(res.body, nullptr, false);
                if (!j.is_discarded() && j.contains("error")) {
                    const auto& e = j["error"];
                    if (e.is_object() && e.contains("message"))  detail += " " + e["message"].get<std::string>();
                    else if (e.is_string())                      detail += " " + e.get<std::string>();
                }
            } catch (...) {}
        } else {
            detail = res.error;
        }

        {
            auto c2 = db_->acquire();
            pqxx::work t2{c2.get()};
            if (ok) t2.exec("UPDATE ir_ai_settings SET last_ok_at=now(), last_error='' WHERE id=1");
            else    t2.exec("UPDATE ir_ai_settings SET last_error=$1 WHERE id=1", pqxx::params{detail});
            t2.commit();
        }
        return {{"ok", ok}, {"provider", provider}, {"model", model},
                {"status", res.status}, {"detail", detail}};
    }

    // The provider list, with every key masked the same way the active one is.
    // Switching provider must not mean re-entering a key, so all of them are
    // stored -- which means all of them need the same masking.
    nlohmann::json handleProviders(const core::CallKwArgs& call) {
        requireAdmin_(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto rows = txn.exec("SELECT name, label, api_key, base_url, model, auth_style, "
                             "workspace_id FROM ir_ai_provider ORDER BY name");
        nlohmann::json out = nlohmann::json::array();
        for (const auto& r : rows) {
            const std::string k = r["api_key"].c_str();
            out.push_back({
                {"name",         r["name"].c_str()},
                {"label",        r["label"].c_str()},
                {"configured",   !k.empty()},
                {"key_tail",     k.size() >= 4 ? k.substr(k.size() - 4) : std::string{}},
                {"base_url",     r["base_url"].c_str()},
                {"model",        r["model"].c_str()},
                {"auth_style",   r["auth_style"].c_str()},
                {"workspace_id", r["workspace_id"].c_str()},
            });
        }
        return out;
    }

    nlohmann::json handleSaveProvider(const core::CallKwArgs& call) {
        requireAdmin_(call);
        const auto v = call.arg(0);
        if (!v.is_object() || !v.contains("name"))
            throw cerp::infrastructure::ValidationError("save_provider: a provider name is required.");
        const std::string name = v["name"].get<std::string>();
        {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto exists = txn.exec("SELECT 1 FROM ir_ai_provider WHERE name=$1", pqxx::params{name});
            if (exists.empty())
                throw cerp::infrastructure::ValidationError("No such provider: " + name);

            // Empty means "leave it": the screen never receives a key, so it
            // cannot send one back, and a blank field must not wipe storage.
            if (v.contains("api_key") && v["api_key"].is_string() && !v["api_key"].get<std::string>().empty())
                txn.exec("UPDATE ir_ai_provider SET api_key=$1, write_date=now() WHERE name=$2",
                         pqxx::params{v["api_key"].get<std::string>(), name});
            if (v.contains("model") && v["model"].is_string())
                txn.exec("UPDATE ir_ai_provider SET model=$1, write_date=now() WHERE name=$2",
                         pqxx::params{v["model"].get<std::string>(), name});
            if (v.contains("base_url") && v["base_url"].is_string())
                txn.exec("UPDATE ir_ai_provider SET base_url=$1, write_date=now() WHERE name=$2",
                         pqxx::params{v["base_url"].get<std::string>(), name});
            if (v.contains("workspace_id") && v["workspace_id"].is_string())
                txn.exec("UPDATE ir_ai_provider SET workspace_id=$1, write_date=now() WHERE name=$2",
                         pqxx::params{v["workspace_id"].get<std::string>(), name});
            txn.commit();
        }
        if (AuditService::ready())
            AuditService::instance().log("ir.ai.settings", "save_provider",
                                         std::vector<int>{1}, extractContext_(call).uid);
        return handleProviders(call);
    }

    // ------------------------------------------------------------------
    // Does this written value ALREADY carry an SI multiplier?
    //
    // The bridge does not need the number — ProductModule::parseSiValue owns
    // the real parsing, and duplicating it here would be two parsers to keep in
    // step. It needs one bit: did the model put a prefix in the value. That is
    // the question the guard in handleAsk turns on.
    //
    // 'R' is deliberately absent: 2R2 is 2.2 Ω, a multiplier of 1, so it can
    // never be half of a double-prefix. Matching it here would flag every
    // resistor written the normal way.
    // ------------------------------------------------------------------
    static double siMultiplierOf_(const std::string& text) {
        static const std::map<char, double> kPrefix = {
            {'p',1e-12},{'n',1e-9},{'u',1e-6},{'m',1e-3},
            {'k',1e3},{'K',1e3},{'M',1e6},{'G',1e9},{'T',1e12}};
        std::string s;
        for (char c : text) if (!std::isspace(static_cast<unsigned char>(c))) s += c;
        size_t mu;                                  // "µ" is two bytes in UTF-8
        while ((mu = s.find("\xc2\xb5")) != std::string::npos) s.replace(mu, 2, "u");
        for (std::size_t i = 1; i < s.size(); ++i) {
            auto it = kPrefix.find(s[i]);
            if (it != kPrefix.end() &&
                (std::isdigit(static_cast<unsigned char>(s[i-1])) || s[i-1] == '.'))
                return it->second;                  // 4k7, 4.7k, 100n, 125m
        }
        return 1.0;
    }

    // ------------------------------------------------------------------
    // The double-multiplier guard.
    //
    // Seen on the very first real call: value "4k7" with unit "kΩ". Read
    // literally that is 4700 kΩ — 4.7 MΩ, a thousand times the actual part —
    // because submit() multiplies the parsed number by the unit's factor
    // (ProductModule: "4.7"+"kΩ" and "4k7"+"Ω" must land on the same base
    // value). The prompt now forbids writing the multiplier twice, but a
    // prompt is a request, not a constraint; the model that ignores it is the
    // one that matters.
    //
    // When both carry a multiplier the VALUE wins and the unit is demoted to
    // its base. "4k7" means 4.7k on every schematic and datasheet ever
    // printed, so the value string is the reliable statement of magnitude and
    // the unit is the afterthought the model appended.
    //
    // Reported, never silent. A correction nobody can see is its own kind of
    // wrong: the reviewer needs to know the number was touched, and it is also
    // how we find out the prompt is still losing.
    // ------------------------------------------------------------------
    nlohmann::json normaliseUnits_(nlohmann::json& parsed) {
        nlohmann::json adjusted = nlohmann::json::array();
        if (!parsed.contains("parameters") || !parsed["parameters"].is_array())
            return adjusted;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (auto& p : parsed["parameters"]) {
            if (!p.is_object() || !p.contains("unit") || !p["unit"].is_string()) continue;
            const std::string us = p["unit"].get<std::string>();
            if (us.empty()) continue;

            std::string raw;
            if (p.contains("value") && p["value"].is_string())
                raw = p["value"].get<std::string>();
            else if (p.contains("value") && p["value"].is_number())
                raw = std::to_string(p["value"].get<double>());
            if (raw.empty() || siMultiplierOf_(raw) == 1.0) continue;   // value carries none

            auto u = txn.exec("SELECT factor, quantity_kind FROM part_unit WHERE symbol=$1",
                              pqxx::params{us});
            if (u.empty() || u[0][1].is_null()) continue;
            if (u[0][0].as<double>(1.0) == 1.0) continue;               // unit carries none

            const std::string kind = u[0][1].c_str();
            auto b = txn.exec("SELECT symbol FROM part_unit "
                              "WHERE quantity_kind=$1 AND is_base ORDER BY id LIMIT 1",
                              pqxx::params{kind});
            if (b.empty()) continue;
            const std::string base = b[0][0].c_str();
            if (base == us) continue;

            p["unit"] = base;
            adjusted.push_back(jstrOr(p, "name", "parameter") +
                               ": \"" + raw + " " + us + "\" would have been applied twice; "
                               "read as \"" + raw + " " + base + "\"");
        }
        return adjusted;
    }

    // ==================================================================
    // Prompts — the instructions sent to a provider, as editable text.
    //
    // They were C++ string literals, which meant the one part of this most
    // likely to need tuning per deployment was the one part that needed a
    // rebuild to change. They now live in `prompts/*.md`, git-tracked, so a
    // deployment team edits them like any other file and the change shows up
    // in review.
    //
    // Three sources, in order, and the screen always says which one is live:
    //
    //   1. a DATABASE override, if somebody edited it in Settings → AI Agent
    //   2. the FILE in prompts/                              ← the normal case
    //   3. a COMPILED copy, only when the file is missing, so a bad deploy
    //      degrades to working-but-stale instead of breaking every AI feature
    //
    // UI edits are NOT written back to the file. A process that rewrites its
    // own git-tracked sources is a process fighting whoever deployed it: the
    // override is a hotfix you can see and revert, and moving it into the file
    // is a deliberate commit.
    //
    // What is NOT here is as important as what is. The double-multiplier
    // guard, the unit allowlist, the staging queue and the rule that the agent
    // never picks a part are all in C++ and still apply to whatever a
    // rewritten prompt produces. Editing a prompt cannot lower that floor.
    // ==================================================================
    struct PromptTask {
        const char* task;
        const char* label;
        const char* file;
        const char* about;
        std::vector<const char*> placeholders;  ///< every {{name}} the code supplies
        std::vector<const char*> required;      ///< without these the prompt asks nothing
    };

    static const std::vector<PromptTask>& promptTasks() {
        static const std::vector<PromptTask> kT = {
            {"part_lookup", "Part lookup", "part_lookup.md",
             "Identifies a component and returns candidates. Used by Products → Part Lookup.",
             {"query", "max_candidates", "units", "categories", "footprints"},
             {"query"}},
            {"help_assistant", "Help assistant", "help_assistant.md",
             "Answers a question from the manual. Used by the Help Centre's assistant rail.",
             {"question", "articles"},
             {"question", "articles"}},
            {"bom_headers", "BOM column mapping", "bom_headers.md",
             "Maps the columns of an unrecognised BOM export. Used by the BOM Editor.",
             {"header", "samples"},
             {"header"}},
            {"bom_clean", "BOM tidy-up", "bom_clean.md",
             "Normalises imported BOM rows to house conventions — values, packages, "
             "designators. Used by the BOM Editor after a parse.",
             {"rows", "units", "footprints"},
             {"rows"}},
        };
        return kT;
    }

    static const PromptTask* promptTask(const std::string& task) {
        for (const auto& t : promptTasks()) if (task == t.task) return &t;
        return nullptr;
    }

    /// Where the prompt files live. Relative to the working directory, the
    /// same convention as web/static and db/snapshots; ERP_PROMPT_DIR moves it
    /// for a deployment that installs them elsewhere.
    static std::string promptDir() {
        const char* e = std::getenv("ERP_PROMPT_DIR");
        return (e && *e) ? std::string(e) : std::string("prompts");
    }

    static std::string readPromptFile(const PromptTask& t, bool& found) {
        found = false;
        std::ifstream f(promptDir() + "/" + t.file, std::ios::binary);
        if (!f) return {};
        std::ostringstream ss; ss << f.rdbuf();
        found = true;
        return ss.str();
    }

    /// The last-resort copy. Deliberately terse: it exists so a missing
    /// prompts/ directory degrades instead of breaking, not as a second place
    /// to maintain the real text. The screen says loudly when it is in use.
    static std::string compiledPrompt(const std::string& task) {
        if (task == "part_lookup")
            return "Identify this electronic component. Answer with a single JSON object:\n"
                   "{\"notes\":string,\"candidates\":[{\"mpn\":string,\"manufacturer\":string,"
                   "\"name\":string,\"footprint\":string,\"source\":string,\"confidence\":number,"
                   "\"parameters\":[{\"name\":string,\"value\":string,\"unit\":string}]}]}\n"
                   "Write a magnitude ONCE - either in the value (4k7) or as a unit prefix "
                   "(kΩ), never both.\nUnits must come from: {{units}}\n"
                   "Return up to {{max_candidates}} candidates.\nPart: {{query}}";
        if (task == "help_assistant")
            return "Answer the question using ONLY these help articles. Say so plainly if "
                   "they do not cover it; do not invent a menu path.\n"
                   "Reply as JSON: {\"answer\":string,\"cited\":[slug,...]}\n"
                   "=== ARTICLES ===\n{{articles}}\n=== QUESTION ===\n{{question}}";
        if (task == "bom_clean")
            return "Tidy these BOM rows. Return the SAME rows in the same order, same count:\n"
                   "{\"rows\":[{\"designators\":string,\"quantity\":int,\"mpn\":string,"
                   "\"manufacturer\":string,\"value\":string,\"footprint\":string,"
                   "\"description\":string,\"fitted\":boolean}],\"notes\":string}\n"
                   "Write a magnitude ONCE (4.7K -> 4k7). Reduce a footprint to its package "
                   "name (C_0603_1608Metric -> 0603). Blank an MPN that is really a library "
                   "reference. Never choose a part.\n=== ROWS ===\n{{rows}}";
        if (task == "bom_headers")
            return "Map these BOM columns. Return zero-based indices, null where absent:\n"
                   "{\"mapping\":{\"designators\":int|null,\"quantity\":int|null,\"mpn\":int|null,"
                   "\"manufacturer\":int|null,\"value\":int|null,\"footprint\":int|null,"
                   "\"description\":int|null,\"fitted\":int|null},\"fitted_negated\":boolean,"
                   "\"tool\":string,\"notes\":string}\n"
                   "In Altium and JLCPCB exports the value column is called Comment.\n"
                   "=== HEADER ===\n{{header}}\n=== SAMPLE ROWS ===\n{{samples}}";
        return {};
    }

    /// The text that will actually be sent, and where it came from.
    struct LoadedPrompt { std::string body; std::string source; bool fileMissing = false; };

    LoadedPrompt loadPrompt(pqxx::work& txn, const PromptTask& t) {
        LoadedPrompt out;
        auto r = txn.exec("SELECT body FROM ir_ai_prompt WHERE task=$1",
                          pqxx::params{std::string(t.task)});
        if (!r.empty() && !std::string(r[0][0].c_str()).empty()) {
            out.body = r[0][0].c_str(); out.source = "override"; return out;
        }
        bool found = false;
        out.body = readPromptFile(t, found);
        if (found && !out.body.empty()) { out.source = "file"; return out; }
        out.fileMissing = true;
        out.body   = compiledPrompt(t.task);
        out.source = "compiled";
        return out;
    }

    /// Substitute {{name}}. An unknown placeholder is left exactly as written:
    /// a visible `{{querry}}` in the sent prompt is a typo somebody can find,
    /// where a silent blank is a prompt that quietly asks about nothing.
    static std::string renderPrompt(const std::string& tmpl,
                                    const std::map<std::string, std::string>& vars) {
        std::string out;
        out.reserve(tmpl.size() + 256);
        for (std::size_t i = 0; i < tmpl.size();) {
            if (tmpl.compare(i, 2, "{{") == 0) {
                const auto end = tmpl.find("}}", i + 2);
                if (end != std::string::npos) {
                    std::string key = tmpl.substr(i + 2, end - i - 2);
                    // tolerate {{ query }}
                    while (!key.empty() && std::isspace((unsigned char)key.front())) key.erase(key.begin());
                    while (!key.empty() && std::isspace((unsigned char)key.back()))  key.pop_back();
                    auto it = vars.find(key);
                    if (it != vars.end()) { out += it->second; i = end + 2; continue; }
                }
            }
            out += tmpl[i++];
        }
        return out;
    }

    /// Build the prompt for a task, ready to send.
    std::string buildPrompt(const std::string& task,
                            const std::map<std::string, std::string>& vars,
                            std::string* sourceOut = nullptr) {
        const PromptTask* t = promptTask(task);
        if (!t) throw cerp::infrastructure::ValidationError("Unknown prompt task '" + task + "'.");
        LoadedPrompt lp;
        {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            lp = loadPrompt(txn, *t);
        }
        if (lp.fileMissing)
            LOG_WARN << "[ir/ai] prompts/" << t->file << " is missing — using the compiled "
                        "fallback. Check ERP_PROMPT_DIR or the deployment.";
        if (sourceOut) *sourceOut = lp.source;
        return renderPrompt(lp.body, vars);
    }

    // ------------------------------------------------------------------
    // prompts — every task, its text, and where that text came from.
    // ------------------------------------------------------------------
    nlohmann::json handlePrompts(const core::CallKwArgs& call) {
        requireAdmin_(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        nlohmann::json out = nlohmann::json::array();
        for (const auto& t : promptTasks()) {
            bool haveFile = false;
            const std::string fileBody = readPromptFile(t, haveFile);
            auto ov = txn.exec("SELECT body, to_char(write_date,'YYYY-MM-DD HH24:MI') "
                               "FROM ir_ai_prompt WHERE task=$1",
                               pqxx::params{std::string(t.task)});
            const bool overridden = !ov.empty() && !std::string(ov[0][0].c_str()).empty();

            nlohmann::json ph = nlohmann::json::array();
            for (const char* p : t.placeholders)
                ph.push_back({{"name", p},
                              {"required", std::find_if(t.required.begin(), t.required.end(),
                                   [&](const char* r){ return std::string(r) == p; })
                                   != t.required.end()}});

            out.push_back({
                {"task", t.task}, {"label", t.label}, {"about", t.about},
                {"file", promptDir() + "/" + t.file},
                {"file_present", haveFile},
                // Which one is LIVE, answered plainly. An operator debugging a
                // strange answer needs to know whether they are looking at the
                // text being sent or a file that is being ignored.
                {"source", overridden ? "override" : (haveFile ? "file" : "compiled")},
                {"body", overridden ? std::string(ov[0][0].c_str())
                                    : (haveFile ? fileBody : compiledPrompt(t.task))},
                {"file_body", haveFile ? fileBody : compiledPrompt(t.task)},
                {"overridden", overridden},
                {"edited_at", overridden && !ov[0][1].is_null() ? ov[0][1].c_str() : ""},
                {"placeholders", ph}});
        }
        return out;
    }

    nlohmann::json handleSavePrompt(const core::CallKwArgs& call) {
        requireAdmin_(call);
        const auto v = call.arg(0);
        if (!v.is_object()) throw cerp::infrastructure::ValidationError(
            "save_prompt expects an object.");
        const std::string task = v.value("task", std::string{});
        const PromptTask* t = promptTask(task);
        if (!t) throw cerp::infrastructure::ValidationError("Unknown prompt task '" + task + "'.");
        const std::string body = v.value("body", std::string{});
        if (body.find_first_not_of(" \t\r\n") == std::string::npos)
            throw cerp::infrastructure::ValidationError(
                "The prompt is empty. Use Reset to go back to the shipped text.");

        // A prompt that lost its {{query}} asks the model about nothing at all,
        // and the failure looks like a bad model rather than a bad edit. Refuse
        // it at save time, where the person can still see what they removed.
        std::vector<std::string> missing;
        for (const char* r : t->required)
            if (body.find(std::string("{{") + r) == std::string::npos)
                missing.push_back(r);
        if (!missing.empty()) {
            std::string m;
            for (const auto& s : missing) { if (!m.empty()) m += ", "; m += "{{" + s + "}}"; }
            throw cerp::infrastructure::ValidationError(
                "This prompt still needs " + m + " — without it the model is never told "
                "what it is being asked about.");
        }

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec("INSERT INTO ir_ai_prompt (task, body, updated_by, write_date) "
                 "VALUES ($1,$2,$3,now()) "
                 "ON CONFLICT (task) DO UPDATE SET body=EXCLUDED.body, "
                 "  updated_by=EXCLUDED.updated_by, write_date=now()",
                 pqxx::params{task, body, core::CurrentUser::get().uid});
        txn.commit();
        LOG_INFO << "[ir/ai] prompt '" << task << "' overridden by uid "
                 << core::CurrentUser::get().uid;
        return {{"ok", true}, {"task", task}, {"source", "override"}};
    }

    /// Drop the override so the file wins again. The shipped text is never
    /// stored in the database, which is what makes this a real reset rather
    /// than a copy of whatever was default at install time.
    nlohmann::json handleResetPrompt(const core::CallKwArgs& call) {
        requireAdmin_(call);
        const auto v = call.arg(0);
        const std::string task = v.is_object() ? v.value("task", std::string{}) : std::string{};
        const PromptTask* t = promptTask(task);
        if (!t) throw cerp::infrastructure::ValidationError("Unknown prompt task '" + task + "'.");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec("DELETE FROM ir_ai_prompt WHERE task=$1", pqxx::params{task});
        txn.commit();
        bool haveFile = false;
        readPromptFile(*t, haveFile);
        return {{"ok", true}, {"task", task},
                {"source", haveFile ? "file" : "compiled"}};
    }

    // ------------------------------------------------------------------
    // status — "can I ask a question?", for any authenticated user.
    //
    // The rest of this model is admin-only, and rightly so: it holds a
    // credential. But the help assistant is offered to everybody, and a
    // sidebar cannot decide whether to enable its Ask button without knowing
    // whether an agent exists. So this returns exactly two booleans and
    // nothing else — no key, no tail, no provider URL, no model name. It
    // cannot leak configuration because it never reads any.
    // ------------------------------------------------------------------
    nlohmann::json handleStatus(const core::CallKwArgs& call) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto r = row_(txn);
        const std::string key = r["api_key"].c_str();
        bool ready = r["enabled"].as<bool>(false);
        if (ready) {
            // A provider with no key cannot answer, so "ready" would be a lie.
            auto p = txn.exec("SELECT api_key, auth_style FROM ir_ai_provider WHERE name=$1",
                              pqxx::params{std::string(r["provider"].c_str())});
            ready = !p.empty() && (std::string(p[0][1].c_str()) == "none" ||
                                   !std::string(p[0][0].c_str()).empty());
        }
        return {{"ready", ready}, {"admin", extractContext_(call).isAdmin}};
    }

    // ------------------------------------------------------------------
    // callProvider_ — the ONE place an outbound model call is assembled.
    //
    // Everything provider-shaped lives here: which gate stops the call, which
    // header carries the key, how the web-search capability is asked for, and
    // where the answer and its citations hide in the envelope. Callers pass a
    // prompt and get back text plus sources; they never learn what an
    // auth_style is.
    //
    // That matters because there are now two callers with very different jobs
    // (a part lookup that wants JSON, a help assistant that wants prose) and
    // exactly one of them must not be the place the cap is enforced.
    // ------------------------------------------------------------------
    struct ProviderReply {
        bool        ok = false;
        bool        mocked = false;
        bool        searched = false;      ///< did we actually ask it to browse
        std::string provider, model, text, detail;
        nlohmann::json sources  = nlohmann::json::array();  ///< [{url,title}]
        nlohmann::json searches = nlohmann::json::array();  ///< the queries it ran
    };

    ProviderReply callProvider_(const std::string& prompt,
                                bool wantSearch,
                                const std::string& mockText,
                                int maxTokOverride = 0)
    {
        ProviderReply out;
        std::string key, base, path, style, wsid, searchStyle, searchTool, searchPath;
        int cap = 0, used = 0, maxTok = 1024;
        bool searchEnabled = true;
        {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto s0 = row_(txn);
            out.provider  = s0["provider"].c_str();
            cap           = s0["daily_call_cap"].as<int>(0);
            used          = s0["calls_today"].as<int>(0);
            maxTok        = s0["max_output_tokens"].as<int>(1024);
            searchEnabled = s0["web_search"].as<bool>(true);
            if (!s0["enabled"].as<bool>(false))
                throw cerp::infrastructure::ValidationError("The AI agent is disabled.");
            auto p = txn.exec("SELECT * FROM ir_ai_provider WHERE name=$1",
                              pqxx::params{out.provider});
            if (p.empty())
                throw cerp::infrastructure::ValidationError(
                    "Provider '" + out.provider + "' is not configured.");
            key         = p[0]["api_key"].c_str();
            base        = p[0]["base_url"].c_str();
            path        = p[0]["path"].c_str();
            out.model   = p[0]["model"].c_str();
            style       = p[0]["auth_style"].c_str();
            wsid        = p[0]["workspace_id"].c_str();
            searchStyle = p[0]["search_style"].c_str();
            searchTool  = p[0]["search_tool"].c_str();
            searchPath  = p[0]["search_path"].c_str();
            // The cap is checked BEFORE the call, not after: a cap that only
            // notices once the money is spent is not a cap.
            if (cap > 0 && used >= cap)
                throw cerp::infrastructure::ValidationError(
                    "The daily call cap (" + std::to_string(cap) + ") has been reached.");
        }
        if (maxTokOverride > 0) maxTok = maxTokOverride;

        if (style == "none") {                       // mock: never touches the network
            out.ok = out.mocked = true;
            out.text = mockText;
            return out;
        }
        if (key.empty())
            throw cerp::infrastructure::ValidationError(
                "No API key is configured for " + out.provider + ".");

        const bool doSearch = wantSearch && searchEnabled && !searchStyle.empty();
        out.searched = doSearch;

        // A searching call may not speak the same wire as a plain one. xAI's
        // web search lives on the Responses endpoint, which takes `input`
        // instead of `messages` and answers with `output[]` — so the shape is
        // chosen by whether we are searching, not only by who we are calling.
        const bool responsesWire = doSearch && searchStyle == "responses_tool"
                                            && !searchPath.empty();
        nlohmann::json payload;
        std::vector<std::pair<std::string, std::string>> headers;

        if (responsesWire) {
            payload = {{"model", out.model}, {"input", prompt},
                       {"max_output_tokens", maxTok},
                       {"tools", nlohmann::json::array({{{"type", searchTool}}})}};
            headers = {{"Authorization", "Bearer " + key}};
            path = searchPath;
        } else {
            payload = {{"model", out.model}, {"max_tokens", maxTok},
                       {"messages", nlohmann::json::array({
                           {{"role","user"},{"content", prompt}}})}};
            if (style == "anthropic") {
                headers = {{"x-api-key", key}, {"anthropic-version", "2023-06-01"}};
                if (!wsid.empty()) headers.emplace_back("anthropic-workspace-id", wsid);
                if (doSearch && searchStyle == "anthropic_tool")
                    payload["tools"] = nlohmann::json::array({
                        {{"type", searchTool}, {"name", "web_search"}, {"max_uses", 6}}});
            } else {
                headers = {{"Authorization", "Bearer " + key}};
            }
        }

        // Browsing takes far longer than answering from memory — several
        // fetches inside one request. A 60s ceiling that was generous for a
        // memory answer is a timeout for a search.
        const double timeout = doSearch ? 180.0 : 60.0;
        auto res = cerp::infrastructure::HttpClient::postJson(base, path, payload.dump(),
                                                              headers, timeout);
        {
            auto c2 = db_->acquire();
            pqxx::work t2{c2.get()};
            t2.exec("UPDATE ir_ai_settings SET calls_today = "
                    "CASE WHEN calls_date = CURRENT_DATE THEN calls_today + 1 ELSE 1 END, "
                    "calls_date = CURRENT_DATE WHERE id=1");
            t2.commit();
        }
        if (!res.ok) {
            out.detail = res.error.empty()
                ? ("the service replied " + std::to_string(res.status)) : res.error;
            try {
                auto j = nlohmann::json::parse(res.body, nullptr, false);
                // `error` is an OBJECT for Anthropic and a bare STRING for xAI.
                // Only handling the object shape swallowed the one message that
                // actually explained a failure — a 410 whose body said "Live
                // search is deprecated, switch to the Agent Tools API" surfaced
                // here as an unexplained "the service replied 410".
                if (!j.is_discarded() && j.contains("error")) {
                    const auto& e = j["error"];
                    if (e.is_object() && e.contains("message") && e["message"].is_string())
                        out.detail = e["message"].get<std::string>();
                    else if (e.is_string())
                        out.detail = e.get<std::string>();
                }
            } catch (...) {}
            return out;
        }

        // Pull the assistant's text — and everything it cited — out of
        // whichever envelope this provider uses.
        std::set<std::string> seen;
        auto addSource = [&](const std::string& url, const std::string& title) {
            if (url.empty() || !seen.insert(url).second) return;
            out.sources.push_back({{"url", url}, {"title", title.empty() ? url : title}});
        };
        try {
            auto j = nlohmann::json::parse(res.body, nullptr, false);
            if (!j.is_discarded()) {
                if (j.contains("output") && j["output"].is_array()) {
                    // Responses wire. The answer is one block among several:
                    // the searches it ran, its reasoning, then the message.
                    //
                    // Two different lists of URLs come back and they are not
                    // the same thing: what the search RETURNED, and what the
                    // answer actually CITED. The cited ones are listed first,
                    // because those are the pages a reader should check.
                    std::vector<std::string> read, cited;
                    for (const auto& blk : j["output"]) {
                        const std::string bt = blk.value("type", "");
                        if (bt == "web_search_call" && blk.contains("action")) {
                            const auto& act = blk["action"];
                            if (act.contains("query") && act["query"].is_string())
                                out.searches.push_back(act["query"]);
                            if (act.contains("sources") && act["sources"].is_array())
                                for (const auto& s : act["sources"])
                                    read.push_back(s.value("url", ""));
                        }
                        if (bt != "message" || !blk.contains("content")) continue;
                        for (const auto& c : blk["content"]) {
                            if (c.contains("text") && c["text"].is_string())
                                out.text += c["text"].get<std::string>();
                            if (c.contains("annotations") && c["annotations"].is_array())
                                for (const auto& a : c["annotations"])
                                    if (a.value("type", "") == "url_citation")
                                        cited.push_back(a.value("url", ""));
                        }
                    }
                    for (const auto& u : cited) addSource(u, "");
                    for (const auto& u : read)  addSource(u, "");
                } else if (j.contains("content") && j["content"].is_array()) {
                    // Anthropic: with a server tool the reply is several blocks
                    // — the search results, then the prose that used them. Only
                    // concatenating the text blocks gives the answer; the URLs
                    // live in the tool-result block and in per-block citations.
                    for (const auto& blk : j["content"]) {
                        const std::string bt = blk.value("type", "");
                        if (bt == "text" && blk.contains("text"))
                            out.text += blk["text"].get<std::string>();
                        if (blk.contains("citations") && blk["citations"].is_array())
                            for (const auto& c : blk["citations"])
                                addSource(c.value("url", ""), c.value("title", ""));
                        if (bt == "web_search_tool_result" && blk.contains("content")
                            && blk["content"].is_array())
                            for (const auto& c : blk["content"])
                                addSource(c.value("url", ""), c.value("title", ""));
                    }
                } else if (j.contains("choices") && j["choices"].is_array()
                           && !j["choices"].empty()) {
                    const auto& m = j["choices"][0]["message"];
                    if (m.contains("content") && m["content"].is_string())
                        out.text = m["content"].get<std::string>();
                    // xAI returns citations alongside the choices, as bare URLs.
                    if (j.contains("citations") && j["citations"].is_array())
                        for (const auto& c : j["citations"])
                            if (c.is_string()) addSource(c.get<std::string>(), "");
                            else               addSource(c.value("url", ""), c.value("title", ""));
                }
            }
        } catch (...) {}

        if (out.text.empty()) {
            out.detail = "The reply could not be read (unexpected response shape).";
            return out;
        }
        out.ok = true;
        return out;
    }

    // ------------------------------------------------------------------
    // Reading a field out of a MODEL's reply.
    //
    // nlohmann's `.value(key, default)` throws when the key exists but holds
    // null — and `{"notes": null}` or `{"fitted_negated": null}` is a
    // perfectly reasonable thing for a model to answer when a file has no such
    // column. The default is only used for a MISSING key, never a null one.
    //
    // That threw a type_error out of the request and surfaced as "An internal
    // error occurred" on a reply that was actually fine. Every field read from
    // a model reply goes through these instead.
    // ------------------------------------------------------------------
    static std::string jstrOr(const nlohmann::json& j, const char* k,
                              const std::string& dflt = {}) {
        auto it = j.find(k);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : dflt;
    }
    static bool jboolOr(const nlohmann::json& j, const char* k, bool dflt) {
        auto it = j.find(k);
        return (it != j.end() && it->is_boolean()) ? it->get<bool>() : dflt;
    }

    /// Take the outermost {...} out of a reply. Models wrap JSON in prose and
    /// code fences however firmly they are told not to.
    static nlohmann::json extractJson_(const std::string& text) {
        const auto a = text.find('{');
        const auto b = text.rfind('}');
        if (a == std::string::npos || b == std::string::npos || b <= a)
            return nlohmann::json(nullptr);
        auto j = nlohmann::json::parse(text.substr(a, b - a + 1), nullptr, false);
        return j.is_discarded() ? nlohmann::json(nullptr) : j;
    }

    // ------------------------------------------------------------------
    // ask — put a part to the active provider and get CANDIDATES back.
    //
    // This is the BRIDGE (docs/110 §6 step 4). It deliberately does not write
    // anything: it returns parsed proposals, and the caller hands the one it
    // wants to part.lookup submit, which stages it for a human. The agent
    // proposes; a person disposes. Giving this method write access would
    // collapse the one property the whole design exists to protect.
    //
    // Prompt injection is the reason that matters. With web search on, the
    // model is reading vendor pages and datasheets — untrusted text that can
    // contain instructions aimed at it. Because the answer lands in a staging
    // table a person reviews, a hijacked reply cannot silently become a part
    // somebody solders.
    // ------------------------------------------------------------------
    nlohmann::json handleAsk(const core::CallKwArgs& call) {
        requireAdmin_(call);
        const auto v = call.arg(0);
        std::string query = v.is_object() ? v.value("query", std::string{}) : std::string{};
        if (query.empty())
            throw cerp::infrastructure::ValidationError("ask: a query is required.");

        // The vocabulary the ERP will accept, stated up front. Without it the
        // model invents unit spellings and category names that describe()
        // would have given it for free.
        //
        // Grouped BY QUANTITY, base unit first, rather than as one flat list.
        // The grouping is the teaching: seeing "resistance: Ω (base) mΩ kΩ MΩ"
        // makes it obvious that kΩ is Ω-with-a-prefix, which is exactly the
        // relationship the rule below turns on. A flat list hides it.
        std::string vocab, catHint, fpHint;
        int wanted = 3;
        {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            wanted = row_(txn)["max_candidates"].as<int>(3);
            auto us = txn.exec(
                "SELECT quantity_kind, "
                "       string_agg(symbol || CASE WHEN is_base THEN ' (base)' ELSE '' END, "
                "                  ' ' ORDER BY factor) "
                "FROM part_unit WHERE quantity_kind IS NOT NULL "
                "GROUP BY quantity_kind ORDER BY quantity_kind");
            for (const auto& r : us)
                vocab += std::string("  ") + r[0].c_str() + ": " + r[1].c_str() + "\n";
            // Real category names, so category_path lands on something that
            // exists instead of a plausible invention submit() has to reject.
            auto cs = txn.exec("SELECT string_agg(name, ', ' ORDER BY name) "
                               "FROM (SELECT DISTINCT name FROM product_category LIMIT 120) t");
            if (!cs.empty() && !cs[0][0].is_null()) catHint = cs[0][0].c_str();
            // The package has its OWN field. Without this the model had nowhere
            // to put "0603" except a parameter, where it was read as the number
            // 603 — the leading zero, and with it the meaning, gone.
            auto fs = txn.exec("SELECT string_agg(name, ', ' ORDER BY name) "
                               "FROM (SELECT name FROM part_footprint ORDER BY name LIMIT 80) t");
            if (!fs.empty() && !fs[0][0].is_null()) fpHint = fs[0][0].c_str();
        }
        if (wanted < 1) wanted = 1;
        if (wanted > 8) wanted = 8;

        // The text lives in prompts/part_lookup.md — see the Prompts section
        // of Settings → AI Agent. The UNITS part of it is long out of all
        // proportion to the rest, deliberately: it is the field that was
        // actually got wrong on the first real call, and the failure was
        // silent — a plausible number with a thousandfold error in it.
        const std::string prompt = buildPrompt("part_lookup", {
            {"query",          query},
            {"max_candidates", std::to_string(wanted)},
            {"units",          vocab.empty()    ? "(none configured)" : vocab},
            {"categories",     catHint.empty()  ? "(none configured)" : catHint},
            {"footprints",     fpHint.empty()   ? "(none configured)" : fpHint},
        });


        // The mock answers with the DOUBLE-PREFIXED shape a real provider
        // produced on the first live call ("4k7" + "kΩ") and then goes through
        // the same post-processing a live answer does. A mock returning clean
        // data would exercise none of it, and since the suite has no network,
        // "untested offline" would mean untested.
        nlohmann::json mock = {
            {"notes", "Mock provider — no network was used, so nothing here was looked up."},
            {"candidates", nlohmann::json::array({
                {{"query", query}, {"mpn", "MOCK-0001"},
                 {"manufacturer", "Mock Manufacturer"},
                 {"name", std::string("Mock part for: ") + query},
                 {"confidence", 0.5}, {"why", "the only mock candidate"},
                 {"source", "https://example.invalid/mock"},
                 {"parameters", nlohmann::json::array({
                     {{"name","resistance"},{"value","4k7"},{"unit","kΩ"}},
                     {{"name","power"},     {"value","125m"},{"unit","W"}},
                     {{"name","tolerance"}, {"value","1"},   {"unit","%"}}})}}})}};

        auto rep = callProvider_(prompt, /*wantSearch=*/true, mock.dump());
        if (!rep.ok)
            return {{"ok", false}, {"provider", rep.provider}, {"detail", rep.detail}};

        auto parsed = extractJson_(rep.text);
        if (parsed.is_null())
            return {{"ok", false}, {"provider", rep.provider},
                    {"detail", "The reply was not valid JSON."},
                    {"raw", rep.text.substr(0, 400)}};

        // A model told to return {candidates:[…]} sometimes returns one bare
        // LookupResult instead. Accept it rather than failing the lookup.
        nlohmann::json cands = nlohmann::json::array();
        if (parsed.contains("candidates") && parsed["candidates"].is_array())
            cands = parsed["candidates"];
        else if (parsed.is_object())
            cands.push_back(parsed);

        nlohmann::json outCands = nlohmann::json::array();
        for (auto& c : cands) {
            if (!c.is_object()) continue;
            if (!c.contains("query") || !c["query"].is_string() ||
                c["query"].get<std::string>().empty())
                c["query"] = query;
            auto adj = normaliseUnits_(c);
            if (!adj.empty())
                LOG_WARN << "[ir/ai] " << rep.provider << " returned " << adj.size()
                         << " double-prefixed value(s); the unit was demoted to base";
            c["adjusted"] = adj;
            outCands.push_back(c);
            if (static_cast<int>(outCands.size()) >= wanted) break;
        }
        if (outCands.empty())
            return {{"ok", false}, {"provider", rep.provider},
                    {"detail", "The reply contained no candidates."},
                    {"raw", rep.text.substr(0, 400)}};

        return {{"ok", true}, {"provider", rep.provider}, {"model", rep.model},
                {"mocked", rep.mocked}, {"searched", rep.searched},
                {"notes", jstrOr(parsed, "notes")},
                {"sources", rep.sources}, {"searches", rep.searches},
                {"candidates", outCands},
                // Kept so an older caller (and the paste path) still works.
                {"result", outCands[0]},
                {"adjusted", outCands[0].value("adjusted", nlohmann::json::array())}};
    }

    // ------------------------------------------------------------------
    // ask_help — the Help Centre's assistant.
    //
    // Deliberately NOT admin-only. Configuring the agent is an administrator's
    // job; asking the manual a question is everybody's. The daily cap is what
    // bounds the spend, and it is enforced in callProvider_ for every caller.
    //
    // Retrieval-augmented: the articles the sidebar already surfaces are sent
    // as the context, and the model is told to answer FROM them and to cite
    // the slugs it used. That is what makes an answer checkable — a cited
    // slug is a button the reader can press to go and verify it.
    // ------------------------------------------------------------------
    nlohmann::json handleAskHelp(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object())
            throw cerp::infrastructure::ValidationError("ask_help expects an object.");
        const std::string question = v.value("question", std::string{});
        if (question.empty())
            throw cerp::infrastructure::ValidationError("A question is required.");
        const std::string book = v.value("book", std::string{});

        // Retrieval. A natural-language question must be scored TERM BY TERM:
        // matching the whole sentence as one substring finds nothing, because
        // no article contains "how do I write 4k7 units" verbatim. That was the
        // first version of this and it silently returned an empty context for
        // every real question — the assistant then answered "not in the manual"
        // about things the manual covers well.
        std::vector<std::string> terms;
        {
            static const std::set<std::string> kStop = {
                "the","and","for","how","what","where","when","why","does","did",
                "can","you","are","was","with","from","this","that","have","has",
                "its","into","about","which","should","would","could","there",
                "their","them","then","than","not","but","use","using","get","got",
                "any","all","out","see","set","way","need","want","please","tell"};
            std::set<std::string> uniq;
            std::string cur;
            auto flush = [&] {
                if (cur.size() >= 3 && !kStop.count(cur) && uniq.insert(cur).second)
                    terms.push_back(cur);
                cur.clear();
            };
            for (char ch : question) {
                const unsigned char u = static_cast<unsigned char>(ch);
                if (std::isalnum(u) || ch == '_' || ch == '-')
                    cur += static_cast<char>(std::tolower(u));
                else flush();
            }
            flush();
            if (terms.size() > 8) terms.resize(8);
        }

        std::string ctx;
        nlohmann::json used = nlohmann::json::array();
        {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            // Score = how many of the question's terms an article carries, with
            // a title or keyword hit worth more than a body hit. Terms are
            // BOUND, never interpolated; only the placeholder count is built.
            std::string score;
            pqxx::params p;
            int n = 0;
            for (const auto& t : terms) {
                const std::string i = std::to_string(++n);
                if (!score.empty()) score += " + ";
                score += "(CASE WHEN title ILIKE $" + i + " THEN 4 ELSE 0 END"
                         " + CASE WHEN COALESCE(keywords,'') ILIKE $" + i + " THEN 3 ELSE 0 END"
                         " + CASE WHEN COALESCE(body,'')     ILIKE $" + i + " THEN 1 ELSE 0 END)";
                p.append("%" + t + "%");
            }
            if (score.empty()) score = "0";
            // The book being read is a strong hint about what is being asked.
            const std::string bi = std::to_string(++n);
            score += " + CASE WHEN $" + bi + " <> '' AND book = $" + bi + " THEN 2 ELSE 0 END";
            p.append(book);

            auto rs = txn.exec(
                "SELECT slug, title, book, body, score FROM ("
                "  SELECT slug, title, book, body, (" + score + ") AS score"
                "    FROM help_article WHERE active AND NOT is_section) t "
                "WHERE score > 0 ORDER BY score DESC, slug LIMIT 6", p);
            for (const auto& r : rs) {
                std::string body = r["body"].c_str();
                if (body.size() > 4000) body = body.substr(0, 4000);   // keep the prompt bounded
                ctx += "--- article: " + std::string(r["slug"].c_str()) +
                       " (" + r["title"].c_str() + ") ---\n" + body + "\n\n";
                used.push_back({{"slug", r["slug"].c_str()}, {"title", r["title"].c_str()}});
            }
        }
        if (ctx.empty())
            return {{"ok", true}, {"answer",
                    "I could not find anything in the manual about that. Try different "
                    "wording, or browse the tabs above."},
                    {"cited", nlohmann::json::array()}, {"grounded", false}};

        // prompts/help_assistant.md
        const std::string prompt = buildPrompt("help_assistant", {
            {"articles", ctx}, {"question", question}});

        nlohmann::json mock = {
            {"answer", "Mock provider: no network was used, so this is not a real answer. "
                       "The retrieval step did run — the cited articles below are the ones "
                       "a real answer would have been drawn from."},
            {"cited", nlohmann::json::array()}};
        for (const auto& u : used) mock["cited"].push_back(u["slug"]);

        // No web search: the manual is the source of truth here, and letting it
        // browse would invite an answer about somebody else's ERP.
        auto rep = callProvider_(prompt, /*wantSearch=*/false, mock.dump(), 1200);
        if (!rep.ok)
            return {{"ok", false}, {"provider", rep.provider}, {"detail", rep.detail}};

        auto parsed = extractJson_(rep.text);
        std::string answer = parsed.is_null() ? rep.text
                                              : jstrOr(parsed, "answer", rep.text);

        // Only hand back citations that really exist — a model can cite a slug
        // it invented, and a dead button is worse than no button.
        nlohmann::json cited = nlohmann::json::array();
        if (!parsed.is_null() && parsed.contains("cited") && parsed["cited"].is_array()) {
            for (const auto& s : parsed["cited"]) {
                if (!s.is_string()) continue;
                const std::string slug = s.get<std::string>();
                for (const auto& u : used)
                    if (u["slug"] == slug) { cited.push_back(u); break; }
            }
        }
        if (cited.empty()) cited = used;

        return {{"ok", true}, {"provider", rep.provider}, {"model", rep.model},
                {"mocked", rep.mocked}, {"answer", answer},
                {"cited", cited}, {"grounded", true}};
    }

    // ------------------------------------------------------------------
    // map_bom_headers — read a BOM's header row and say which column is what.
    //
    // The narrowest possible AI seam for the importer, and narrow on purpose.
    // A model maps COLUMNS; it never resolves a part. Mapping headers is a
    // judgement call that differs per vendor and per tool version, which is
    // what a model is good at. Choosing which capacitor a row means is a
    // lookup that has to be reproducible and reviewable, which it is not.
    //
    // Only the header row and a couple of sample rows are sent. A 2,000-line
    // BOM would cost a fortune in tokens and tell the model nothing the first
    // two rows do not — the mapping is decidable from the shape alone. It also
    // means the company's part list is not shipped to a vendor wholesale.
    // ------------------------------------------------------------------
    nlohmann::json handleMapBomHeaders(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw cerp::infrastructure::ValidationError(
            "map_bom_headers expects an object.");
        const std::string header = v.value("header", std::string{});
        if (header.empty())
            throw cerp::infrastructure::ValidationError("A header row is required.");
        std::string samples;
        if (v.contains("samples") && v["samples"].is_array()) {
            int n = 0;
            for (const auto& s : v["samples"]) {
                if (!s.is_string() || ++n > 3) break;
                samples += s.get<std::string>() + "\n";
            }
        }

        // prompts/bom_headers.md
        const std::string prompt = buildPrompt("bom_headers", {
            {"header", header}, {"samples", samples}});

        nlohmann::json mock = {
            {"mapping", {{"designators", 0}, {"quantity", 1}, {"value", 2},
                         {"footprint", 3}, {"mpn", 4}}},
            {"fitted_negated", false}, {"tool", "mock"},
            {"notes", "Mock provider — no network was used, so nothing was actually read."}};

        // No web search: the answer is in the header row, not on the internet.
        auto rep = callProvider_(prompt, /*wantSearch=*/false, mock.dump(), 800);
        if (!rep.ok)
            return {{"ok", false}, {"provider", rep.provider}, {"detail", rep.detail}};

        auto parsed = extractJson_(rep.text);
        if (parsed.is_null() || !parsed.contains("mapping") || !parsed["mapping"].is_object())
            return {{"ok", false}, {"provider", rep.provider},
                    {"detail", "The reply did not contain a column mapping."},
                    {"raw", rep.text.substr(0, 400)}};

        // Keep only sane indices. A model that answers "designators": 14 for a
        // six-column file would otherwise produce a mapping that reads empty
        // cells forever and looks like an empty BOM.
        const int cols = static_cast<int>(std::count(header.begin(), header.end(), ',')
                                        + std::count(header.begin(), header.end(), ';')
                                        + std::count(header.begin(), header.end(), '\t')) + 1;
        nlohmann::json clean = nlohmann::json::object();
        nlohmann::json dropped = nlohmann::json::array();
        for (const char* f : {"designators","quantity","mpn","manufacturer",
                              "value","footprint","description","fitted"}) {
            const auto& m = parsed["mapping"];
            if (!m.contains(f) || !m[f].is_number_integer()) continue;
            const int idx = m[f].get<int>();
            if (idx >= 0 && idx < cols) clean[f] = idx;
            else dropped.push_back(std::string(f) + " -> column " + std::to_string(idx));
        }
        if (clean.empty())
            return {{"ok", false}, {"provider", rep.provider},
                    {"detail", "The mapping named no column this file has."}};

        return {{"ok", true}, {"provider", rep.provider}, {"model", rep.model},
                {"mocked", rep.mocked}, {"columns", cols},
                {"mapping", clean}, {"dropped", dropped},
                {"fitted_negated", jboolOr(parsed, "fitted_negated", false)},
                {"tool", jstrOr(parsed, "tool")},
                {"notes", parsed.value("notes", std::string{})}};
    }

    // ------------------------------------------------------------------
    // clean_bom_rows — normalise imported rows to the ERP's conventions.
    //
    // The second AI seam in the importer, and the same shape as the first: the
    // model rewrites TEXT, it never chooses a part. `4.7K` becomes `4k7`,
    // `Capacitor_SMD:C_0603_1608Metric` becomes `0603`, a library reference
    // sitting in an MPN column gets blanked. What each row then resolves to is
    // still the server's lookup, so it stays reproducible.
    //
    // The rows come back and are handed to `bom.import parse` as `rows:[...]`,
    // which was already the documented agent path — nothing new is trusted.
    //
    // The identity guard below is the load-bearing part. A model asked to
    // tidy 60 rows will sometimes return 58, merge two that look similar, or
    // reorder them, and a BOM quietly missing a line is far worse than an
    // untidy one: the board is short a part and nothing says so.
    // ------------------------------------------------------------------
    nlohmann::json handleCleanBomRows(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object() || !v.contains("rows") || !v["rows"].is_array())
            throw cerp::infrastructure::ValidationError("clean_bom_rows expects {rows:[...]}.");
        const auto& in = v["rows"];
        if (in.empty()) throw cerp::infrastructure::ValidationError("There are no rows to tidy.");
        // A cap on cost and on how much of the part list leaves the building.
        if (in.size() > 300)
            throw cerp::infrastructure::ValidationError(
                "That is " + std::to_string(in.size()) + " rows. Tidy up to 300 at a time.");

        std::string vocab, fpHint;
        {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto us = txn.exec("SELECT string_agg(symbol, ' ' ORDER BY quantity_kind, factor) "
                               "FROM part_unit WHERE quantity_kind IS NOT NULL");
            if (!us.empty() && !us[0][0].is_null()) vocab = us[0][0].c_str();
            auto fs = txn.exec("SELECT string_agg(name, ', ' ORDER BY name) "
                               "FROM (SELECT name FROM part_footprint ORDER BY name LIMIT 120) t");
            if (!fs.empty() && !fs[0][0].is_null()) fpHint = fs[0][0].c_str();
        }

        // Only the fields the importer actually reads. Sending the staged
        // severity or candidate list back would invite the model to argue with
        // the resolution, which is not its job.
        nlohmann::json slim = nlohmann::json::array();
        for (const auto& r : in) {
            if (!r.is_object()) continue;
            slim.push_back({
                {"designators",  jstrOr(r, "designators")},
                {"quantity",     r.value("quantity", 0)},
                {"mpn",          jstrOr(r, "mpn")},
                {"manufacturer", jstrOr(r, "manufacturer")},
                {"value",        jstrOr(r, "value")},
                {"footprint",    jstrOr(r, "footprint")},
                {"description",  jstrOr(r, "description")},
                {"fitted",       jboolOr(r, "fitted", true)}});
        }

        const std::string prompt = buildPrompt("bom_clean", {
            {"rows",       slim.dump(2)},
            {"units",      vocab.empty()  ? "(none configured)" : vocab},
            {"footprints", fpHint.empty() ? "(none configured)" : fpHint}});

        // The mock returns the rows untouched: it exercises the identity guard
        // and the diff without pretending to have cleaned anything.
        nlohmann::json mock = {{"rows", slim},
                               {"notes", "Mock provider — the rows were returned unchanged."}};

        auto rep = callProvider_(prompt, /*wantSearch=*/false, mock.dump(),
                                 static_cast<int>(slim.size()) * 120 + 512);
        if (!rep.ok)
            return {{"ok", false}, {"provider", rep.provider}, {"detail", rep.detail}};

        auto parsed = extractJson_(rep.text);
        if (parsed.is_null() || !parsed.contains("rows") || !parsed["rows"].is_array())
            return {{"ok", false}, {"provider", rep.provider},
                    {"detail", "The reply did not contain a rows array."},
                    {"raw", rep.text.substr(0, 400)}};

        // The guard. Same count, or the answer is refused outright — there is
        // no safe way to reconcile a tidy-up that lost a line.
        if (parsed["rows"].size() != slim.size())
            return {{"ok", false}, {"provider", rep.provider},
                    {"detail", "The assistant returned " +
                               std::to_string(parsed["rows"].size()) + " rows for " +
                               std::to_string(slim.size()) + ". Nothing was changed — a BOM "
                               "quietly missing a line is worse than an untidy one."}};

        nlohmann::json out = nlohmann::json::array();
        nlohmann::json changed = nlohmann::json::array();
        for (std::size_t i = 0; i < slim.size(); ++i) {
            const auto& before = slim[i];
            const auto& after  = parsed["rows"][i];
            nlohmann::json row = before;                    // start from ours
            if (after.is_object()) {
                for (const char* f : {"designators","mpn","manufacturer",
                                      "value","footprint","description"})
                    if (after.contains(f) && after[f].is_string()) row[f] = after[f];
                if (after.contains("quantity") && after["quantity"].is_number_integer())
                    row["quantity"] = after["quantity"];
                if (after.contains("fitted") && after["fitted"].is_boolean())
                    row["fitted"] = after["fitted"];
            }
            // Report every field it touched, so the diff is reviewable rather
            // than a wall of rows somebody scrolls past.
            for (const char* f : {"designators","quantity","mpn","manufacturer",
                                  "value","footprint","description","fitted"}) {
                if (before[f] == row[f]) continue;
                changed.push_back({{"row", static_cast<int>(i)},
                                   {"designators", jstrOr(before, "designators")},
                                   {"field", f},
                                   {"from", before[f].is_string()
                                                ? before[f].get<std::string>() : before[f].dump()},
                                   {"to",   row[f].is_string()
                                                ? row[f].get<std::string>() : row[f].dump()}});
            }
            out.push_back(row);
        }

        return {{"ok", true}, {"provider", rep.provider}, {"model", rep.model},
                {"mocked", rep.mocked}, {"rows", out}, {"changed", changed},
                {"notes", jstrOr(parsed, "notes")}};
    }

    nlohmann::json handleFieldsGet(const core::CallKwArgs&) {
        return {
            {"enabled",           {{"type","boolean"},{"string","Enabled"}}},
            {"provider",          {{"type","char"},   {"string","Provider"}}},
            {"model",             {{"type","char"},   {"string","Model"}}},
            {"max_output_tokens", {{"type","integer"},{"string","Max output tokens"}}},
            {"daily_call_cap",    {{"type","integer"},{"string","Daily call cap"}}},
        };
    }
};

void IrModule::registerViewModels() {
    auto db  = services_.db();
    auto& mf = models_;

    viewModels_.registerCreator("ir.ai.settings", [db]{
        return std::make_shared<IrAiSettingsViewModel>(db);
    });
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
    // the standard the reference ERP mechanism a lot of later features assume exists.
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
    // filestore, addressed by content hash. Same split the reference ERP uses, and the
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
    // res_field is carried for parity with the reference ERP (an attachment that backs
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

    // docs/106 — classify what a file IS, so a fabrication package can be shown
    // as groups rather than as sixteen files called top.gtl and bot.gbl.
    //
    // A NEW migration, not an edit to 1040. Migrations are applied once and
    // recorded; changing the body of one that has already run affects fresh
    // databases only and silently does nothing to every existing install. That
    // is exactly what happened on the first attempt here.
    runner.registerMigration({1041, "ir_attachment_document_type", R"SQL(
        ALTER TABLE ir_attachment ADD COLUMN IF NOT EXISTS document_type TEXT;
        CREATE INDEX IF NOT EXISTS ir_attachment_doctype_idx
            ON ir_attachment (res_model, res_id, document_type);
        -- Files uploaded before this column existed are classified from their
        -- names, so history groups the same way new uploads do.
        UPDATE ir_attachment SET document_type =
            CASE
              WHEN lower(name) ~ '\.(gbr|ger|gtl|gbl|gto|gbo|gts|gbs|gm1|gko|gbp|gtp|gpt|gpb)$' THEN 'gerber'
              WHEN lower(name) ~ '\.(drl|xln|drd|tap)$'        THEN 'drill'
              WHEN lower(name) ~ '\.(pos|xy)$'                 THEN 'placement'
              WHEN lower(name) ~ '\.(kicad_pcb|brd)$'          THEN 'pcb-design'
              WHEN lower(name) ~ '\.(kicad_sch|sch)$'          THEN 'schematic'
              WHEN lower(name) ~ '\.net$'                      THEN 'netlist'
              WHEN lower(name) ~ '\.(step|stp|iges|igs|stl|3mf)$' THEN '3d-model'
              WHEN lower(name) ~ '\.dxf$'                      THEN 'drawing'
              WHEN lower(name) ~ '\.(png|jpg|jpeg|gif|svg)$'   THEN 'image'
              WHEN lower(name) ~ '\.(csv|xlsx)$'               THEN 'data'
              WHEN lower(name) ~ '\.zip$'                      THEN 'archive'
              ELSE 'document'
            END
        WHERE document_type IS NULL;
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

    // ------------------------------------------------------------------
    // ir_ai_settings — the AI agent configuration (docs/110).
    //
    // ONE ROW, id=1. Deliberately a table of its own rather than rows in
    // ir_config_parameter: that table is a plain key/value store with no
    // access control, so any authenticated user could search_read the API key
    // straight out of it.
    //
    // The key is stored here in plaintext, by decision (docs/110 §1): the
    // database is then the whole migration unit -- restore the dump on a new
    // machine and it is configured. The cost is that a backup carries a live
    // credential, which is why the template dumps are scrubbed and the backup
    // screen says so.
    // ------------------------------------------------------------------
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS ir_ai_settings (
            id                SERIAL PRIMARY KEY,
            enabled           BOOLEAN NOT NULL DEFAULT FALSE,
            provider          VARCHAR NOT NULL DEFAULT 'anthropic',
            api_key           VARCHAR NOT NULL DEFAULT '',
            model             VARCHAR NOT NULL DEFAULT 'claude-sonnet-5',
            max_output_tokens INTEGER NOT NULL DEFAULT 2048,
            daily_call_cap    INTEGER NOT NULL DEFAULT 200,
            calls_today       INTEGER NOT NULL DEFAULT 0,
            calls_date        DATE,
            last_ok_at        TIMESTAMP,
            last_error        VARCHAR NOT NULL DEFAULT '',
            -- Configurable so the same binary can talk straight to Anthropic or
            -- through an nginx egress proxy. One row differs; no code does.
            api_base_url      VARCHAR NOT NULL DEFAULT 'https://api.anthropic.com',
            -- Identity-linked API keys must name the workspace they act in.
            -- Not a secret: it is an identifier, and hiding it would only make
            -- the setup harder to check.
            workspace_id      VARCHAR NOT NULL DEFAULT '',
            create_date       TIMESTAMP DEFAULT now(),
            write_date        TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec("INSERT INTO ir_ai_settings (id) VALUES (1) ON CONFLICT (id) DO NOTHING");
    // ---- AI agent configuration (docs/110) --------------------------------
    // All of it here, in ensureSchema_, because this is the function that runs
    // on every boot. An earlier version of this landed in the viewmodel's
    // row_() helper by mistake, where one ALTER sat inside an `if (r.empty())`
    // that is false whenever the row exists -- so it never ran, and three
    // schema changes silently never reached any existing database.
    //
    // No migration framework is needed: IF NOT EXISTS is idempotent, so a
    // fresh database and an existing one converge on the same shape. What is
    // needed is that it actually executes.
    txn.exec("ALTER TABLE ir_ai_settings ADD COLUMN IF NOT EXISTS api_base_url "
             "VARCHAR NOT NULL DEFAULT 'https://api.anthropic.com'");
    txn.exec("ALTER TABLE ir_ai_settings ADD COLUMN IF NOT EXISTS workspace_id "
             "VARCHAR NOT NULL DEFAULT ''");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS ir_ai_provider (
            name         VARCHAR PRIMARY KEY,
            label        VARCHAR NOT NULL DEFAULT '',
            api_key      VARCHAR NOT NULL DEFAULT '',
            base_url     VARCHAR NOT NULL DEFAULT '',
            path         VARCHAR NOT NULL DEFAULT '/v1/chat/completions',
            model        VARCHAR NOT NULL DEFAULT '',
            auth_style   VARCHAR NOT NULL DEFAULT 'bearer',
            workspace_id VARCHAR NOT NULL DEFAULT '',
            write_date   TIMESTAMP DEFAULT now()
        )
    )");
    // auth_style is the only thing that really differs between providers:
    // Anthropic wants x-api-key plus a version header; the OpenAI-compatible
    // ones (xAI included) want Authorization: Bearer.
    txn.exec(R"(
        INSERT INTO ir_ai_provider (name, label, base_url, path, model, auth_style) VALUES
            ('anthropic', 'Anthropic (Claude)',       'https://api.anthropic.com', '/v1/messages',
             'claude-sonnet-5', 'anthropic'),
            ('xai',       'xAI (Grok)',               'https://api.x.ai',          '/v1/chat/completions',
             'grok-3', 'bearer'),
            ('mock',      'Mock (tests, no network)', '', '', 'mock', 'none')
        ON CONFLICT (name) DO UPDATE
            SET label=EXCLUDED.label, auth_style=EXCLUDED.auth_style
    )");
    // A key entered before this table existed follows its provider across.
    txn.exec(R"(
        UPDATE ir_ai_provider p SET api_key = s.api_key, workspace_id = s.workspace_id
          FROM ir_ai_settings s
         WHERE p.name='anthropic' AND p.api_key='' AND s.id=1 AND s.api_key <> ''
    )");

    // How this provider is asked to SEARCH THE WEB. Without it a lookup is
    // answered from training data alone — which is how a model returns a
    // confident part number and a source URL it has never read. The two
    // vendors expose the capability differently and neither is guessable, so
    // it is stored rather than compiled in: a provider whose API changes is
    // then a settings fix, not a rebuild.
    //
    //   anthropic_tool  — a server tool in `tools` on the messages endpoint
    //   responses_tool  — an OpenAI-style *Responses* call on its own path,
    //                     which is a THIRD wire shape: `input` rather than
    //                     `messages`, and an `output[]` array back
    //   ''              — cannot search; answers from training data only
    //
    // xAI's original `search_parameters` was retired and now answers 410 with
    // "Live search is deprecated"; the working route is the Responses endpoint
    // with a web_search tool. That is exactly the kind of change this table
    // exists to absorb without a rebuild.
    txn.exec("ALTER TABLE ir_ai_provider ADD COLUMN IF NOT EXISTS search_style "
             "VARCHAR NOT NULL DEFAULT ''");
    txn.exec("ALTER TABLE ir_ai_provider ADD COLUMN IF NOT EXISTS search_tool "
             "VARCHAR NOT NULL DEFAULT ''");
    txn.exec("ALTER TABLE ir_ai_provider ADD COLUMN IF NOT EXISTS search_path "
             "VARCHAR NOT NULL DEFAULT ''");
    txn.exec(R"(
        UPDATE ir_ai_provider SET search_style='anthropic_tool',
               search_tool='web_search_20250305', search_path=''
         WHERE name='anthropic'
    )");
    txn.exec(R"(
        UPDATE ir_ai_provider SET search_style='responses_tool',
               search_tool='web_search', search_path='/v1/responses'
         WHERE name='xai'
    )");

    // Prompt OVERRIDES only. The shipped text lives in prompts/*.md and is
    // git-tracked; a row appears here only when somebody edits one on screen,
    // and deleting the row hands control back to the file. Keeping the
    // defaults out of the database is what makes "reset" meaningful.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS ir_ai_prompt (
            task       VARCHAR PRIMARY KEY,
            body       TEXT    NOT NULL,
            updated_by INTEGER,
            write_date TIMESTAMP DEFAULT now()
        )
    )");

    // Searching costs money and latency, so it is a setting rather than an
    // assumption. On by default: a part lookup that cannot read the web is
    // the failure mode this whole feature exists to avoid.
    txn.exec("ALTER TABLE ir_ai_settings ADD COLUMN IF NOT EXISTS web_search "
             "BOOLEAN NOT NULL DEFAULT TRUE");
    txn.exec("ALTER TABLE ir_ai_settings ADD COLUMN IF NOT EXISTS max_candidates "
             "INTEGER NOT NULL DEFAULT 3");


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

    // Settings -> AI Agent (docs/110). Ids 117 / 403 are the next free pair;
    // scripts/verify_menu_ids.sh fails the suite on a reused id.
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, path, context) VALUES
            (117, 'AI Agent', 'ir.ai.settings', 'form', 'ai-agent', '{}')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
                view_mode=EXCLUDED.view_mode, path=EXCLUDED.path
    )");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (403, 'AI Agent', 30, 45, 117)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

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

    // NOTE: auth_signup.allow / auth_signup.reset_pwd used to live here. Self-
    // registration and self-service password reset were removed as a matter of
    // policy (accounts are created by an administrator; resets are issued by an
    // admin-generated link — see AuthSignupModule and res.users
    // action_generate_reset_link), so these flags no longer gate anything and
    // are not seeded. web.base.url stays: the reset link the admin sends is
    // built from it.
    txn.exec(R"(
        INSERT INTO ir_config_parameter (key, value) VALUES
            ('web.base.url',           'http://localhost:8069'),
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

} // namespace cerp::modules::ir

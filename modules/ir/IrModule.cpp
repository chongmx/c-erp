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
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
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

// ================================================================
// 2. VIEWMODELS
// ================================================================

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
// 3. MODULE
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
}

void IrModule::registerServices()   {}
void IrModule::registerViews()      {}
void IrModule::registerRoutes()     {}

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
    viewModels_.registerCreator("ir.config.parameter", [db]{
        return std::make_shared<core::GenericViewModel<IrConfigParameter>>(db);
    });
}

void IrModule::initialize() {
    ensureSchema_();
    seedActions_();
    seedMenus_();
    seedConfigParams_();
    seedRules_();
    // S-30: start the rule engine so BaseModel can enforce record-level rules
    core::RuleEngine::initialize(services_.db());
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

    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id, web_icon) VALUES
            (10, 'Accounting', NULL, 10, NULL, 'accounting'),
            (20, 'Contacts',   NULL, 20, NULL, 'contacts'),
            (30, 'Settings',   NULL, 30, NULL, 'settings')
        ON CONFLICT (id) DO NOTHING
    )");

    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (21, 'Contacts', 20, 10, 1)
        ON CONFLICT (id) DO NOTHING
    )");

    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (31, 'Users',        30, 10, 2),
            (32, 'Companies',    30, 20, 3)
        ON CONFLICT (id) DO NOTHING
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

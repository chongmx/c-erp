#pragma once
// =============================================================
// modules/mail/MailModule.hpp
//
// Phase 27 — Chatter / Audit Log
//
// Provides:
//   mail.message  (table: mail_message)
//     - res_model, res_id  : polymorphic target
//     - author_id          : FK → res_users (nullable)
//     - body               : message text
//     - subtype            : 'note' | 'comment'
//     - date               : auto-set at insert
//
//   MailMessageViewModel  — search_read + create
//     search_read does a LEFT JOIN on res_users to return
//     author_name in every record.
// =============================================================
#include "MailHelpers.hpp"
#include "IModule.hpp"
#include "Factories.hpp"
#include "BaseModel.hpp"
#include "BaseView.hpp"
#include "BaseViewModel.hpp"
#include "DbConnection.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::mail {

using namespace odoo::infrastructure;
using namespace odoo::core;

// ================================================================
// MailMessage — mail.message
// ================================================================
class MailMessage : public BaseModel<MailMessage> {
public:
    static constexpr const char* MODEL_NAME = "mail.message";
    static constexpr const char* TABLE_NAME = "mail_message";

    std::string resModel;
    int         resId     = 0;
    int         authorId  = 0;
    std::string body;
    std::string subtype   = "note";
    std::string date;

    explicit MailMessage(std::shared_ptr<DbConnection> db)
        : BaseModel(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"res_model", FieldType::Char,    "Model"});
        fieldRegistry_.add({"res_id",    FieldType::Integer, "Record ID"});
        fieldRegistry_.add({"author_id", FieldType::Many2one,"Author",  false, false, true, false, "res.users"});
        fieldRegistry_.add({"body",      FieldType::Text,    "Body"});
        fieldRegistry_.add({"subtype",   FieldType::Char,    "Subtype"});
        fieldRegistry_.add({"date",      FieldType::Char,    "Date"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["res_model"] = resModel;
        j["res_id"]    = resId;
        j["author_id"] = authorId > 0 ? nlohmann::json(authorId) : nlohmann::json(false);
        j["body"]      = body;
        j["subtype"]   = subtype;
        j["date"]      = date;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("res_model") && j["res_model"].is_string()) resModel = j["res_model"];
        if (j.contains("res_id")    && j["res_id"].is_number())    resId    = j["res_id"];
        if (j.contains("author_id") && j["author_id"].is_number()) authorId = j["author_id"];
        if (j.contains("body")      && j["body"].is_string())      body     = j["body"];
        if (j.contains("subtype")   && j["subtype"].is_string())   subtype  = j["subtype"];
        if (j.contains("date")      && j["date"].is_string())      date     = j["date"];
    }

    std::vector<std::string> validate() const override { return {}; }
};

// ================================================================
// MailMessageViewModel — search_read + create
// ================================================================
class MailMessageViewModel : public BaseViewModel {
public:
    explicit MailMessageViewModel(std::shared_ptr<DbConnection> db)
        : db_(std::move(db))
    {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("create",          handleCreate)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
    }

    std::string modelName() const override { return "mail.message"; }

private:
    std::shared_ptr<DbConnection> db_;

    // ----------------------------------------------------------
    // search_read — expects domain like:
    //   [['res_model','=','sale.order'],['res_id','=',123]]
    // Returns records with author_name included.
    // ----------------------------------------------------------
    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        const auto& dom = call.domain();

        // Parse domain for res_model and res_id filters
        std::string filterModel;
        int         filterId    = 0;

        for (const auto& leaf : dom) {
            if (!leaf.is_array() || leaf.size() != 3) continue;
            std::string field = leaf[0].get<std::string>();
            std::string op    = leaf[1].get<std::string>();
            if (op != "=") continue;
            if (field == "res_model" && leaf[2].is_string())
                filterModel = leaf[2].get<std::string>();
            else if (field == "res_id" && leaf[2].is_number_integer())
                filterId = leaf[2].get<int>();
        }

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        // Build WHERE clause
        std::string where = "1=1";
        std::vector<std::string> params;
        int pn = 1;
        if (!filterModel.empty()) {
            where += " AND mm.res_model = $" + std::to_string(pn++);
            params.push_back(filterModel);
        }
        if (filterId > 0) {
            where += " AND mm.res_id = $" + std::to_string(pn++);
            params.push_back(std::to_string(filterId));
        }

        std::string sql =
            "SELECT mm.id, mm.res_model, mm.res_id, mm.author_id, "
            "       mm.body, mm.subtype, "
            "       to_char(mm.date AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI') AS date, "
            "       COALESCE(rp.name, ru.login, 'System') AS author_name "
            "FROM mail_message mm "
            "LEFT JOIN res_users ru ON ru.id = mm.author_id "
            "LEFT JOIN res_partner rp ON rp.id = ru.partner_id "
            "WHERE " + where + " "
            "ORDER BY mm.date ASC "
            "LIMIT 100";

        pqxx::result rows;
        if (params.empty()) {
            rows = txn.exec(sql);
        } else if (params.size() == 1) {
            rows = txn.exec(sql, pqxx::params{params[0]});
        } else {
            rows = txn.exec(sql, pqxx::params{params[0], params[1]});
        }

        nlohmann::json result = nlohmann::json::array();
        for (const auto& row : rows) {
            nlohmann::json rec;
            rec["id"]          = row["id"].as<int>();
            rec["res_model"]   = row["res_model"].is_null() ? "" : row["res_model"].c_str();
            rec["res_id"]      = row["res_id"].is_null()    ? 0  : row["res_id"].as<int>();
            rec["body"]        = row["body"].is_null()       ? "" : row["body"].c_str();
            rec["subtype"]     = row["subtype"].is_null()    ? "" : row["subtype"].c_str();
            rec["date"]        = row["date"].is_null()       ? "" : row["date"].c_str();
            rec["author_name"] = row["author_name"].is_null() ? "System" : row["author_name"].c_str();
            rec["author_id"]   = row["author_id"].is_null()   ? nlohmann::json(false) : nlohmann::json(row["author_id"].as<int>());
            result.push_back(rec);
        }
        return result;
    }

    // ----------------------------------------------------------
    // create — inserts a single mail.message row
    // ----------------------------------------------------------
    nlohmann::json handleCreate(const CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("mail.message create: args[0] must be a dict");

        std::string resModel = v.value("res_model", "");
        int         resId    = v.value("res_id",    0);
        int         authorId = 0;
        if (v.contains("author_id") && v["author_id"].is_number_integer())
            authorId = v["author_id"].get<int>();
        std::string body    = v.value("body",    "");
        std::string subtype = v.value("subtype", "note");

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        postLog(txn, resModel, resId, authorId, body, subtype);
        auto row = txn.exec("SELECT lastval()");
        int newId = row[0][0].as<int>();
        txn.commit();
        return newId;
    }

    nlohmann::json handleFieldsGet(const CallKwArgs&) {
        return {
            {"res_model",   {{"type","char"},    {"string","Model"}}},
            {"res_id",      {{"type","integer"}, {"string","Record ID"}}},
            {"author_id",   {{"type","many2one"},{"string","Author"},{"relation","res.users"}}},
            {"body",        {{"type","text"},    {"string","Body"}}},
            {"subtype",     {{"type","char"},    {"string","Subtype"}}},
            {"date",        {{"type","char"},    {"string","Date"}}},
            {"author_name", {{"type","char"},    {"string","Author Name"}}},
        };
    }
};

// ================================================================
// MailModule — IModule implementation
// ================================================================
class MailModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "mail"; }

    explicit MailModule(core::ModelFactory&     modelFactory,
                        core::ServiceFactory&   serviceFactory,
                        core::ViewModelFactory& viewModelFactory,
                        core::ViewFactory&      viewFactory)
        : models_    (modelFactory)
        , services_  (serviceFactory)
        , viewModels_(viewModelFactory)
        , views_     (viewFactory)
    {}

    std::string              moduleName()   const override { return "mail"; }
    std::string              version()      const override { return "17.0.1.0.0"; }
    std::vector<std::string> dependencies() const override { return {"auth"}; }

    void registerModels()     override {}
    void registerServices()   override {}
    void registerViews()      override {}

    void registerViewModels() override {
        auto db = services_.db();
        viewModels_.registerCreator("mail.message", [db]{
            return std::make_shared<MailMessageViewModel>(db);
        });
    }

    void initialize() override {
        ensureSchema_();
    }

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    void ensureSchema_() {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(
            "CREATE TABLE IF NOT EXISTS mail_message ("
            "  id        SERIAL PRIMARY KEY, "
            "  res_model TEXT, "
            "  res_id    INTEGER, "
            "  author_id INTEGER REFERENCES res_users(id) ON DELETE SET NULL, "
            "  body      TEXT NOT NULL DEFAULT '', "
            "  subtype   TEXT NOT NULL DEFAULT 'note', "
            "  date      TIMESTAMPTZ NOT NULL DEFAULT now() "
            ")");
        txn.exec(
            "CREATE INDEX IF NOT EXISTS mail_message_target_idx "
            "ON mail_message (res_model, res_id)");
        txn.commit();
    }
};

} // namespace odoo::modules::mail

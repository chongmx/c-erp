#pragma once
#include "IModule.hpp"
#include "Factories.hpp"
#include "ResCompany.hpp"
#include "ResGroups.hpp"
#include "ResUsers.hpp"
#include "AuthService.hpp"
#include "AuthViewModel.hpp"
#include "AuthViews.hpp"
#include "DbConnection.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace odoo::modules::auth {

// ================================================================
// AuthModule
// ================================================================
/**
 * @brief Registers res.users, res.groups, res.company and AuthService.
 *
 * Also creates the required tables and seeds default groups if they
 * do not yet exist (idempotent — safe to call on every boot).
 *
 * Depends on: "base"   (res.partner must exist first)
 *
 * Usage in main():
 * @code
 *   container->addModule<odoo::modules::auth::AuthModule>();
 * @endcode
 */
class AuthModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "auth"; }

    explicit AuthModule(core::ModelFactory&     modelFactory,
                        core::ServiceFactory&   serviceFactory,
                        core::ViewModelFactory& viewModelFactory,
                        core::ViewFactory&      viewFactory)
        : models_    (modelFactory)
        , services_  (serviceFactory)
        , viewModels_(viewModelFactory)
        , views_     (viewFactory)
    {}

    std::string              moduleName()   const override { return "auth"; }
    std::string              version()      const override { return "19.0.1.0.0"; }
    std::vector<std::string> dependencies() const override { return {"base"}; }

    // ----------------------------------------------------------
    // Boot sequence
    // ----------------------------------------------------------

    void registerModels() override {
        auto db = services_.db();
        models_.registerCreator("res.company", [db] {
            return std::make_shared<ResCompany>(db);
        });
        models_.registerCreator("res.groups", [db] {
            return std::make_shared<ResGroups>(db);
        });
        models_.registerCreator("res.users", [db] {
            return std::make_shared<ResUsers>(db);
        });
    }

    void registerServices() override {
        auto db = services_.db();
        services_.registerCreator("auth", [db] {
            return std::make_shared<AuthService>(db);
        });
    }

    void registerViews() override {
        views_.registerView<UsersListView>  ("res.users.list");
        views_.registerView<UsersFormView>  ("res.users.form");
        views_.registerView<CompanyListView>("res.company.list");
        views_.registerView<CompanyFormView>("res.company.form");
        views_.registerView<GroupsListView> ("res.groups.list");
    }

    void registerViewModels() override {
        auto& sf  = services_;
        auto& vf  = views_;
        auto  db  = services_.db();

        viewModels_.registerCreator("res.users", [&sf, &vf, db] {
            auto auth = std::static_pointer_cast<AuthService>(
                sf.create("auth", core::Lifetime::Singleton));
            auto sessions = std::make_shared<infrastructure::SessionManager>();
            auto vfPtr    = std::shared_ptr<core::ViewFactory>(&vf, [](auto*){});
            return std::make_shared<AuthViewModel>(auth, sessions, db, db->config().name);
        });

        viewModels_.registerCreator("res.company", [db] {
            return std::make_shared<CompanyViewModel>(db);
        });

        viewModels_.registerCreator("res.groups", [db] {
            return std::make_shared<GroupsViewModel>(db);
        });
    }

    void registerRoutes() override {}

    // ----------------------------------------------------------
    // initialize — runs after all modules are booted
    // Ensures tables exist and seeds default data.
    // ----------------------------------------------------------
    void initialize() override {
        ensureSchema_();
        seedGroups_();
        seedGroupPermissions_();
        seedAdminUser_();
    }

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    // ----------------------------------------------------------
    // Full CRUD ViewModel for res.company
    // ----------------------------------------------------------
    class CompanyViewModel : public core::BaseViewModel {
    public:
        explicit CompanyViewModel(std::shared_ptr<infrastructure::DbConnection> db)
            : db_(std::move(db))
        {
            REGISTER_METHOD("search_read",  handleSearchRead)
            REGISTER_METHOD("read",         handleRead)
            REGISTER_METHOD("web_read",     handleRead)
            REGISTER_METHOD("create",       handleCreate)
            REGISTER_METHOD("write",        handleWrite)
            REGISTER_METHOD("unlink",       handleUnlink)
            REGISTER_METHOD("fields_get",   handleFieldsGet)
            REGISTER_METHOD("search_count", handleSearchCount)
        }

        std::string modelName() const override { return "res.company"; }

    private:
        std::shared_ptr<infrastructure::DbConnection> db_;

        nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
            ResCompany proto(db_);
            return proto.searchRead(call.domain(), call.fields(),
                                    call.limit() > 0 ? call.limit() : 80,
                                    call.offset(), "id ASC");
        }
        nlohmann::json handleRead(const core::CallKwArgs& call) {
            ResCompany proto(db_);
            return proto.read(call.ids(), call.fields());
        }
        nlohmann::json handleCreate(const core::CallKwArgs& call) {
            const auto v = call.arg(0);
            if (!v.is_object()) throw std::runtime_error("create: args[0] must be a dict");
            ResCompany proto(db_);
            return proto.create(v);
        }
        nlohmann::json handleWrite(const core::CallKwArgs& call) {
            const auto v = call.arg(1);
            if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
            ResCompany proto(db_);
            return proto.write(call.ids(), v);
        }
        nlohmann::json handleUnlink(const core::CallKwArgs& call) {
            ResCompany proto(db_);
            return proto.unlink(call.ids());
        }
        nlohmann::json handleFieldsGet(const core::CallKwArgs& call) {
            ResCompany proto(db_);
            return proto.fieldsGet(call.fields());
        }
        nlohmann::json handleSearchCount(const core::CallKwArgs& call) {
            ResCompany proto(db_);
            return proto.searchCount(call.domain());
        }
    };

    // ----------------------------------------------------------
    // Schema creation — idempotent
    // ----------------------------------------------------------
    void ensureSchema_() {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};

        // res_company
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS res_company (
                id          SERIAL  PRIMARY KEY,
                name        VARCHAR NOT NULL,
                email       VARCHAR,
                phone       VARCHAR,
                website     VARCHAR,
                vat         VARCHAR,
                parent_id   INTEGER REFERENCES res_company(id)  ON DELETE SET NULL,
                active      BOOLEAN   NOT NULL DEFAULT TRUE,
                create_date TIMESTAMP DEFAULT now(),
                write_date  TIMESTAMP DEFAULT now()
            )
        )");
        // partner_id and currency_id added after base module runs
        txn.exec("ALTER TABLE res_company ADD COLUMN IF NOT EXISTS "
                 "partner_id  INTEGER REFERENCES res_partner(id)  ON DELETE SET NULL");
        txn.exec("ALTER TABLE res_company ADD COLUMN IF NOT EXISTS "
                 "currency_id INTEGER REFERENCES res_currency(id) ON DELETE SET NULL");

        // res_groups
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS res_groups (
                id          SERIAL PRIMARY KEY,
                name        VARCHAR NOT NULL,
                full_name   VARCHAR,
                share       BOOLEAN NOT NULL DEFAULT FALSE,
                permissions JSONB    NOT NULL DEFAULT '[]',
                create_date TIMESTAMP DEFAULT now(),
                write_date  TIMESTAMP DEFAULT now()
            )
        )");
        // Migration: add permissions column to existing databases
        txn.exec(
            "ALTER TABLE res_groups "
            "ADD COLUMN IF NOT EXISTS permissions JSONB NOT NULL DEFAULT '[]'");

        // res_users
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS res_users (
                id          SERIAL PRIMARY KEY,
                login       VARCHAR NOT NULL UNIQUE,
                password    VARCHAR NOT NULL,
                partner_id  INTEGER REFERENCES res_partner(id),
                company_id  INTEGER REFERENCES res_company(id),
                lang        VARCHAR NOT NULL DEFAULT 'en_US',
                tz          VARCHAR NOT NULL DEFAULT 'UTC',
                active      BOOLEAN NOT NULL DEFAULT TRUE,
                share       BOOLEAN NOT NULL DEFAULT FALSE,
                create_date TIMESTAMP DEFAULT now(),
                write_date  TIMESTAMP DEFAULT now()
            )
        )");

        // res_groups_users_rel — Many2many junction
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS res_groups_users_rel (
                gid INTEGER NOT NULL REFERENCES res_groups(id) ON DELETE CASCADE,
                uid INTEGER NOT NULL REFERENCES res_users(id)  ON DELETE CASCADE,
                PRIMARY KEY (gid, uid)
            )
        )");

        txn.commit();
    }

    // ----------------------------------------------------------
    // Seed default groups (fully idempotent — safe to run on upgrade)
    // ----------------------------------------------------------
    void seedGroups_() {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};

        // Each row uses ON CONFLICT (id) DO NOTHING so new groups are added
        // on upgrade without touching existing ones.
        txn.exec(R"(
            INSERT INTO res_groups (id, name, full_name, share) VALUES
                -- ── Base ──────────────────────────────────────────────────
                (1,  'Public',        'Base / Public',                     TRUE),
                (2,  'Internal User', 'Base / Internal User',              FALSE),
                (3,  'Administrator', 'Base / Administrator',              FALSE),
                -- ── Settings ──────────────────────────────────────────────
                (4,  'Configuration', 'Settings / Configuration',          FALSE),
                -- ── Accounting ────────────────────────────────────────────
                (5,  'Billing',       'Accounting / Billing',              FALSE),
                (6,  'Accountant',    'Accounting / Accountant',           FALSE),
                -- ── Sales ─────────────────────────────────────────────────
                (7,  'User',          'Sales / User',                      FALSE),
                (8,  'Manager',       'Sales / Manager',                   FALSE),
                -- ── Purchase ──────────────────────────────────────────────
                (9,  'User',          'Purchase / User',                   FALSE),
                (10, 'Manager',       'Purchase / Manager',                FALSE),
                -- ── Inventory ─────────────────────────────────────────────
                (11, 'User',          'Inventory / User',                  FALSE),
                (12, 'Manager',       'Inventory / Manager',               FALSE),
                -- ── Manufacturing ─────────────────────────────────────────
                (13, 'User',          'Manufacturing / User',              FALSE),
                (14, 'Manager',       'Manufacturing / Manager',           FALSE),
                -- ── Human Resources ───────────────────────────────────────
                (15, 'Employee',      'Human Resources / Employee',        FALSE),
                (16, 'Manager',       'Human Resources / Manager',         FALSE)
            ON CONFLICT (id) DO NOTHING
        )");
        // Advance sequence past all seeded IDs
        txn.exec("SELECT setval('res_groups_id_seq', 16, true)");
        txn.commit();
    }

    // ----------------------------------------------------------
    // Seed default permissions for all 16 groups (idempotent —
    // only writes rows whose permissions are still empty '[]').
    // ----------------------------------------------------------
    void seedGroupPermissions_() {
        // permission string arrays keyed by group id
        static const std::vector<std::pair<int,std::string>> defaults = {
            // 1 — Public / Customer (portal users: read-only view of their own contact)
            {1,  R"(["partner.view"])"},
            // 2 — Internal User (basic read access across all modules)
            {2,  R"(["partner.view","partner.create","product.view",
                      "sale.view_orders","purchase.view_orders",
                      "stock.view_transfers","hr.view_employees","hr.view_departments",
                      "hr.view_schedules"])"},
            // 3 — Administrator (full access to everything)
            {3,  R"(["partner.view","partner.create",
                      "product.view","product.create","product.manage_cats","product.manage_uom",
                      "account.view_invoices","account.create_invoices","account.validate_invoices",
                      "account.view_bills","account.create_bills","account.manage_accounts",
                      "account.view_journals","account.manage_journals",
                      "sale.view_orders","sale.create_orders","sale.confirm_orders","sale.override_price",
                      "purchase.view_orders","purchase.create_orders","purchase.approve_orders",
                      "stock.view_transfers","stock.create_transfers","stock.validate_transfers",
                      "stock.manage_locations","stock.manage_warehouses","stock.manage_op_types",
                      "mrp.view_bom","mrp.manage_bom","mrp.view_orders","mrp.create_orders","mrp.validate_orders",
                      "hr.view_employees","hr.manage_employees","hr.view_departments","hr.manage_departments",
                      "hr.view_schedules","hr.manage_schedules",
                      "settings.view_users","settings.manage_users","settings.manage_groups",
                      "settings.manage_companies","settings.erp_config","settings.technical"])"},
            // 4 — Configuration (settings power user)
            {4,  R"(["partner.view","partner.create","product.view",
                      "sale.view_orders","purchase.view_orders","stock.view_transfers",
                      "hr.view_employees","settings.view_users","settings.manage_users",
                      "settings.manage_groups","settings.manage_companies",
                      "settings.erp_config","settings.technical"])"},
            // 5 — Billing (create invoices & bills, no validation)
            {5,  R"(["partner.view","partner.create","product.view",
                      "account.view_invoices","account.create_invoices",
                      "account.view_bills","account.create_bills","account.view_journals"])"},
            // 6 — Accountant (full accounting)
            {6,  R"(["partner.view","partner.create","product.view",
                      "account.view_invoices","account.create_invoices","account.validate_invoices",
                      "account.view_bills","account.create_bills",
                      "account.manage_accounts","account.view_journals","account.manage_journals"])"},
            // 7 — Sales User
            {7,  R"(["partner.view","partner.create","product.view",
                      "sale.view_orders","sale.create_orders","sale.confirm_orders"])"},
            // 8 — Sales Manager
            {8,  R"(["partner.view","partner.create","product.view","product.create",
                      "sale.view_orders","sale.create_orders","sale.confirm_orders","sale.override_price"])"},
            // 9 — Purchase User
            {9,  R"(["partner.view","partner.create","product.view",
                      "purchase.view_orders","purchase.create_orders"])"},
            // 10 — Purchase Manager
            {10, R"(["partner.view","partner.create","product.view","product.create",
                      "purchase.view_orders","purchase.create_orders","purchase.approve_orders"])"},
            // 11 — Inventory User
            {11, R"(["product.view",
                      "stock.view_transfers","stock.create_transfers","stock.validate_transfers"])"},
            // 12 — Inventory Manager
            {12, R"(["product.view","product.create",
                      "stock.view_transfers","stock.create_transfers","stock.validate_transfers",
                      "stock.manage_locations","stock.manage_warehouses","stock.manage_op_types"])"},
            // 13 — Manufacturing User
            {13, R"(["product.view",
                      "mrp.view_bom","mrp.view_orders","mrp.create_orders"])"},
            // 14 — Manufacturing Manager
            {14, R"(["product.view","product.create",
                      "mrp.view_bom","mrp.manage_bom",
                      "mrp.view_orders","mrp.create_orders","mrp.validate_orders"])"},
            // 15 — HR Employee (self-service read)
            {15, R"(["hr.view_employees","hr.view_departments","hr.view_schedules"])"},
            // 16 — HR Manager (full HR)
            {16, R"(["hr.view_employees","hr.manage_employees",
                      "hr.view_departments","hr.manage_departments",
                      "hr.view_schedules","hr.manage_schedules"])"},
        };
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};
        // Only seed groups whose permissions are still the default empty array
        for (const auto& [gid, perms] : defaults) {
            // Compact the JSON (remove whitespace/newlines) before storing
            nlohmann::json parsed = nlohmann::json::parse(perms);
            txn.exec(
                "UPDATE res_groups SET permissions=$2::jsonb "
                "WHERE id=$1 AND permissions='[]'::jsonb",
                pqxx::params{gid, parsed.dump()});
        }
        txn.commit();
    }

    // ----------------------------------------------------------
    // Seed admin user + default company (idempotent)
    // ----------------------------------------------------------
    void seedAdminUser_() {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};

        if (txn.exec("SELECT COUNT(*) FROM res_users")[0][0].as<int>() > 0) return;

        // Partner for the company
        auto compPartner = txn.exec(
            "INSERT INTO res_partner (name, email, is_company) "
            "VALUES ('My Company', 'company@example.com', TRUE) "
            "RETURNING id");
        const int compPartnerId = compPartner[0][0].as<int>();

        // Default company (currency_id=1 = USD seeded by base module)
        auto companyRes = txn.exec(
            "INSERT INTO res_company (id, name, partner_id, currency_id) "
            "VALUES (1, 'My Company', $1, 1) "
            "ON CONFLICT (id) DO NOTHING RETURNING id",
            pqxx::params{compPartnerId});
        txn.exec("SELECT setval('res_company_id_seq', 1, true)");

        // Partner for the admin user
        auto partnerRes = txn.exec(
            "INSERT INTO res_partner (name, email) "
            "VALUES ('Administrator', 'admin@example.com') "
            "RETURNING id");
        const int adminPartnerId = partnerRes[0][0].as<int>();

        // Admin user
        const std::string hash = AuthService::hashPassword("admin");
        txn.exec(
            "INSERT INTO res_users (id, login, password, partner_id, company_id, "
            "                       lang, tz, active, share) "
            "VALUES (1, 'admin', $1, $2, 1, 'en_US', 'UTC', TRUE, FALSE) "
            "ON CONFLICT (login) DO NOTHING",
            pqxx::params{hash, adminPartnerId});

        // Add admin to Internal User + Administrator groups
        txn.exec(
            "INSERT INTO res_groups_users_rel (gid, uid) VALUES (2, 1),(3, 1) "
            "ON CONFLICT DO NOTHING");

        txn.exec("SELECT setval('res_users_id_seq', 1, true)");
        txn.commit();
    }

    // ----------------------------------------------------------
    // GroupsViewModel — full CRUD for res.groups
    // ----------------------------------------------------------
    class GroupsViewModel : public core::BaseViewModel {
    public:
        explicit GroupsViewModel(std::shared_ptr<infrastructure::DbConnection> db)
            : db_(std::move(db))
        {
            REGISTER_METHOD("search_read",  handleSearchRead)
            REGISTER_METHOD("read",         handleRead)
            REGISTER_METHOD("web_read",     handleRead)
            REGISTER_METHOD("create",       handleCreate)
            REGISTER_METHOD("write",        handleWrite)
            REGISTER_METHOD("unlink",       handleUnlink)
            REGISTER_METHOD("fields_get",   handleFieldsGet)
            REGISTER_METHOD("search_count", handleSearchCount)
        }

        std::string modelName() const override { return "res.groups"; }

    private:
        std::shared_ptr<infrastructure::DbConnection> db_;

        nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto rows = txn.exec(
                "SELECT g.id, g.name, g.full_name, g.share, "
                "       COALESCE(g.permissions::text, '[]') AS permissions, "
                "       COUNT(r.uid) AS user_count "
                "FROM res_groups g "
                "LEFT JOIN res_groups_users_rel r ON r.gid = g.id "
                "GROUP BY g.id ORDER BY g.id");
            nlohmann::json result = nlohmann::json::array();
            for (const auto& row : rows) {
                result.push_back({
                    {"id",          row["id"].as<int>()},
                    {"name",        row["name"].c_str()},
                    {"full_name",   row["full_name"].is_null() ? "" : row["full_name"].c_str()},
                    {"share",       std::string(row["share"].c_str()) == "t"},
                    {"permissions", nlohmann::json::parse(row["permissions"].c_str())},
                    {"user_count",  row["user_count"].as<int>()},
                });
            }
            return result;
        }

        nlohmann::json handleRead(const core::CallKwArgs& call) {
            const auto ids = call.ids();
            if (ids.empty()) return nlohmann::json::array();
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            nlohmann::json result = nlohmann::json::array();
            for (int id : ids) {
                auto rows = txn.exec(
                    "SELECT g.id, g.name, g.full_name, g.share, "
                    "       COALESCE(g.permissions::text, '[]') AS permissions, "
                    "       ARRAY(SELECT uid FROM res_groups_users_rel "
                    "             WHERE gid=g.id ORDER BY uid) AS user_ids "
                    "FROM res_groups g WHERE g.id=$1",
                    pqxx::params{id});
                if (rows.empty()) continue;
                const auto& r = rows[0];
                nlohmann::json uArr = nlohmann::json::array();
                // pqxx returns array as "{1,2,3}" string
                std::string arrStr = r["user_ids"].c_str();
                if (arrStr.size() > 2) {
                    arrStr = arrStr.substr(1, arrStr.size() - 2);
                    std::istringstream ss(arrStr);
                    std::string tok;
                    while (std::getline(ss, tok, ','))
                        try { uArr.push_back(std::stoi(tok)); } catch (...) {}
                }
                result.push_back({
                    {"id",          r["id"].as<int>()},
                    {"name",        r["name"].c_str()},
                    {"full_name",   r["full_name"].is_null() ? "" : r["full_name"].c_str()},
                    {"share",       std::string(r["share"].c_str()) == "t"},
                    {"permissions", nlohmann::json::parse(r["permissions"].c_str())},
                    {"user_ids",    uArr},
                });
            }
            return result;
        }

        nlohmann::json handleCreate(const core::CallKwArgs& call) {
            auto vals = call.arg(0).is_object() ? call.arg(0) : nlohmann::json::object();
            const std::string name     = vals.value("name", std::string{});
            const std::string fullName = vals.value("full_name", name);
            const bool        share    = vals.value("share", false);
            const std::string perms    = vals.contains("permissions") && vals["permissions"].is_array()
                                         ? vals["permissions"].dump() : "[]";
            if (name.empty()) throw std::runtime_error("name is required");
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto rows = txn.exec(
                "INSERT INTO res_groups (name, full_name, share, permissions) "
                "VALUES ($1,$2,$3,$4::jsonb) RETURNING id",
                pqxx::params{name, fullName, share, perms});
            txn.commit();
            return rows[0][0].as<int>();
        }

        nlohmann::json handleWrite(const core::CallKwArgs& call) {
            auto vals = call.arg(1).is_object() ? call.arg(1) : nlohmann::json::object();
            for (int id : call.ids()) {
                auto conn = db_->acquire();
                pqxx::work txn{conn.get()};
                // Update only the fields present in vals (avoid blanking absent fields)
                if (vals.contains("name"))
                    txn.exec("UPDATE res_groups SET name=$2 WHERE id=$1",
                        pqxx::params{id, vals["name"].get<std::string>()});
                if (vals.contains("full_name"))
                    txn.exec("UPDATE res_groups SET full_name=$2 WHERE id=$1",
                        pqxx::params{id, vals["full_name"].get<std::string>()});
                if (vals.contains("share"))
                    txn.exec("UPDATE res_groups SET share=$2 WHERE id=$1",
                        pqxx::params{id, vals["share"].get<bool>()});
                if (vals.contains("permissions") && vals["permissions"].is_array())
                    txn.exec("UPDATE res_groups SET permissions=$2::jsonb WHERE id=$1",
                        pqxx::params{id, vals["permissions"].dump()});
                txn.exec("UPDATE res_groups SET write_date=now() WHERE id=$1",
                    pqxx::params{id});
                txn.commit();
            }
            return true;
        }

        nlohmann::json handleUnlink(const core::CallKwArgs& call) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            for (int id : call.ids())
                txn.exec("DELETE FROM res_groups WHERE id=$1", pqxx::params{id});
            txn.commit();
            return true;
        }

        nlohmann::json handleFieldsGet(const core::CallKwArgs&) {
            return {
                {"name",        {{"type","char"},    {"string","Group Name"}, {"required",true}}},
                {"full_name",   {{"type","char"},    {"string","Full Name"}}},
                {"share",       {{"type","boolean"}, {"string","Share/Portal Group"}}},
                {"permissions", {{"type","json"},    {"string","Permissions"}}},
                {"user_count",  {{"type","integer"}, {"string","Users"}}},
                {"user_ids",    {{"type","many2many"},{"string","Users"},{"relation","res.users"}}},
            };
        }

        nlohmann::json handleSearchCount(const core::CallKwArgs&) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            return txn.exec("SELECT COUNT(*) FROM res_groups")[0][0].as<int>();
        }
    };
};

} // namespace odoo::modules::auth
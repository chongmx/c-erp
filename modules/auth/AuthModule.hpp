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
        views_.registerView<UsersFormView> ("res.users.form");
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
            // SessionManager reached via auth service's db — create a fresh
            // one here; the real shared instance is in Container.
            // AuthViewModel's session operations go through the shared
            // SessionManager injected below via initialize().
            auto sessions = std::make_shared<infrastructure::SessionManager>();
            auto vfPtr    = std::shared_ptr<core::ViewFactory>(&vf, [](auto*){});
            return std::make_shared<AuthViewModel>(auth, sessions, db, db->config().name);
        });

        viewModels_.registerCreator("res.company", [db] {
            // Minimal viewmodel — company CRUD goes through a service later
            return std::make_shared<SimpleViewModel>("res.company", db);
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
        seedAdminUser_();
    }

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    // ----------------------------------------------------------
    // Minimal passthrough ViewModel for models without business logic
    // ----------------------------------------------------------
    class SimpleViewModel : public core::BaseViewModel {
    public:
        explicit SimpleViewModel(std::string modelName,
                                  std::shared_ptr<infrastructure::DbConnection> db)
            : modelName_(std::move(modelName)), db_(std::move(db))
        {
            REGISTER_METHOD("search_read", handleSearchRead)
            REGISTER_METHOD("read",        handleRead)
            REGISTER_METHOD("fields_get",  handleFieldsGet)
        }

        std::string modelName() const override { return modelName_; }

    private:
        std::string modelName_;
        std::shared_ptr<infrastructure::DbConnection> db_;

        nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
            // Generic pass-through using raw SQL
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            const std::string table = modelName_;
            // Replace dots with underscores for table name
            std::string tbl = table;
            std::replace(tbl.begin(), tbl.end(), '.', '_');
            const int limit = call.limit() > 0 ? call.limit() : 80;
            auto res = txn.exec(
                "SELECT * FROM " + tbl + " LIMIT " + std::to_string(limit));
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& row : res) {
                nlohmann::json obj;
                for (const auto& f : row)
                    obj[f.name()] = f.is_null()
                        ? nlohmann::json(nullptr)
                        : nlohmann::json(f.c_str());
                arr.push_back(std::move(obj));
            }
            return arr;
        }

        nlohmann::json handleRead(const core::CallKwArgs& call) {
            return handleSearchRead(call);
        }

        nlohmann::json handleFieldsGet(const core::CallKwArgs& /*call*/) {
            return nlohmann::json::object();
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
                create_date TIMESTAMP DEFAULT now(),
                write_date  TIMESTAMP DEFAULT now()
            )
        )");

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
    // Seed default groups (idempotent)
    // ----------------------------------------------------------
    void seedGroups_() {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};

        // Insert built-in groups only if the table is empty
        auto res = txn.exec("SELECT COUNT(*) FROM res_groups");
        if (res[0][0].as<int>() > 0) return;

        txn.exec(R"(
            INSERT INTO res_groups (id, name, full_name, share) VALUES
                (1, 'Public',        'Base / Public',   TRUE),
                (2, 'Internal User', 'Base / Internal', FALSE),
                (3, 'Administrator', 'Base / Admin',    FALSE)
            ON CONFLICT (id) DO NOTHING
        )");
        // Reset sequence to avoid PK collisions on future inserts
        txn.exec("SELECT setval('res_groups_id_seq', 3, true)");
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

        // Add admin to Administrator group (id=3)
        txn.exec(
            "INSERT INTO res_groups_users_rel (gid, uid) VALUES (3, 1) "
            "ON CONFLICT DO NOTHING");

        txn.exec("SELECT setval('res_users_id_seq', 1, true)");
        txn.commit();
    }
};

} // namespace odoo::modules::auth
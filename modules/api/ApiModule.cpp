// =============================================================
// modules/api/ApiModule.cpp — see ApiModule.hpp and docs/reference/api-v1.md
// =============================================================
#include "ApiModule.hpp"
#include "AttachmentStore.hpp"
#include "BaseViewModel.hpp"
#include "DbConnection.hpp"
#include "Errors.hpp"
#include "Filestore.hpp"
#include "MigrationRunner.hpp"
#include "UserContext.hpp"
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <pqxx/pqxx>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace cerp::modules::api {

using namespace cerp::infrastructure;
using namespace cerp::core;
using json = nlohmann::json;

// ================================================================
// Scopes — what a key may do. Grouped by area; the issue tracker is first.
// A write scope implies tickets:read (a write answers with the ticket).
// ================================================================
struct ScopeDef { const char* name; const char* area; const char* label; const char* description; };
static const std::vector<ScopeDef>& scopeDefs() {
    static const std::vector<ScopeDef> k = {
        {"tickets:read",      "Issue tracker", "Read tickets",
         "List and search tickets; read a ticket, its comments, history and attachments; "
         "list projects, statuses, labels and users."},
        {"tickets:write",     "Issue tracker", "Create and update tickets",
         "Create tickets and change status, assignee, reporter, type, priority, labels, "
         "due date, estimate, parent and project; watch and unwatch."},
        {"comments:write",    "Issue tracker", "Comment",
         "Post comments, and edit or delete the key owner's own comments."},
        {"attachments:write", "Issue tracker", "Attach files",
         "Upload screenshots and files to tickets, and remove them."},
    };
    return k;
}
static bool knownScope(const std::string& s) {
    for (const auto& d : scopeDefs()) if (s == d.name) return true;
    return false;
}

static std::string randomToken() {
    unsigned char buf[24];
    if (RAND_bytes(buf, sizeof buf) != 1) throw std::runtime_error("RAND_bytes failed");
    static const char* hex = "0123456789abcdef";
    std::string s = "cerp_";
    for (unsigned char b : buf) { s += hex[b >> 4]; s += hex[b & 15]; }
    return s;
}
static std::string pgArray(const std::vector<std::string>& v) {
    std::string s = "{";
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += "\"" + v[i] + "\""; }
    return s + "}";
}
static std::string pgIntArray(const std::vector<int>& v) {
    std::string s = "{";
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += std::to_string(v[i]); }
    return s + "}";
}

// ================================================================
// api.key — managing keys, over JSON-RPC (a real session) only.
// ================================================================
class ApiKeyViewModel : public BaseViewModel {
public:
    explicit ApiKeyViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("scopes",      handleScopes)
        REGISTER_METHOD("list",        handleList)
        REGISTER_MUTATOR("create_key", handleCreate)
        REGISTER_MUTATOR("revoke",     handleRevoke)
    }
    std::string modelName() const override { return "api.key"; }

private:
    std::shared_ptr<DbConnection> db_;

    json handleScopes(const CallKwArgs&) {
        json out = json::array();
        for (const auto& d : scopeDefs())
            out.push_back({{"name", d.name}, {"area", d.area}, {"label", d.label},
                           {"description", d.description}});
        return out;
    }

    static json rowJson(const pqxx::row& r) {
        return {{"id", r["id"].as<int>()}, {"name", r["name"].c_str()},
                {"prefix", r["token_prefix"].c_str()},
                {"user_id", r["user_id"].as<int>()}, {"user", r["user_name"].c_str()},
                {"scopes", json::parse(r["scopes"].c_str(), nullptr, false)},
                {"projects", json::parse(r["projects"].c_str(), nullptr, false)},
                {"created", r["created"].is_null() ? "" : r["created"].c_str()},
                {"expires", r["expires"].is_null() ? "" : r["expires"].c_str()},
                {"last_used", r["last_used"].is_null() ? "" : r["last_used"].c_str()},
                {"last_ip", r["last_used_ip"].is_null() ? "" : r["last_used_ip"].c_str()},
                {"use_count", r["use_count"].as<long long>(0)},
                {"status", r["status"].c_str()}};
    }

    /// Your keys; an administrator also sees everyone's, to revoke them.
    json handleList(const CallKwArgs& call) {
        const auto ctx = extractContext_(call);
        if (ctx.uid <= 0) throw ValidationError("Sign in to manage API keys.");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        const std::string sql =
            "SELECT k.id, k.name, k.token_prefix, k.user_id, "
            "       COALESCE(NULLIF(p.name,''), u.login) AS user_name, "
            "       array_to_json(k.scopes)::text AS scopes, "
            "       COALESCE((SELECT json_agg(json_build_object('id', pp.id, 'prefix', pp.task_prefix, "
            "                                  'name', pp.name) ORDER BY pp.task_prefix) "
            "                   FROM project_project pp WHERE pp.id = ANY(k.project_ids)), '[]')::text AS projects, "
            "       to_char(k.create_date AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created, "
            "       to_char(k.expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS expires, "
            "       to_char(k.last_used_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS last_used, "
            "       k.last_used_ip, k.use_count, "
            "       CASE WHEN k.revoked_at IS NOT NULL THEN 'revoked' "
            "            WHEN k.expires_at IS NOT NULL AND k.expires_at < now() THEN 'expired' "
            "            ELSE 'active' END AS status "
            "FROM res_users_apikey k JOIN res_users u ON u.id = k.user_id "
            "LEFT JOIN res_partner p ON p.id = u.partner_id ";
        json mine = json::array(), all = json::array();
        for (const auto& r : txn.exec(sql + "WHERE k.user_id = $1 ORDER BY k.id DESC", pqxx::params{ctx.uid}))
            mine.push_back(rowJson(r));
        if (ctx.isAdmin)
            for (const auto& r : txn.exec(sql + "ORDER BY k.id DESC LIMIT 500"))
                all.push_back(rowJson(r));
        return {{"mine", mine}, {"all", all}, {"is_admin", ctx.isAdmin}};
    }

    /// A key for the signed-in user. The token is returned ONCE and only its
    /// hash is stored, so it cannot be shown again — lose it, make another.
    json handleCreate(const CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw ValidationError("create_key expects an object.");
        const auto ctx = extractContext_(call);
        if (ctx.uid <= 0) throw ValidationError("Sign in to create an API key.");

        std::string name = v.value("name", std::string{});
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        if (name.empty()) throw ValidationError("Give the key a name — what will use it?");
        if (name.size() > 80) throw ValidationError("A key name is at most 80 characters.");

        std::set<std::string> scopes;
        for (const auto& s : v.value("scopes", json::array())) {
            if (!s.is_string() || !knownScope(s.get<std::string>()))
                throw ValidationError("Unknown scope: " + s.dump());
            scopes.insert(s.get<std::string>());
        }
        if (scopes.empty()) throw ValidationError("Choose at least one permission.");
        scopes.insert("tickets:read");   // every write answers with the ticket

        std::vector<int> projects;
        for (const auto& p : v.value("project_ids", json::array()))
            if (p.is_number_integer() && p.get<int>() > 0) projects.push_back(p.get<int>());
        const int days = v.value("expires_days", 90);
        if (days < 0 || days > 3650) throw ValidationError("Expiry must be 0 (never) to 3650 days.");

        const std::string token = randomToken();
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        if (!projects.empty()) {
            auto n = txn.exec("SELECT count(*) FROM project_project WHERE id = ANY($1::int[])",
                              pqxx::params{pgIntArray(projects)});
            if (n[0][0].as<size_t>() != projects.size()) throw ValidationError("No such project.");
        }
        auto r = txn.exec(
            "INSERT INTO res_users_apikey (user_id, name, token_prefix, token_hash, scopes, "
            "                              project_ids, expires_at, created_by) "
            "VALUES ($1, $2, $3, $4, $5::text[], $6::int[], "
            "        CASE WHEN $7 > 0 THEN now() + make_interval(days => $7) END, $1) RETURNING id",
            pqxx::params{ctx.uid, name, token.substr(0, 13), core::Filestore::sha256Hex(token),
                         pgArray({scopes.begin(), scopes.end()}), pgIntArray(projects), days});
        txn.commit();
        return {{"id", r[0][0].as<int>()}, {"token", token}, {"prefix", token.substr(0, 13)}};
    }

    /// Your own key, or anyone's if you are an administrator. Takes effect on
    /// the key's very next request — nothing is cached.
    json handleRevoke(const CallKwArgs& call) {
        const auto v = call.arg(0);
        const int id = v.is_object() ? v.value("id", 0) : (v.is_number_integer() ? v.get<int>() : 0);
        const auto ctx = extractContext_(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto r = txn.exec("SELECT user_id FROM res_users_apikey WHERE id = $1", pqxx::params{id});
        if (r.empty()) throw ValidationError("No such key.");
        if (r[0][0].as<int>() != ctx.uid && !ctx.isAdmin)
            throw ValidationError("You can only revoke your own keys.");
        txn.exec("UPDATE res_users_apikey SET revoked_at = COALESCE(revoked_at, now()) WHERE id = $1",
                 pqxx::params{id});
        txn.commit();
        return true;
    }
};

// ================================================================
// The REST side
// ================================================================
class ApiError : public std::runtime_error {
public:
    ApiError(int status, std::string code, const std::string& msg)
        : std::runtime_error(msg), status(status), code(std::move(code)) {}
    int status;
    std::string code;
};

/// The key's owner, loaded fresh on every request: a revoked key, a
/// deactivated user or a removed group takes effect immediately.
struct ApiUser {
    int keyId = 0, uid = 0, companyId = 0, partnerId = 0;
    bool isAdmin = false;
    std::string login, name, keyName;
    std::vector<int> groupIds, allowedCompanyIds, projectIds;
    std::set<std::string> scopes;

    bool mayProject(int pid) const {
        return projectIds.empty() || std::find(projectIds.begin(), projectIds.end(), pid) != projectIds.end();
    }
    void need(const std::string& scope) const {
        if (!scopes.count(scope))
            throw ApiError(403, "insufficient_scope",
                           "This API key does not have the '" + scope + "' permission.");
    }
    json context() const {
        json g = json::array(), c = json::array();
        for (int x : groupIds) g.push_back(x);
        for (int x : allowedCompanyIds) c.push_back(x);
        return {{"uid", uid}, {"company_id", companyId}, {"partner_id", partnerId},
                {"is_admin", isAdmin}, {"group_ids", g}, {"allowed_company_ids", c},
                {"api_key_id", keyId}};
    }
    UserContext userContext() const {
        UserContext u;
        u.uid = uid; u.companyId = companyId; u.partnerId = partnerId; u.isAdmin = isAdmin;
        u.groupIds = groupIds; u.allowedCompanyIds = allowedCompanyIds;
        return u;
    }
};

/// A fixed one-minute window per key. Generous for a person's script, and it
/// bounds what a leaked key can do before someone notices and revokes it.
class RateLimiter {
public:
    bool allow(int keyId, int perMinute = 300) {
        std::lock_guard<std::mutex> lock(m_);
        const auto now = std::chrono::steady_clock::now();
        auto& w = windows_[keyId];
        if (now - w.first > std::chrono::minutes(1)) { w.first = now; w.second = 0; }
        return ++w.second <= perMinute;
    }
private:
    std::mutex m_;
    std::unordered_map<int, std::pair<std::chrono::steady_clock::time_point, int>> windows_;
};

namespace {

const char* kPriorityNames[] = {"low", "normal", "high", "urgent"};   // -1..2
std::string priorityName(int p) { return (p >= -1 && p <= 2) ? kPriorityNames[p + 1] : "normal"; }
int priorityValue(const json& v) {
    if (v.is_number_integer()) {
        const int p = v.get<int>();
        if (p >= -1 && p <= 2) return p;
    } else if (v.is_string()) {
        std::string s = v.get<std::string>();
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (int i = 0; i < 4; ++i) if (s == kPriorityNames[i]) return i - 1;
    }
    throw ApiError(400, "invalid", "priority must be low, normal, high or urgent.");
}
bool validDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9}) if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}
std::string clientIp(const drogon::HttpRequestPtr& req) {
    // Cloudflare / nginx put the caller here; the peer is the proxy.
    for (const char* h : {"cf-connecting-ip", "x-real-ip", "x-forwarded-for"}) {
        std::string v = req->getHeader(h);
        if (!v.empty()) { if (auto c = v.find(','); c != std::string::npos) v = v.substr(0, c); return v; }
    }
    return req->getPeerAddr().toIp();
}

} // namespace

// ================================================================
// ApiModule
// ================================================================
ApiModule::ApiModule(core::ModelFactory& models, core::ServiceFactory& services,
                     core::ViewModelFactory& viewModels, core::ViewFactory& views)
    : models_(models), services_(services), viewModels_(viewModels), views_(views) {}

std::string              ApiModule::moduleName()   const { return "api"; }
std::string              ApiModule::version()      const { return "1.0"; }
std::vector<std::string> ApiModule::dependencies() const { return {"auth", "ir", "project"}; }

void ApiModule::registerModels()   {}
void ApiModule::registerServices() {}
void ApiModule::registerViews()    {}

void ApiModule::registerViewModels() {
    auto db = services_.db();
    viewModels_.registerCreator("api.key", [db]{ return std::make_shared<ApiKeyViewModel>(db); });
}

void ApiModule::initialize() { seedMenus_(); }

void ApiModule::registerMigrations(cerp::infrastructure::MigrationRunner& runner) {
    // --------------------------------------------------------
    // 1200 — API keys
    //
    // Only the SHA-256 of a token is stored; the token itself exists once, in
    // the create response. token_prefix ("cerp_1a2b3c4d") is what a person sees
    // to tell keys apart. project_ids empty = every project the owner can see.
    // --------------------------------------------------------
    runner.registerMigration({1200, "api_keys", R"SQL(
        CREATE TABLE IF NOT EXISTS res_users_apikey (
            id            SERIAL PRIMARY KEY,
            user_id       INTEGER NOT NULL REFERENCES res_users(id) ON DELETE CASCADE,
            name          VARCHAR NOT NULL,
            token_prefix  VARCHAR NOT NULL,
            token_hash    CHAR(64) NOT NULL,
            scopes        TEXT[]    NOT NULL DEFAULT '{}',
            project_ids   INTEGER[] NOT NULL DEFAULT '{}',
            expires_at    TIMESTAMPTZ,
            revoked_at    TIMESTAMPTZ,
            last_used_at  TIMESTAMPTZ,
            last_used_ip  VARCHAR,
            use_count     BIGINT NOT NULL DEFAULT 0,
            created_by    INTEGER REFERENCES res_users(id) ON DELETE SET NULL,
            create_date   TIMESTAMPTZ NOT NULL DEFAULT now()
        );
        CREATE UNIQUE INDEX IF NOT EXISTS res_users_apikey_hash_uniq ON res_users_apikey (token_hash);
        CREATE INDEX IF NOT EXISTS res_users_apikey_user_idx ON res_users_apikey (user_id);
    )SQL"});
}

void ApiModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, path, context) VALUES
            (129, 'API Keys', 'api.keys', 'list', 'api-keys', '{}')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
                view_mode=EXCLUDED.view_mode, path=EXCLUDED.path, domain=NULL
    )");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (87, 'API Keys', 413, 60, 129)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");
    txn.commit();
}

// ----------------------------------------------------------------
// registerRoutes — /api/v1/*
// ----------------------------------------------------------------
void ApiModule::registerRoutes() {
    auto db = services_.db();
    const bool devMode = services_.devMode();
    auto* vms = &viewModels_;
    auto limiter = std::make_shared<RateLimiter>();

    // ---- authentication -------------------------------------------------
    auto authenticate = [db, limiter](const drogon::HttpRequestPtr& req) -> ApiUser {
        std::string h = req->getHeader("authorization");
        if (h.size() < 8 || !(h.compare(0, 7, "Bearer ") == 0 || h.compare(0, 7, "bearer ") == 0))
            throw ApiError(401, "unauthorized", "Send the API key as 'Authorization: Bearer cerp_…'.");
        const std::string token = h.substr(7);
        if (token.rfind("cerp_", 0) != 0 || token.size() != 53)
            throw ApiError(401, "unauthorized", "That is not a c-erp API key.");

        auto conn = db->acquire();
        pqxx::work txn{conn.get()};
        // Every column is aliased distinctly. k.name and the owner's name were
        // both "name": pqxx returns the FIRST match, so /api/v1/me reported
        // the KEY's name as the person's.
        auto r = txn.exec(
            "SELECT k.id AS key_id, k.name AS key_name, k.user_id, "
            "       array_to_json(k.scopes)::text AS scopes, "
            "       array_to_json(k.project_ids)::text AS projects, "
            "       (k.revoked_at IS NOT NULL) AS revoked, "
            "       (k.expires_at IS NOT NULL AND k.expires_at < now()) AS expired, "
            "       u.active, u.login, COALESCE(u.company_id, 0) AS company_id, "
            "       COALESCE(u.partner_id, 0) AS partner_id, "
            "       COALESCE(NULLIF(p.name,''), u.login) AS user_name "
            "FROM res_users_apikey k JOIN res_users u ON u.id = k.user_id "
            "LEFT JOIN res_partner p ON p.id = u.partner_id WHERE k.token_hash = $1",
            pqxx::params{core::Filestore::sha256Hex(token)});
        if (r.empty())                        throw ApiError(401, "unauthorized", "Unknown API key.");
        if (r[0]["revoked"].as<bool>())       throw ApiError(401, "revoked", "This API key has been revoked.");
        if (r[0]["expired"].as<bool>())       throw ApiError(401, "expired", "This API key has expired.");
        if (!r[0]["active"].as<bool>(false))  throw ApiError(401, "unauthorized", "The key's owner is deactivated.");

        ApiUser u;
        u.keyId = r[0]["key_id"].as<int>();
        u.keyName = r[0]["key_name"].c_str();
        u.uid = r[0]["user_id"].as<int>();
        u.login = r[0]["login"].c_str();
        u.name = r[0]["user_name"].c_str();
        u.companyId = r[0]["company_id"].as<int>();
        u.partnerId = r[0]["partner_id"].as<int>();
        for (const auto& s : json::parse(r[0]["scopes"].c_str(), nullptr, false))
            if (s.is_string()) u.scopes.insert(s.get<std::string>());
        for (const auto& p : json::parse(r[0]["projects"].c_str(), nullptr, false))
            if (p.is_number_integer()) u.projectIds.push_back(p.get<int>());
        for (const auto& g : txn.exec("SELECT gid FROM res_groups_users_rel WHERE uid = $1", pqxx::params{u.uid}))
            u.groupIds.push_back(g[0].as<int>());
        u.isAdmin = std::find(u.groupIds.begin(), u.groupIds.end(), 3) != u.groupIds.end();
        if (!u.isAdmin && std::find(u.groupIds.begin(), u.groupIds.end(), 2) == u.groupIds.end())
            throw ApiError(403, "forbidden", "The key's owner is not an internal user.");
        // The companies the owner may act for — the same rule a login applies
        // (docs/094): their memberships, else the company on their record.
        for (const auto& c : txn.exec(
                "SELECT c.id FROM res_company_users_rel rel JOIN res_company c ON c.id = rel.company_id "
                "WHERE rel.user_id = $1 AND c.active ORDER BY c.id", pqxx::params{u.uid}))
            u.allowedCompanyIds.push_back(c[0].as<int>());
        if (u.allowedCompanyIds.empty() && u.companyId > 0) u.allowedCompanyIds.push_back(u.companyId);
        if (!u.allowedCompanyIds.empty() &&
            std::find(u.allowedCompanyIds.begin(), u.allowedCompanyIds.end(), u.companyId) == u.allowedCompanyIds.end())
            u.companyId = u.allowedCompanyIds.front();

        if (!limiter->allow(u.keyId))
            throw ApiError(429, "rate_limited", "Too many requests for this key; slow down (300 per minute).");
        txn.exec("UPDATE res_users_apikey SET last_used_at = now(), last_used_ip = $2, "
                 "use_count = use_count + 1 WHERE id = $1",
                 pqxx::params{u.keyId, clientIp(req)});
        txn.commit();
        return u;
    };

    // ---- calling the same view models the screens use --------------------
    auto callVm = [vms](const ApiUser& u, const std::string& model, const std::string& method,
                        json args, json kwargs = json::object()) -> json {
        core::CallKwArgs c;
        c.model = model; c.method = method; c.args = std::move(args);
        c.kwargs = kwargs.is_object() ? std::move(kwargs) : json::object();
        c.kwargs["context"] = u.context();
        core::CurrentUser::Scope scope(u.userContext());
        auto vm = vms->create(model, core::Lifetime::Transient);
        return vm->callKw(c);
    };

    // A ticket by key, if this key may see it. Out-of-scope and nonexistent
    // answer the same 404, so a key cannot probe for other projects' tickets.
    auto resolveTicket = [db](const ApiUser& u, const std::string& key) -> std::pair<int, int> {
        auto conn = db->acquire();
        pqxx::work txn{conn.get()};
        auto r = txn.exec("SELECT id, project_id FROM project_task WHERE upper(key) = upper($1)",
                          pqxx::params{key});
        if (r.empty() || !u.mayProject(r[0][1].as<int>()))
            throw ApiError(404, "not_found", "No ticket " + key + ".");
        return {r[0][0].as<int>(), r[0][1].as<int>()};
    };
    auto userIdFor = [db](const json& v, const char* field) -> json {
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) return false;
        auto conn = db->acquire();
        pqxx::work txn{conn.get()};
        pqxx::result r = v.is_number_integer()
            ? txn.exec("SELECT id FROM res_users WHERE id = $1 AND active", pqxx::params{v.get<int>()})
            : txn.exec("SELECT u.id FROM res_users u LEFT JOIN res_partner p ON p.id = u.partner_id "
                       "WHERE u.active AND (lower(u.login) = lower($1) OR lower(p.email) = lower($1)) "
                       "ORDER BY u.id LIMIT 1", pqxx::params{v.is_string() ? v.get<std::string>() : std::string{}});
        if (r.empty()) throw ApiError(400, "invalid", std::string(field) + ": no active user " + v.dump() + ".");
        return r[0][0].as<int>();
    };
    auto projectFor = [db](const ApiUser& u, const std::string& prefix) -> int {
        auto conn = db->acquire();
        pqxx::work txn{conn.get()};
        auto r = txn.exec("SELECT id FROM project_project WHERE upper(task_prefix) = upper($1) AND active",
                          pqxx::params{prefix});
        if (r.empty() || !u.mayProject(r[0][0].as<int>()))
            throw ApiError(404, "not_found", "No project " + prefix + ".");
        return r[0][0].as<int>();
    };

    // ---- shaping the answers --------------------------------------------
    auto ticketJson = [](const json& d) -> json {
        std::string status;
        for (const auto& s : d.value("stages", json::array()))
            if (s.value("id", 0) == d.value("stage_id", 0)) status = s.value("name", "");
        auto person = [&](const char* id, const char* name) -> json {
            return d.value(id, 0) > 0 ? json{{"id", d.value(id, 0)}, {"name", d.value(name, "")}} : json(nullptr);
        };
        json labels = json::array(), subtasks = json::array(), watchers = json::array();
        for (const auto& t : d.value("tags", json::array())) labels.push_back(t.value("name", ""));
        for (const auto& s : d.value("subtasks", json::array()))
            subtasks.push_back({{"key", s.value("key", "")}, {"title", s.value("name", "")},
                                {"status", s.value("stage", "")}, {"closed", s.value("closed", false)},
                                {"type", s.value("issue_type", "task")}});
        for (const auto& w : d.value("watchers", json::array()))
            watchers.push_back({{"id", w.value("id", 0)}, {"name", w.value("name", "")},
                                {"login", w.value("login", "")}});
        const std::string key = d.value("key", "");
        return {
            {"id", d.value("id", 0)}, {"key", key}, {"url", "/api/v1/tickets/" + key},
            {"title", d.value("name", "")}, {"description", d.value("description", "")},
            {"type", d.value("issue_type", "task")},
            {"priority", priorityName(d.value("priority", 0))}, {"priority_value", d.value("priority", 0)},
            {"status", status}, {"closed", d.value("closed", false)},
            {"blocked", d.value("kanban_state", "") == "blocked"},
            {"project", d.value("project_prefix", "")}, {"project_name", d.value("project_name", "")},
            {"assignee", person("user_id", "user_name")}, {"reporter", person("reporter_id", "reporter_name")},
            {"parent", d.value("parent_key", "").empty() ? json(nullptr) : json(d.value("parent_key", ""))},
            {"labels", labels}, {"watchers", watchers}, {"subtasks", subtasks},
            {"statuses", [&] { json a = json::array();
                               for (const auto& s : d.value("stages", json::array())) a.push_back(s.value("name", ""));
                               return a; }()},
            {"due", d.value("date_deadline", "")}, {"estimate_hours", d.value("planned_hours", 0.0)},
            {"logged_hours", d.value("logged_hours", 0.0)},
            {"comment_count", d.value("comment_count", 0)}, {"attachment_count", d.value("attachment_count", 0)},
            {"created_at", d.value("create_date", "")}, {"updated_at", d.value("write_date", "")},
            {"closed_on", d.value("date_end", "")}};
    };
    auto activityJson = [](const json& m) -> json {
        const std::string sub = m.value("subtype", "");
        return {{"id", m.value("id", 0)},
                {"kind", sub == "comment" ? "comment" : "history"},
                {"author", {{"id", m.value("author_id", 0)}, {"name", m.value("author_name", "")}}},
                {"body", m.value("body", "")}, {"created_at", m.value("date", "")},
                {"edited", m.value("edited", false)}};
    };
    auto detail = [callVm, ticketJson](const ApiUser& u, int id) -> json {
        return ticketJson(callVm(u, "project.task", "task_detail", json::array({{{"id", id}}})));
    };

    // ---- the envelope every route shares --------------------------------
    using Cb = std::function<void(const drogon::HttpResponsePtr&)>;
    auto respond = [](const Cb& cb, int status, const json& body) {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
        r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        r->addHeader("Cache-Control", "no-store");
        if (status == 401) r->addHeader("WWW-Authenticate", "Bearer");
        r->setBody(body.dump());
        cb(r);
    };
    // Runs a route: authenticate, then fn, and turn every failure into the
    // right status with a message the caller can act on (SEC-28: internals
    // only in dev mode).
    auto serve = [authenticate, respond, devMode](const drogon::HttpRequestPtr& req, Cb cb,
                                                  const std::function<std::pair<int, json>(const ApiUser&)>& fn) {
        try {
            const ApiUser u = authenticate(req);
            const auto [status, body] = fn(u);
            respond(cb, status, body);
        } catch (const ApiError& e) {
            respond(cb, e.status, {{"error", e.code}, {"message", e.what()}});
        } catch (const cerp::modules::ir::UploadRejected& e) {
            respond(cb, e.status, {{"error", "invalid"}, {"message", e.what()}});
        } catch (const AccessDeniedError& e) {
            respond(cb, 403, {{"error", "forbidden"}, {"message", e.what()}});
        } catch (const ValidationError& e) {
            const std::string m = e.what();
            const bool missing = m.rfind("No such", 0) == 0;
            respond(cb, missing ? 404 : 400, {{"error", missing ? "not_found" : "invalid"}, {"message", m}});
        } catch (const PoolExhaustedException& e) {
            LOG_ERROR << "[api] pool: " << e.what();
            respond(cb, 503, {{"error", "unavailable"}, {"message", "The server is busy. Retry shortly."}});
        } catch (const std::exception& e) {
            LOG_ERROR << "[api] " << req->path() << ": " << e.what();
            respond(cb, 500, {{"error", "internal"},
                              {"message", devMode ? e.what() : "An internal error occurred"}});
        }
    };
    auto body = [](const drogon::HttpRequestPtr& req) -> json {
        const std::string b(req->body());
        if (b.empty()) return json::object();
        json j = json::parse(b, nullptr, false);
        if (j.is_discarded() || !j.is_object()) throw ApiError(400, "invalid", "The body must be a JSON object.");
        return j;
    };
    auto qint = [](const drogon::HttpRequestPtr& req, const char* k, int def) {
        const std::string s = req->getParameter(k);
        if (s.empty()) return def;
        try { return std::stoi(s); } catch (...) { throw ApiError(400, "invalid", std::string(k) + " must be a number."); }
    };
    auto method = [](const drogon::HttpRequestPtr& req) { return req->method(); };

    /// Ticket fields from a JSON body into project.task write values.
    auto toVals = [userIdFor, resolveTicket, projectFor](const ApiUser& u, const json& b,
                                                         const json& stages, bool creating) -> json {
        static const std::set<std::string> kFields = {
            "title", "description", "type", "priority", "status", "assignee", "reporter",
            "due", "estimate_hours", "blocked", "parent", "project", "labels"};
        for (auto it = b.begin(); it != b.end(); ++it)
            if (!kFields.count(it.key()))
                throw ApiError(400, "invalid", "Unknown field '" + it.key() + "'. Fields: title, description, "
                               "type, priority, status, assignee, reporter, due, estimate_hours, blocked, "
                               "parent, project, labels.");
        json v = json::object();
        if (b.contains("title")) {
            if (!b["title"].is_string() || b["title"].get<std::string>().find_first_not_of(" \t") == std::string::npos)
                throw ApiError(400, "invalid", "title must be a non-empty string.");
            v["name"] = b["title"];
        }
        if (b.contains("description")) {
            if (!b["description"].is_string() && !b["description"].is_null())
                throw ApiError(400, "invalid", "description must be a string.");
            v["description"] = b["description"].is_null() ? "" : b["description"].get<std::string>();
        }
        if (b.contains("type")) {
            if (!b["type"].is_string()) throw ApiError(400, "invalid", "type must be task, bug, feature or chore.");
            v["issue_type"] = b["type"];
        }
        if (b.contains("priority")) v["priority"] = priorityValue(b["priority"]);
        if (b.contains("status")) {
            if (!b["status"].is_string()) throw ApiError(400, "invalid", "status must be a status name.");
            std::string want = b["status"].get<std::string>(), names;
            for (auto& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            int found = 0;
            for (const auto& s : stages) {
                std::string n = s.value("name", "");
                if (!names.empty()) names += ", ";
                names += n;
                for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (n == want) found = s.value("id", 0);
            }
            if (!found) throw ApiError(400, "invalid", "Unknown status. Statuses: " + names + ".");
            v["stage_id"] = found;
        }
        if (b.contains("assignee")) v["user_id"]     = userIdFor(b["assignee"], "assignee");
        if (b.contains("reporter")) v["reporter_id"] = userIdFor(b["reporter"], "reporter");
        if (b.contains("due")) {
            if (b["due"].is_null() || (b["due"].is_string() && b["due"].get<std::string>().empty()))
                v["date_deadline"] = false;
            else if (b["due"].is_string() && validDate(b["due"].get<std::string>()))
                v["date_deadline"] = b["due"];
            else throw ApiError(400, "invalid", "due must be YYYY-MM-DD or null.");
        }
        if (b.contains("estimate_hours")) {
            if (!b["estimate_hours"].is_number() || b["estimate_hours"].get<double>() < 0)
                throw ApiError(400, "invalid", "estimate_hours must be a number, 0 or more.");
            v["planned_hours"] = b["estimate_hours"];
        }
        if (b.contains("blocked")) {
            if (!b["blocked"].is_boolean()) throw ApiError(400, "invalid", "blocked must be true or false.");
            v["kanban_state"] = b["blocked"].get<bool>() ? "blocked" : "normal";
        }
        if (b.contains("parent")) {
            if (b["parent"].is_null() || (b["parent"].is_string() && b["parent"].get<std::string>().empty()))
                v["parent_id"] = false;
            else if (b["parent"].is_string()) v["parent_id"] = resolveTicket(u, b["parent"].get<std::string>()).first;
            else throw ApiError(400, "invalid", "parent must be a ticket key or null.");
        }
        if (b.contains("project")) {
            if (!b["project"].is_string()) throw ApiError(400, "invalid", "project must be a project key, e.g. CERP.");
            v["project_id"] = projectFor(u, b["project"].get<std::string>());
        } else if (creating) {
            throw ApiError(400, "invalid", "project is required, e.g. \"project\": \"CERP\".");
        }
        return v;
    };
    auto labelsOf = [](const json& b) -> std::vector<std::string> {
        std::vector<std::string> out;
        if (!b.contains("labels")) return out;
        if (!b["labels"].is_array()) throw ApiError(400, "invalid", "labels must be an array of names.");
        for (const auto& l : b["labels"]) {
            if (!l.is_string()) throw ApiError(400, "invalid", "labels must be an array of names.");
            out.push_back(l.get<std::string>());
        }
        return out;
    };

    // ================================================================
    // Routes
    // ================================================================
    // GET /api/v1/me — who this key acts as, and what it may do
    drogon::app().registerHandler("/api/v1/me",
        [serve](const drogon::HttpRequestPtr& req, Cb&& cb) {
            serve(req, std::move(cb), [](const ApiUser& u) -> std::pair<int, json> {
                return {200, {{"user", {{"id", u.uid}, {"login", u.login}, {"name", u.name}}},
                              {"key", {{"id", u.keyId}, {"name", u.keyName}}},
                              {"scopes", std::vector<std::string>(u.scopes.begin(), u.scopes.end())},
                              {"project_ids", u.projectIds}}};
            });
        }, {drogon::Get});

    // GET /api/v1/projects   POST /api/v1/projects (create)
    //
    // Creating a project is part of running the tracker — the first ticket
    // needs somewhere to live — so it sits under tickets:write rather than a
    // scope of its own. A key limited to certain projects cannot create more.
    drogon::app().registerHandler("/api/v1/projects",
        [serve, callVm, body, method, db](const drogon::HttpRequestPtr& req, Cb&& cb) {
            serve(req, std::move(cb), [&](const ApiUser& u) -> std::pair<int, json> {
                if (method(req) == drogon::Post) {
                    u.need("tickets:write");
                    if (!u.projectIds.empty())
                        throw ApiError(403, "forbidden",
                                       "This API key is limited to certain projects, so it cannot create one.");
                    const json b = body(req);
                    for (auto it = b.begin(); it != b.end(); ++it)
                        if (it.key() != "name" && it.key() != "key" && it.key() != "description")
                            throw ApiError(400, "invalid",
                                           "Unknown field '" + it.key() + "'. Fields: name, key, description.");
                    if (!b.contains("name") || !b["name"].is_string() || b["name"].get<std::string>().empty())
                        throw ApiError(400, "invalid", "name is required.");
                    json vals = {{"name", b["name"]}};
                    if (b.contains("key")) {
                        if (!b["key"].is_string())
                            throw ApiError(400, "invalid", "key must be 2-10 letters or digits, e.g. CERP.");
                        vals["task_prefix"] = b["key"];
                    }
                    if (b.contains("description")) vals["description"] = b["description"];
                    const int pid = callVm(u, "project.project", "create", json::array({vals})).get<int>();
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    auto r = txn.exec("SELECT name, task_prefix FROM project_project WHERE id = $1",
                                      pqxx::params{pid});
                    return {201, {{"key", r[0]["task_prefix"].c_str()}, {"name", r[0]["name"].c_str()},
                                  {"tickets", 0}}};
                }
                u.need("tickets:read");
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                json out = json::array();
                for (const auto& r : txn.exec(
                        "SELECT id, name, task_prefix, task_seq FROM project_project "
                        "WHERE active AND (company_id IS NULL OR company_id = ANY($1::int[])) ORDER BY task_prefix",
                        pqxx::params{pgIntArray(u.allowedCompanyIds)})) {
                    if (!u.mayProject(r["id"].as<int>())) continue;
                    out.push_back({{"key", r["task_prefix"].c_str()}, {"name", r["name"].c_str()},
                                   {"tickets", r["task_seq"].as<int>(0)}});
                }
                return {200, out};
            });
        }, {drogon::Get, drogon::Post});

    // GET /api/v1/projects/{key}/statuses — the board's columns, in order
    drogon::app().registerHandler("/api/v1/projects/{1}/statuses",
        [serve, db, projectFor](const drogon::HttpRequestPtr& req, Cb&& cb, const std::string& prefix) {
            serve(req, std::move(cb), [db, projectFor, prefix](const ApiUser& u) -> std::pair<int, json> {
                u.need("tickets:read");
                const int pid = projectFor(u, prefix);
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                json out = json::array();
                for (const auto& r : txn.exec(
                        "SELECT name, COALESCE(is_closed,false) AS closed FROM project_task_type "
                        "WHERE active AND (project_id IS NULL OR project_id = $1) ORDER BY sequence, id",
                        pqxx::params{pid}))
                    out.push_back({{"name", r["name"].c_str()}, {"closed", r["closed"].as<bool>()}});
                return {200, out};
            });
        }, {drogon::Get});

    // GET /api/v1/labels
    drogon::app().registerHandler("/api/v1/labels",
        [serve, db](const drogon::HttpRequestPtr& req, Cb&& cb) {
            serve(req, std::move(cb), [db](const ApiUser& u) -> std::pair<int, json> {
                u.need("tickets:read");
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                json out = json::array();
                for (const auto& r : txn.exec("SELECT name FROM project_tag WHERE active ORDER BY lower(name)"))
                    out.push_back(r[0].c_str());
                return {200, out};
            });
        }, {drogon::Get});

    // GET /api/v1/users?q= — who a ticket can be assigned to
    drogon::app().registerHandler("/api/v1/users",
        [serve, db](const drogon::HttpRequestPtr& req, Cb&& cb) {
            const std::string q = req->getParameter("q");
            serve(req, std::move(cb), [db, q](const ApiUser& u) -> std::pair<int, json> {
                u.need("tickets:read");
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                json out = json::array();
                for (const auto& r : txn.exec(
                        "SELECT u.id, u.login, COALESCE(NULLIF(p.name,''), u.login) AS name "
                        "FROM res_users u LEFT JOIN res_partner p ON p.id = u.partner_id "
                        "WHERE u.active AND EXISTS (SELECT 1 FROM res_groups_users_rel g "
                        "                           WHERE g.uid = u.id AND g.gid IN (2, 3)) "
                        "  AND ($1 = '' OR u.login ILIKE '%' || $1 || '%' OR p.name ILIKE '%' || $1 || '%') "
                        "ORDER BY 3 LIMIT 50", pqxx::params{q}))
                    out.push_back({{"id", r["id"].as<int>()}, {"login", r["login"].c_str()},
                                   {"name", r["name"].c_str()}});
                return {200, out};
            });
        }, {drogon::Get});

    // GET /api/v1/tickets  (search)   POST /api/v1/tickets  (create)
    drogon::app().registerHandler("/api/v1/tickets",
        [serve, callVm, detail, toVals, labelsOf, body, qint, method, projectFor, userIdFor, db]
        (const drogon::HttpRequestPtr& req, Cb&& cb) {
            serve(req, std::move(cb), [&](const ApiUser& u) -> std::pair<int, json> {
                if (method(req) == drogon::Get) {
                    u.need("tickets:read");
                    json f = {{"limit", std::clamp(qint(req, "limit", 50), 1, 200)},
                              {"offset", std::max(0, qint(req, "offset", 0))},
                              {"state", req->getParameter("state").empty() ? std::string("open")
                                                                           : req->getParameter("state")}};
                    const std::string st = f["state"];
                    if (st != "open" && st != "closed" && st != "all")
                        throw ApiError(400, "invalid", "state must be open, closed or all.");
                    if (const auto p = req->getParameter("project"); !p.empty())
                        f["project_ids"] = json::array({projectFor(u, p)});
                    else if (!u.projectIds.empty())
                        f["project_ids"] = u.projectIds;
                    for (const char* k : {"type", "status", "label", "q"})
                        if (const auto val = req->getParameter(k); !val.empty())
                            f[std::string(k) == "type" ? "issue_type" : k] = val;
                    if (const auto a = req->getParameter("assignee"); !a.empty()) {
                        if (a == "none") f["assignee_id"] = -1;
                        else if (a == "me") f["assignee_id"] = u.uid;
                        else f["assignee_id"] = userIdFor(json(a), "assignee").get<int>();
                    }
                    json r = callVm(u, "project.task", "search_tickets", json::array({f}));
                    for (auto& t : r["tickets"]) {
                        t["priority"] = priorityName(t.value("priority_value", 0));
                        t["url"] = "/api/v1/tickets/" + t.value("key", std::string{});
                    }
                    r["limit"] = f["limit"]; r["offset"] = f["offset"];
                    return {200, r};
                }
                // POST — create
                u.need("tickets:write");
                const json b = body(req);
                // status is resolved after the ticket exists, against ITS
                // project's columns; everything else is validated now.
                json first = b;
                first.erase("status");
                json vals = toVals(u, first, json::array(), true);
                if (!vals.contains("name")) throw ApiError(400, "invalid", "title is required.");
                const json statusWanted = b.contains("status") ? b["status"] : json(nullptr);
                const int id = callVm(u, "project.task", "create", json::array({vals})).get<int>();
                const auto labels = labelsOf(b);
                if (!labels.empty())
                    callVm(u, "project.task", "set_tags", json::array({{{"task_id", id}, {"tags", labels}}}));
                if (!statusWanted.is_null()) {
                    const json d = callVm(u, "project.task", "task_detail", json::array({{{"id", id}}}));
                    const json sv = toVals(u, {{"status", statusWanted}}, d["stages"], false);
                    callVm(u, "project.task", "write", json::array({json::array({id}), sv}));
                }
                (void)db;
                return {201, detail(u, id)};
            });
        }, {drogon::Get, drogon::Post});

    // GET / PATCH /api/v1/tickets/{key}
    drogon::app().registerHandler("/api/v1/tickets/{1}",
        [serve, callVm, detail, toVals, labelsOf, body, method, resolveTicket]
        (const drogon::HttpRequestPtr& req, Cb&& cb, const std::string& key) {
            serve(req, std::move(cb), [&](const ApiUser& u) -> std::pair<int, json> {
                const int id = resolveTicket(u, key).first;
                if (method(req) == drogon::Get) { u.need("tickets:read"); return {200, detail(u, id)}; }
                u.need("tickets:write");
                const json b = body(req);
                const json d = callVm(u, "project.task", "task_detail", json::array({{{"id", id}}}));
                json vals = toVals(u, b, d["stages"], false);
                if (!vals.empty())
                    callVm(u, "project.task", "write", json::array({json::array({id}), vals}));
                if (b.contains("labels"))
                    callVm(u, "project.task", "set_tags", json::array({{{"task_id", id}, {"tags", labelsOf(b)}}}));
                return {200, detail(u, id)};
            });
        }, {drogon::Get, drogon::Patch});

    // GET /api/v1/tickets/{key}/activity — comments and history, oldest first
    drogon::app().registerHandler("/api/v1/tickets/{1}/activity",
        [serve, callVm, activityJson, resolveTicket]
        (const drogon::HttpRequestPtr& req, Cb&& cb, const std::string& key) {
            serve(req, std::move(cb), [&](const ApiUser& u) -> std::pair<int, json> {
                u.need("tickets:read");
                const int id = resolveTicket(u, key).first;
                json out = json::array();
                for (const auto& m : callVm(u, "project.task", "activity", json::array({{{"task_id", id}}})))
                    out.push_back(activityJson(m));
                return {200, out};
            });
        }, {drogon::Get});

    // GET / POST /api/v1/tickets/{key}/comments
    drogon::app().registerHandler("/api/v1/tickets/{1}/comments",
        [serve, callVm, activityJson, resolveTicket, body, method]
        (const drogon::HttpRequestPtr& req, Cb&& cb, const std::string& key) {
            serve(req, std::move(cb), [&](const ApiUser& u) -> std::pair<int, json> {
                const int id = resolveTicket(u, key).first;
                if (method(req) == drogon::Get) {
                    u.need("tickets:read");
                    json out = json::array();
                    for (const auto& m : callVm(u, "project.task", "activity", json::array({{{"task_id", id}}})))
                        if (m.value("subtype", "") == "comment") out.push_back(activityJson(m));
                    return {200, out};
                }
                u.need("comments:write");
                const json b = body(req);
                if (!b.contains("body") || !b["body"].is_string())
                    throw ApiError(400, "invalid", "body (the comment text) is required.");
                const int mid = callVm(u, "project.task", "post_comment",
                                       json::array({{{"task_id", id}, {"body", b["body"]}}})).get<int>();
                for (const auto& m : callVm(u, "project.task", "activity", json::array({{{"task_id", id}}})))
                    if (m.value("id", 0) == mid) return {201, activityJson(m)};
                return {201, {{"id", mid}}};
            });
        }, {drogon::Get, drogon::Post});

    // PATCH / DELETE /api/v1/comments/{id}
    drogon::app().registerHandler("/api/v1/comments/{1}",
        [serve, callVm, body, method, db](const drogon::HttpRequestPtr& req, Cb&& cb, const std::string& idStr) {
            serve(req, std::move(cb), [&](const ApiUser& u) -> std::pair<int, json> {
                u.need("comments:write");
                int mid = 0;
                try { mid = std::stoi(idStr); } catch (...) {}
                {   // the comment's ticket must be inside this key's projects
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    auto r = txn.exec("SELECT t.project_id FROM mail_message m JOIN project_task t ON t.id = m.res_id "
                                      "WHERE m.id = $1 AND m.res_model = 'project.task' AND m.subtype = 'comment'",
                                      pqxx::params{mid});
                    if (r.empty() || !u.mayProject(r[0][0].as<int>()))
                        throw ApiError(404, "not_found", "No comment " + idStr + ".");
                }
                if (method(req) == drogon::Delete) {
                    callVm(u, "project.task", "delete_comment", json::array({{{"message_id", mid}}}));
                    return {200, {{"deleted", mid}}};
                }
                const json b = body(req);
                if (!b.contains("body") || !b["body"].is_string())
                    throw ApiError(400, "invalid", "body (the new comment text) is required.");
                callVm(u, "project.task", "edit_comment", json::array({{{"message_id", mid}, {"body", b["body"]}}}));
                return {200, {{"id", mid}, {"edited", true}}};
            });
        }, {drogon::Patch, drogon::Delete});

    // GET / POST /api/v1/tickets/{key}/attachments
    drogon::app().registerHandler("/api/v1/tickets/{1}/attachments",
        [serve, callVm, resolveTicket, method, db](const drogon::HttpRequestPtr& req, Cb&& cb, const std::string& key) {
            serve(req, std::move(cb), [&](const ApiUser& u) -> std::pair<int, json> {
                const int id = resolveTicket(u, key).first;
                // Visibility through the ORM (record rules, company) before any SQL.
                callVm(u, "project.task", "task_detail", json::array({{{"id", id}}}));
                auto fileJson = [](int aid, const std::string& name, const std::string& mime, long long size,
                                   const std::string& created) -> json {
                    return {{"id", aid}, {"name", name}, {"mimetype", mime}, {"size", size},
                            {"created_at", created}, {"download_url", "/api/v1/attachments/" + std::to_string(aid)},
                            {"markdown", "![" + name + "](/web/content/" + std::to_string(aid) + ")"}};
                };
                if (method(req) == drogon::Get) {
                    u.need("tickets:read");
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    json out = json::array();
                    for (const auto& r : txn.exec(
                            "SELECT id, name, COALESCE(mimetype,'') AS mimetype, COALESCE(file_size,0) AS size, "
                            "       to_char(create_date,'YYYY-MM-DD\"T\"HH24:MI:SS') AS created "
                            "FROM ir_attachment WHERE res_model = 'project.task' AND res_id = $1 ORDER BY id",
                            pqxx::params{id}))
                        out.push_back(fileJson(r["id"].as<int>(), r["name"].c_str(), r["mimetype"].c_str(),
                                               r["size"].as<long long>(0),
                                               r["created"].is_null() ? "" : r["created"].c_str()));
                    return {200, out};
                }
                u.need("attachments:write");
                drogon::MultiPartParser parser;
                if (parser.parse(req) != 0 || parser.getFiles().empty())
                    throw ApiError(400, "invalid", "Send the file as multipart/form-data, field 'file'.");
                const auto& file = parser.getFiles()[0];
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                const auto stored = cerp::modules::ir::storeAttachment(
                    txn, std::string(file.fileContent()), file.getFileName(),
                    parser.getParameter<std::string>("name"), "", "project.task", id, u.uid, "");
                txn.exec("UPDATE project_task SET write_date = now() WHERE id = $1", pqxx::params{id});
                txn.commit();
                return {201, fileJson(stored.id, stored.name, stored.mimetype, stored.size, "")};
            });
        }, {drogon::Get, drogon::Post});

    // GET (download) / DELETE /api/v1/attachments/{id}
    drogon::app().registerHandler("/api/v1/attachments/{1}",
        [authenticate, respond, serve, callVm, method, db, devMode]
        (const drogon::HttpRequestPtr& req, Cb&& cb, const std::string& idStr) {
            int aid = 0;
            try { aid = std::stoi(idStr); } catch (...) {}
            // The attachment's ticket, if this key may see it.
            auto ticketOf = [&](const ApiUser& u) -> pqxx::row {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                auto r = txn.exec("SELECT a.res_id, t.project_id, a.name, COALESCE(a.mimetype,'') AS mimetype, "
                                  "       a.store_fname FROM ir_attachment a JOIN project_task t ON t.id = a.res_id "
                                  "WHERE a.id = $1 AND a.res_model = 'project.task'", pqxx::params{aid});
                if (r.empty() || !u.mayProject(r[0]["project_id"].as<int>()))
                    throw ApiError(404, "not_found", "No attachment " + idStr + ".");
                callVm(u, "project.task", "task_detail", json::array({{{"id", r[0]["res_id"].as<int>()}}}));
                return r[0];
            };
            if (method(req) == drogon::Delete) {
                serve(req, std::move(cb), [&](const ApiUser& u) -> std::pair<int, json> {
                    u.need("attachments:write");
                    ticketOf(u);
                    callVm(u, "ir.attachment", "unlink", json::array({json::array({aid})}));
                    return {200, {{"deleted", aid}}};
                });
                return;
            }
            // Download streams bytes, so it cannot go through `serve`'s JSON reply.
            try {
                const ApiUser u = authenticate(req);
                u.need("tickets:read");
                const auto row = ticketOf(u);
                std::string bytes = core::Filestore::get(row["store_fname"].c_str());
                std::string name = row["name"].c_str();
                for (auto& c : name) if (c == '"' || c == '\\' || c == '\r' || c == '\n') c = '_';
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k200OK);
                r->setContentTypeString(row["mimetype"].c_str()[0] ? row["mimetype"].c_str()
                                                                   : "application/octet-stream");
                // Always a download: an API client wants the bytes, and an SVG
                // must never be rendered from this origin.
                r->addHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
                r->addHeader("X-Content-Type-Options", "nosniff");
                r->addHeader("Cache-Control", "no-store");
                r->setBody(std::move(bytes));
                cb(r);
            } catch (const ApiError& e) {
                respond(cb, e.status, {{"error", e.code}, {"message", e.what()}});
            } catch (const ValidationError& e) {
                respond(cb, 404, {{"error", "not_found"}, {"message", e.what()}});
            } catch (const std::exception& e) {
                LOG_ERROR << "[api] download " << aid << ": " << e.what();
                respond(cb, 500, {{"error", "internal"},
                                  {"message", devMode ? e.what() : "An internal error occurred"}});
            }
        }, {drogon::Get, drogon::Delete});

    // PUT / DELETE /api/v1/tickets/{key}/watch
    drogon::app().registerHandler("/api/v1/tickets/{1}/watch",
        [serve, callVm, resolveTicket, method](const drogon::HttpRequestPtr& req, Cb&& cb, const std::string& key) {
            serve(req, std::move(cb), [&](const ApiUser& u) -> std::pair<int, json> {
                u.need("tickets:write");
                const int id = resolveTicket(u, key).first;
                const bool on = method(req) != drogon::Delete;
                callVm(u, "project.task", "watch", json::array({{{"task_id", id}, {"watch", on}}}));
                return {200, {{"key", key}, {"watching", on}}};
            });
        }, {drogon::Put, drogon::Delete});
}

} // namespace cerp::modules::api

// ============================================================
// tools/admin/erp_admin.cpp — ERP Admin Console (docs/073)
//
// A SEPARATE, self-contained admin tool for the IT operator — NOT the Odoo
// frontend, NOT exposed to the public site. It binds loopback only and is
// reached over an SSH tunnel; access is gated by a per-launch token printed to
// the console (so only whoever started it over SSH can use it).
//
// It manages: the c-erp service, PostgreSQL databases (list/backup/restore),
// multi-company tenants (create/link/toggle), nginx, and SSL — by shelling out
// to the system tools with an argv array (never a shell string) after
// allowlist-validating every identifier.
//
// Build:  cmake --build ./build --target erp-admin
// Run:    ./build/erp-admin            (prints the tunnel URL + token)
// ============================================================
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <climits>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// ───────────────────────── config ─────────────────────────
struct DbCfg { std::string host="localhost", port="5432", user="odoo", pass="", name="odoo"; };
static DbCfg     g_db;
static std::string g_token;
static std::string g_configPath = "config/system.cfg";
static std::string g_backupDir   = "backups";
static std::string g_appRoot     = ".";          // c-erp working dir (for provision + web)
static std::string g_nginxConf   = "/etc/nginx/sites-available/c-erp.conf";
static std::string g_serviceName = "c-erp";
static std::string g_logFile     = "log/system.log";

// ─────────────────────── small helpers ────────────────────
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static void loadConfig(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
        if      (k == "db_host")     g_db.host = v;
        else if (k == "db_port")     g_db.port = v;
        else if (k == "db_user")     g_db.user = v;
        else if (k == "db_password") g_db.pass = (v == "False" ? "" : v);
        else if (k == "db_name")     g_db.name = v;
        else if (k == "logfile" && !v.empty()) g_logFile = v;
    }
}

// A database/identifier name is safe iff it is [A-Za-z0-9_] and <= 63 chars.
static bool validName(const std::string& s) {
    if (s.empty() || s.size() > 63) return false;
    for (char c : s) if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}
// A hostname/domain: letters, digits, dot, hyphen.
static bool validDomain(const std::string& s) {
    if (s.empty() || s.size() > 253) return false;
    for (char c : s) if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-')) return false;
    return true;
}
// A backup filename must be a bare name (no path separators, no ..).
static bool validBackupFile(const std::string& s) {
    if (s.empty() || s.size() > 200) return false;
    if (s.find('/') != std::string::npos || s.find("..") != std::string::npos) return false;
    return true;
}

// ─────────────── run a command WITHOUT a shell ────────────
struct CmdResult { int code = -1; std::string out; };

static CmdResult runCmd(const std::vector<std::string>& args,
                        const std::vector<std::pair<std::string, std::string>>& env = {},
                        const std::string& cwd = "") {
    int pipefd[2];
    if (pipe(pipefd) != 0) return {-1, "pipe() failed"};
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return {-1, "fork() failed"}; }
    if (pid == 0) {                       // child
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        if (!cwd.empty()) { if (chdir(cwd.c_str()) != 0) { perror("chdir"); _exit(126); } }
        for (auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        fprintf(stderr, "exec '%s' failed: %s\n", args.empty() ? "?" : args[0].c_str(), strerror(errno));
        _exit(127);
    }
    close(pipefd[1]);                      // parent
    std::string out; char buf[8192]; ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(pipefd[0]);
    int status = 0; waitpid(pid, &status, 0);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, out};
}

// psql/pg_* args with the configured connection + PGPASSWORD env.
static std::vector<std::string> pgArgs(const std::string& bin, const std::string& db = "") {
    std::vector<std::string> a = {bin, "-h", g_db.host, "-p", g_db.port, "-U", g_db.user};
    if (!db.empty()) { a.push_back("-d"); a.push_back(db); }
    return a;
}
static std::vector<std::pair<std::string, std::string>> pgEnv() {
    return {{"PGPASSWORD", g_db.pass}, {"PGCONNECT_TIMEOUT", "6"}};
}

static bool amRoot() { return geteuid() == 0; }

// Run a SYSTEM command that may need root (nginx/certbot/systemctl/reading a
// protected file). Tries it directly first; if it fails and we're not root,
// retries via `sudo -n` (non-interactive). If sudo itself needs a password, a
// clear instruction is prepended so the operator knows exactly what to do.
static CmdResult runMaybePriv(const std::vector<std::string>& args,
                              const std::vector<std::pair<std::string, std::string>>& env = {}) {
    CmdResult r = runCmd(args, env);
    if (r.code == 0 || amRoot()) return r;
    // Only escalate on a genuine PRIVILEGE failure. A plain non-zero — e.g.
    // `systemctl is-active` returning "failed", or `nginx -t` reporting a real
    // config error — is a valid answer, not a permission problem, so leave it.
    auto has = [&](const char* s) { return r.out.find(s) != std::string::npos; };
    const bool priv =
        has("permission denied") || has("Permission denied") ||
        has("Interactive authentication required") || has("authentication is required") ||
        has("Access denied") || has("must be run as root") || has("You must be root") ||
        has("requires root") || has("Operation not permitted");
    if (!priv) return r;
    std::vector<std::string> s = {"sudo", "-n"};
    s.insert(s.end(), args.begin(), args.end());
    CmdResult r2 = runCmd(s, env);
    if (r2.code != 0 &&
        (r2.out.find("sudo:") != std::string::npos ||
         r2.out.find("password is required") != std::string::npos ||
         r2.out.find("a terminal is required") != std::string::npos)) {
        r2.out = "This action needs root privileges.\n"
                 "Run the console with sudo:   sudo ./build/erp-admin\n"
                 "or allow passwordless sudo for this command in /etc/sudoers.d/.\n\n"
                 "--- sudo output ---\n" + r2.out;
    }
    return r2;
}

// ─────────────────────── HTTP helpers ─────────────────────
using drogon::HttpRequestPtr;
using drogon::HttpResponsePtr;
using drogon::HttpResponse;

static bool isLoopback(const HttpRequestPtr& req) {
    const std::string ip = req->getPeerAddr().toIp();
    return ip == "127.0.0.1" || ip == "::1" || ip == "::ffff:127.0.0.1";
}
static bool authed(const HttpRequestPtr& req) {
    if (!isLoopback(req)) return false;
    std::string tok = req->getHeader("X-Admin-Token");
    if (tok.empty()) tok = req->getParameter("token");
    return !tok.empty() && tok == g_token;
}
static HttpResponsePtr jsonResp(const json& j, drogon::HttpStatusCode code = drogon::k200OK) {
    auto r = HttpResponse::newHttpResponse();
    r->setStatusCode(code);
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    r->setBody(j.dump());
    return r;
}
static HttpResponsePtr denied() {
    return jsonResp({{"error", "unauthorized (loopback + token required)"}}, drogon::k401Unauthorized);
}
// Wrap a CmdResult into a JSON API result.
static json cmdJson(const CmdResult& r) {
    return {{"code", r.code}, {"ok", r.code == 0}, {"output", r.out}};
}

// read a whole file (bounded)
static std::string readFile(const std::string& path, size_t maxBytes = 2 * 1024 * 1024) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > maxBytes) s = s.substr(s.size() - maxBytes);
    return s;
}

static std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    char b[32]; std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", std::localtime(&t));
    return b;
}

// ─────────────────── tenants.json helpers ─────────────────
static std::string tenantsPath() {
    auto slash = g_configPath.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? "" : g_configPath.substr(0, slash + 1);
    return dir + "tenants.json";
}
static json loadTenants() {
    std::ifstream f(tenantsPath());
    if (!f.is_open()) return json::array();
    try {
        json j; f >> j;
        if (j.is_object() && j.contains("tenants")) return j["tenants"];
        if (j.is_array()) return j;
    } catch (...) {}
    return json::array();
}
static void saveTenants(const json& arr) {
    std::ofstream f(tenantsPath());
    f << json{{"tenants", arr}}.dump(2);
}

// ─────────────────────── endpoints ────────────────────────
static void registerApi() {
    auto& app = drogon::app();

    // The single-page UI (no auth on the shell; the API is what's gated).
    app.registerHandler("/", [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& cb) {
        auto r = HttpResponse::newHttpResponse();
        std::string html = readFile("tools/admin/web/index.html");
        if (html.empty()) html = readFile(g_appRoot + "/tools/admin/web/index.html");
        r->setContentTypeCode(drogon::CT_TEXT_HTML);
        r->setBody(html.empty() ? "<h1>erp-admin: UI file not found</h1>" : html);
        cb(r);
    }, {drogon::Get});

    app.registerHandler("/favicon.ico", [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& cb) {
        auto r = HttpResponse::newHttpResponse();
        r->setStatusCode(drogon::k204NoContent);
        cb(r);
    }, {drogon::Get});

    // whoami — lets the UI confirm the token before showing anything
    app.registerHandler("/api/whoami", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        cb(jsonResp({{"ok", true}, {"service", g_serviceName}, {"config", g_configPath}}));
    }, {drogon::Get});

    // ── Overview ──────────────────────────────────────────
    app.registerHandler("/api/overview", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        json o;
        o["service"] = cmdJson(runMaybePriv({"systemctl", "is-active", g_serviceName}));
        o["db_ok"]   = runCmd(pgArgs("psql", "postgres"), pgEnv()).code == 0
                       ? (json)true : (json)false;
        o["disk"]    = cmdJson(runCmd({"df", "-h", "/"}));
        o["uptime"]  = cmdJson(runCmd({"uptime"}));
        o["db"]      = {{"host", g_db.host}, {"port", g_db.port}, {"user", g_db.user}, {"name", g_db.name}};
        cb(jsonResp(o));
    }, {drogon::Get});

    // ── Databases ─────────────────────────────────────────
    app.registerHandler("/api/databases", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        auto r = runCmd(pgArgs("psql", "postgres"), pgEnv());
        std::vector<std::string> extra = {"-tAc",
            "SELECT datname||'|'||pg_size_pretty(pg_database_size(datname)) "
            "FROM pg_database WHERE datistemplate=false ORDER BY datname"};
        auto args = pgArgs("psql", "postgres");
        args.insert(args.end(), extra.begin(), extra.end());
        auto q = runCmd(args, pgEnv());
        json dbs = json::array();
        std::istringstream ss(q.out); std::string ln;
        while (std::getline(ss, ln)) {
            ln = trim(ln); if (ln.empty()) continue;
            auto bar = ln.find('|');
            dbs.push_back({{"name", ln.substr(0, bar)},
                           {"size", bar == std::string::npos ? "" : ln.substr(bar + 1)}});
        }
        cb(jsonResp({{"ok", q.code == 0}, {"databases", dbs}, {"error", q.code == 0 ? "" : q.out}}));
    }, {drogon::Get});

    // ── Backups ───────────────────────────────────────────
    app.registerHandler("/api/backups", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        runCmd({"mkdir", "-p", g_backupDir});
        auto ls = runCmd({"ls", "-1", g_backupDir});
        json files = json::array();
        std::istringstream ss(ls.out); std::string ln;
        while (std::getline(ss, ln)) { ln = trim(ln); if (!ln.empty() && ln.size() > 5 && ln.substr(ln.size()-5)==".dump") files.push_back(ln); }
        cb(jsonResp({{"ok", true}, {"dir", g_backupDir}, {"files", files}}));
    }, {drogon::Get});

    app.registerHandler("/api/backup", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        json body; try { body = json::parse(req->getBody()); } catch (...) {}
        std::string db = body.value("db", "");
        if (!validName(db)) return cb(jsonResp({{"error", "invalid database name"}}, drogon::k400BadRequest));
        runCmd({"mkdir", "-p", g_backupDir});
        std::string file = g_backupDir + "/" + db + "-" + nowStamp() + ".dump";
        auto args = pgArgs("pg_dump", db);   // -Fc custom (compressed) format
        args.push_back("-Fc"); args.push_back("-f"); args.push_back(file);
        auto r = runCmd(args, pgEnv());
        cb(jsonResp({{"ok", r.code == 0}, {"file", r.code == 0 ? file : ""}, {"output", r.out}}));
    }, {drogon::Post});

    app.registerHandler("/api/restore", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        json body; try { body = json::parse(req->getBody()); } catch (...) {}
        std::string db = body.value("db", ""), file = body.value("file", "");
        if (!validName(db))         return cb(jsonResp({{"error", "invalid database name"}}, drogon::k400BadRequest));
        if (!validBackupFile(file)) return cb(jsonResp({{"error", "invalid backup file"}}, drogon::k400BadRequest));
        std::string path = g_backupDir + "/" + file;
        if (access(path.c_str(), R_OK) != 0) return cb(jsonResp({{"error", "backup file not found"}}, drogon::k404NotFound));
        // ensure the target db exists (createdb is a no-op-ish if it already does)
        runCmd(pgArgs("createdb", ""), pgEnv()).out.clear();
        { auto ca = pgArgs("createdb"); ca.push_back(db); runCmd(ca, pgEnv()); }
        auto args = pgArgs("pg_restore", db);
        args.push_back("--clean"); args.push_back("--if-exists"); args.push_back("--no-owner"); args.push_back(path);
        auto r = runCmd(args, pgEnv());
        // pg_restore returns non-zero on benign warnings; report output verbatim
        cb(jsonResp({{"ok", true}, {"code", r.code}, {"output", r.out}}));
    }, {drogon::Post});

    app.registerHandler("/api/backup/download", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        std::string file = req->getParameter("file");
        if (!validBackupFile(file)) return cb(jsonResp({{"error", "invalid file"}}, drogon::k400BadRequest));
        std::string path = g_backupDir + "/" + file;
        if (access(path.c_str(), R_OK) != 0) return cb(jsonResp({{"error", "not found"}}, drogon::k404NotFound));
        auto r = HttpResponse::newFileResponse(path, file, drogon::CT_APPLICATION_OCTET_STREAM);
        cb(r);
    }, {drogon::Get});

    // ── Tenants (multi-company) ───────────────────────────
    app.registerHandler("/api/tenants", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        cb(jsonResp({{"ok", true}, {"tenants", loadTenants()}, {"file", tenantsPath()}}));
    }, {drogon::Get});

    app.registerHandler("/api/tenants/create", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        json body; try { body = json::parse(req->getBody()); } catch (...) {}
        std::string name = body.value("name", ""), sub = body.value("subdomain", ""), emails = body.value("email_domains", "");
        if (!validName(name)) return cb(jsonResp({{"error", "invalid company database name (use [A-Za-z0-9_])"}}, drogon::k400BadRequest));
        if (!sub.empty() && !validDomain(sub))      return cb(jsonResp({{"error", "invalid subdomain"}}, drogon::k400BadRequest));
        // provision_tenant.sh does: createdb -> register in tenants.json -> provision
        std::vector<std::string> args = {"bash", "tools/provision_tenant.sh", name};
        if (!sub.empty())    args.push_back(sub);
        if (!emails.empty()) { if (sub.empty()) args.push_back(""); args.push_back(emails); }
        auto r = runCmd(args, {{"ERP_CONFIG", g_configPath}, {"PGPASSWORD", g_db.pass}}, g_appRoot);
        cb(jsonResp({{"ok", r.code == 0}, {"output", r.out}}));
    }, {drogon::Post});

    // Link an EXISTING database as a tenant (register only; no provisioning).
    app.registerHandler("/api/tenants/link", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        json body; try { body = json::parse(req->getBody()); } catch (...) {}
        std::string name = body.value("name", ""), sub = body.value("subdomain", "");
        if (!validName(name)) return cb(jsonResp({{"error", "invalid database name"}}, drogon::k400BadRequest));
        json arr = loadTenants();
        for (auto& t : arr) if (t.value("name", "") == name)
            return cb(jsonResp({{"error", "tenant already registered"}}, drogon::k400BadRequest));
        json e = {{"name", name}, {"active", true}};
        if (!sub.empty()) e["subdomain"] = sub;
        arr.push_back(e); saveTenants(arr);
        cb(jsonResp({{"ok", true}, {"tenant", e}}));
    }, {drogon::Post});

    app.registerHandler("/api/tenants/toggle", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        json body; try { body = json::parse(req->getBody()); } catch (...) {}
        std::string name = body.value("name", "");
        json arr = loadTenants(); bool found = false;
        for (auto& t : arr) if (t.value("name", "") == name) { t["active"] = !t.value("active", true); found = true; }
        if (!found) return cb(jsonResp({{"error", "tenant not found"}}, drogon::k404NotFound));
        saveTenants(arr);
        cb(jsonResp({{"ok", true}, {"tenants", arr}}));
    }, {drogon::Post});

    // ── nginx ─────────────────────────────────────────────
    app.registerHandler("/api/nginx", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        auto cfg = runMaybePriv({"cat", g_nginxConf});
        cb(jsonResp({{"ok", true},
                     {"config_path", g_nginxConf},
                     {"config", cfg.code == 0 ? cfg.out
                                : (std::string("(could not read ") + g_nginxConf + ")\n" + cfg.out)},
                     {"test", cmdJson(runMaybePriv({"nginx", "-t"}))}}));
    }, {drogon::Get});
    app.registerHandler("/api/nginx/reload", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        auto test = runMaybePriv({"nginx", "-t"});
        if (test.code != 0) return cb(jsonResp({{"ok", false}, {"stage", "test"}, {"output", test.out}}));
        cb(jsonResp(cmdJson(runMaybePriv({"systemctl", "reload", "nginx"}))));
    }, {drogon::Post});

    // ── SSL (certbot) ─────────────────────────────────────
    app.registerHandler("/api/ssl", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        cb(jsonResp(cmdJson(runMaybePriv({"certbot", "certificates"}))));
    }, {drogon::Get});
    app.registerHandler("/api/ssl/renew", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        json body; try { body = json::parse(req->getBody()); } catch (...) {}
        bool dry = body.value("dry_run", true);
        std::vector<std::string> a = {"certbot", "renew"};
        if (dry) a.push_back("--dry-run");
        cb(jsonResp(cmdJson(runMaybePriv(a))));
    }, {drogon::Post});

    // ── Service control + logs ────────────────────────────
    app.registerHandler("/api/service", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        json body; try { body = json::parse(req->getBody()); } catch (...) {}
        std::string action = body.value("action", "status");
        static const std::vector<std::string> allowed = {"status", "start", "stop", "restart"};
        bool ok = false; for (auto& a : allowed) if (a == action) ok = true;
        if (!ok) return cb(jsonResp({{"error", "invalid action"}}, drogon::k400BadRequest));
        if (action == "status")
            cb(jsonResp(cmdJson(runMaybePriv({"systemctl", "--no-pager", "status", g_serviceName}))));
        else
            cb(jsonResp(cmdJson(runMaybePriv({"systemctl", action, g_serviceName}))));
    }, {drogon::Post});

    app.registerHandler("/api/logs", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        if (!authed(req)) return cb(denied());
        std::string src = req->getParameter("src");
        int lines = 200; try { lines = std::max(10, std::min(2000, std::stoi(req->getParameter("lines")))); } catch (...) {}
        std::string sl = std::to_string(lines);
        if (src == "nginx")   cb(jsonResp(cmdJson(runMaybePriv({"tail", "-n", sl, "/var/log/nginx/error.log"}))));
        else if (src == "journal") cb(jsonResp(cmdJson(runMaybePriv({"journalctl", "-u", g_serviceName, "-n", sl, "--no-pager"}))));
        else                  cb(jsonResp(cmdJson(runCmd({"tail", "-n", sl, g_logFile}))));
    }, {drogon::Get});
}

// ─────────────────────────── main ─────────────────────────
static std::string randomToken() {
    std::string hex; std::random_device rd;
    std::uniform_int_distribution<int> d(0, 15);
    const char* H = "0123456789abcdef";
    for (int i = 0; i < 40; ++i) hex += H[d(rd)];
    return hex;
}

int main(int argc, char** argv) {
    std::string bind = "127.0.0.1";
    int port = 8072;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--config"     && i + 1 < argc) g_configPath = argv[++i];
        else if (a == "--port"       && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--backup-dir" && i + 1 < argc) g_backupDir = argv[++i];
        else if (a == "--app-root"   && i + 1 < argc) g_appRoot = argv[++i];
        else if (a == "--nginx-conf" && i + 1 < argc) g_nginxConf = argv[++i];
        else if (a == "--service"    && i + 1 < argc) g_serviceName = argv[++i];
        else if (a == "--bind"       && i + 1 < argc) bind = argv[++i];  // e.g. ::1
        else if (a == "--help") {
            printf("erp-admin — loopback-only IT admin console\n"
                   "  --config <path>   c-erp config (default config/system.cfg)\n"
                   "  --port <n>        default 8072\n"
                   "  --backup-dir <d>  default backups\n"
                   "  --app-root <d>    c-erp working dir (default .)\n"
                   "  --service <name>  systemd unit (default c-erp)\n");
            return 0;
        }
    }
    if (bind != "127.0.0.1" && bind != "::1") {
        fprintf(stderr, "[erp-admin] REFUSING to bind %s — this tool is loopback-only.\n", bind.c_str());
        return 2;
    }
    // App root: default to the directory ABOVE the binary (…/build/erp-admin ->
    // project root) so config/, tools/ and web/ resolve no matter where it was
    // launched from. --app-root overrides.
    if (g_appRoot == ".") {
        char buf[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::string exe(buf);
            auto s1 = exe.find_last_of('/');
            if (s1 != std::string::npos) {
                std::string bindir = exe.substr(0, s1);
                auto s2 = bindir.find_last_of('/');
                if (s2 != std::string::npos) g_appRoot = bindir.substr(0, s2);
            }
        }
    }
    if (!g_appRoot.empty() && g_appRoot != ".")
        if (chdir(g_appRoot.c_str()) != 0)
            fprintf(stderr, "[erp-admin] warning: chdir('%s') failed — relative paths may not resolve.\n", g_appRoot.c_str());
    loadConfig(g_configPath);
    g_token = randomToken();
    registerApi();

    printf("\n=====================================================================\n");
    printf(" ERP Admin Console — LOOPBACK ONLY (reach it over an SSH tunnel):\n");
    printf("   ssh -L %d:localhost:%d <user>@<server>\n", port, port);
    printf(" then open:\n");
    printf("   http://127.0.0.1:%d/?token=%s\n", port, g_token.c_str());
    printf("---------------------------------------------------------------------\n");
    { char cwd[PATH_MAX]; if (getcwd(cwd, sizeof(cwd))) printf(" app root : %s\n", cwd); }
    printf(" config   : %s   (db %s@%s:%s)\n", g_configPath.c_str(), g_db.user.c_str(), g_db.host.c_str(), g_db.port.c_str());
    if (!amRoot())
        printf(" NOTE: not running as root — nginx/SSL/systemctl actions will use\n"
               "       `sudo -n`. For full control run:  sudo ./build/erp-admin\n");
    printf("=====================================================================\n\n");
    fflush(stdout);

    drogon::app().addListener(bind, port).setThreadNum(1).run();
    return 0;
}

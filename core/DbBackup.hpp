#pragma once
// ============================================================
// core/DbBackup.hpp — per-tenant database snapshots (docs/075)
//
// pg_dump / pg_restore for the in-app Database section. Every command runs via
// an argv ARRAY (never a shell string), so there is no shell to inject into.
// The CALLER is responsible for the security envelope — admin gate, password
// re-confirmation, and passing ONLY the caller's own tenant DbConfig (so one
// company can never dump/restore another's database).
// ============================================================
#include "infrastructure/DbConnection.hpp"   // DbConfig
#include <nlohmann/json.hpp>

#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

namespace cerp::core {

struct BackupCmd { int code = -1; std::string out; };

inline BackupCmd dbRunCmd_(const std::vector<std::string>& args,
                           const std::vector<std::pair<std::string, std::string>>& env = {}) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return {-1, "pipe() failed"};
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return {-1, "fork() failed"}; }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO); dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        for (auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        fprintf(stderr, "exec %s failed: %s\n", args.empty() ? "?" : args[0].c_str(), strerror(errno));
        _exit(127);
    }
    close(pipefd[1]);
    std::string out; char buf[8192]; ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(pipefd[0]);
    int st = 0; waitpid(pid, &st, 0);
    return {WIFEXITED(st) ? WEXITSTATUS(st) : -1, out};
}

class DbBackup {
public:
    // A backup filename must be a bare *.dump name — no path separators, no "..".
    static bool validFile(const std::string& s) {
        if (s.empty() || s.size() > 200) return false;
        if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos ||
            s.find("..") != std::string::npos) return false;
        return s.size() > 5 && s.substr(s.size() - 5) == ".dump";
    }
    static std::string baseName(const std::string& p) {
        auto s = p.find_last_of('/'); return s == std::string::npos ? p : p.substr(s + 1);
    }
    static std::string stamp() {
        std::time_t t = std::time(nullptr); char b[32];
        std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", std::localtime(&t)); return b;
    }

    static std::vector<std::string> pgArgs(const std::string& bin,
                                           const infrastructure::DbConfig& c, bool withDb = true) {
        std::vector<std::string> a = {bin, "-h", c.host, "-p", std::to_string(c.port), "-U", c.user};
        if (withDb) { a.push_back("-d"); a.push_back(c.name); }
        return a;
    }
    static std::vector<std::pair<std::string, std::string>> pgEnv(const infrastructure::DbConfig& c) {
        return {{"PGPASSWORD", c.password}, {"PGCONNECT_TIMEOUT", "8"}};
    }

    /// Create a compressed snapshot (pg_dump -Fc) in `dir`. Returns {ok,file,output}.
    static nlohmann::json backup(const std::string& dir, const infrastructure::DbConfig& c,
                                 const std::string& label = "") {
        dbRunCmd_({"mkdir", "-p", dir});
        std::string safeLabel;
        for (char ch : label) if (std::isalnum((unsigned char)ch)) safeLabel += ch;
        std::string file = dir + "/" + c.name + "-" + stamp()
                         + (safeLabel.empty() ? "" : ("-" + safeLabel)) + ".dump";
        auto a = pgArgs("pg_dump", c);
        a.push_back("-Fc"); a.push_back("-f"); a.push_back(file);
        auto r = dbRunCmd_(a, pgEnv(c));
        return {{"ok", r.code == 0}, {"file", r.code == 0 ? baseName(file) : ""}, {"output", r.out}};
    }

    /// Restore a snapshot into the tenant DB (DESTRUCTIVE: --clean drops objects).
    static nlohmann::json restore(const std::string& dir, const infrastructure::DbConfig& c,
                                  const std::string& file) {
        if (!validFile(file)) return {{"ok", false}, {"output", "invalid backup file name"}};
        std::string path = dir + "/" + file;
        if (access(path.c_str(), R_OK) != 0) return {{"ok", false}, {"output", "backup not found"}};

        // DROP SCHEMA first, rather than relying on pg_restore --clean.
        //
        // --clean drops objects ONE AT A TIME and treats a failed DROP as a
        // warning. A table with dependents (product_template has several)
        // cannot be dropped that way: the DROP fails, the CREATE then fails
        // with "already exists", and its data is SKIPPED — so every other
        // table is replaced while that one silently keeps its old rows.
        //
        // The result is a restore that reports success and leaves the database
        // in a state that was never dumped: in our case 163 products pointing
        // at template ids the restore had refused to load. Dropping the schema
        // outright removes the ordering problem entirely.
        //
        // Same fix as scripts/db_snapshot.sh — see docs/109 §9.
        {
            auto d = pgArgs("psql", c);
            d.push_back("-q");
            d.push_back("-c");
            d.push_back("DROP SCHEMA IF EXISTS public CASCADE; CREATE SCHEMA public;");
            auto dr = dbRunCmd_(d, pgEnv(c));
            if (dr.code != 0)
                return {{"ok", false},
                        {"output", "could not clear the database before restoring:\n" + dr.out}};
        }

        auto a = pgArgs("pg_restore", c);
        a.push_back("--no-owner"); a.push_back("--no-privileges"); a.push_back(path);
        auto r = dbRunCmd_(a, pgEnv(c));
        // pg_restore exits non-zero on benign warnings; surface output verbatim.
        return {{"ok", true}, {"code", r.code}, {"output", r.out}};
    }

    /// Newest-first list of snapshots in `dir`.
    static std::vector<nlohmann::json> list(const std::string& dir) {
        dbRunCmd_({"mkdir", "-p", dir});
        auto ls = dbRunCmd_({"ls", "-1t", dir});
        std::vector<nlohmann::json> out;
        std::istringstream ss(ls.out); std::string line;
        while (std::getline(ss, line)) {
            if (!validFile(line)) continue;
            auto st = dbRunCmd_({"stat", "-c", "%s|%y", dir + "/" + line});
            std::string sz, when; auto bar = st.out.find('|');
            if (bar != std::string::npos) { sz = st.out.substr(0, bar); when = st.out.substr(bar + 1, 19); }
            out.push_back({{"file", line}, {"size", sz}, {"date", when}});
        }
        return out;
    }
};

} // namespace cerp::core

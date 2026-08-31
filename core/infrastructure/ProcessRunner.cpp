// ============================================================
// core/infrastructure/ProcessRunner.cpp
// ============================================================
#include "ProcessRunner.hpp"

#include <trantor/utils/Logger.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cerp::infrastructure {

// ── SecureTempDir ─────────────────────────────────────────────

SecureTempDir::SecureTempDir() {
    // TMPDIR is honoured so the location can be moved off /tmp in a
    // hardened deployment (e.g. a systemd PrivateTmp unit).
    const char* base = std::getenv("TMPDIR");
    std::string tmpl = (base && *base ? std::string(base) : std::string("/tmp"));
    if (!tmpl.empty() && tmpl.back() == '/') tmpl.pop_back();
    tmpl += "/c-erp-XXXXXX";

    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');

    // mkdtemp() creates the directory with mode 0700 atomically — no
    // window in which another process could win a race on the name.
    if (::mkdtemp(buf.data()) == nullptr)
        throw std::runtime_error(std::string("SecureTempDir: mkdtemp failed: ")
                                 + std::strerror(errno));

    path_.assign(buf.data());
}

SecureTempDir::~SecureTempDir() {
    if (path_.empty()) return;

    // Shallow delete: this class only ever holds regular files created by
    // us, never subdirectories, so a single pass is sufficient.
    if (DIR* d = ::opendir(path_.c_str())) {
        while (dirent* e = ::readdir(d)) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
                continue;
            ::unlink((path_ + "/" + e->d_name).c_str());
        }
        ::closedir(d);
    }
    ::rmdir(path_.c_str());
}


// ── runProcess ────────────────────────────────────────────────

ProcessResult runProcess(const std::vector<std::string>& argv,
                         int*                            exitCode,
                         int                             timeoutMs) {
    if (exitCode) *exitCode = -1;
    if (argv.empty()) return ProcessResult::SpawnFailed;

    // Build the char* array before forking: after fork() in a threaded
    // process only async-signal-safe calls are legal, and allocating is
    // not among them.
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv)
        cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        LOG_ERROR << "[proc] fork failed: " << std::strerror(errno);
        return ProcessResult::SpawnFailed;
    }

    if (pid == 0) {
        // ---- child ----
        // Silence stdout/stderr (replaces the old "2>/dev/null").
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }
        ::execvp(cargv[0], cargv.data());
        // execvp only returns on failure. _exit (not exit) to avoid
        // running the parent's atexit handlers in the forked child.
        ::_exit(127);
    }

    // ---- parent: wait with a timeout ----
    const int  kPollUs  = 5000;                       // 5 ms
    const long maxPolls = (static_cast<long>(timeoutMs) * 1000) / kPollUs;

    for (long i = 0; i < maxPolls; ++i) {
        int   status = 0;
        pid_t r      = ::waitpid(pid, &status, WNOHANG);

        if (r == pid) {
            const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            if (exitCode) *exitCode = code;
            return code == 0 ? ProcessResult::Ok : ProcessResult::NonZeroExit;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR << "[proc] waitpid failed: " << std::strerror(errno);
            return ProcessResult::SpawnFailed;
        }

        struct timespec ts { 0, kPollUs * 1000 };
        ::nanosleep(&ts, nullptr);
    }

    // Timed out — kill and reap so we never leave a zombie or a runaway.
    LOG_ERROR << "[proc] '" << argv[0] << "' exceeded " << timeoutMs
              << " ms — killing pid " << pid;
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return ProcessResult::Timeout;
}

} // namespace cerp::infrastructure

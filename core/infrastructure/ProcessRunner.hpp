#pragma once
// ============================================================
// core/infrastructure/ProcessRunner.hpp
//
// Shell-free subprocess execution + unpredictable temp directories.
//
// Exists to close S-39 (command injection via PDF temp paths) at the
// root rather than per call site, and to enforce SEC-31 ("no
// std::system()").
//
// Why not std::system():
//   std::system() hands the string to /bin/sh, so ANY user-derived
//   substring is a shell-injection sink — including substrings that
//   look pre-validated.  In the S-39 case `std::stoi("12$(id)")`
//   returns 12 without throwing, so an "id" that passed validation
//   still carried a command substitution into the shell.  Quoting is
//   not a fix: "$(...)" and backticks expand inside double quotes.
//   runProcess() passes an argv array straight to execvp(), so there
//   is no shell and no parsing stage that could reinterpret an
//   argument.
//
// Why SecureTempDir:
//   The old paths were deterministic (/tmp/erp_report_<model>_<id>.html).
//   That is both an injection sink (user data in the path) and a
//   symlink-attack target (any local process can pre-plant a symlink at
//   a predictable path and have the server write through it).
//   mkdtemp() gives a 0700 directory with an unpredictable name; the
//   files inside then use fixed literal names, so no user-derived string
//   ever reaches the filesystem path or the argv array.
//
// PERF-E: implementation lives in ProcessRunner.cpp.
// ============================================================
#include <string>
#include <vector>

namespace odoo::infrastructure {

/**
 * @brief RAII temp directory created with mkdtemp() (mode 0700).
 *
 * The directory and everything in it is removed by the destructor, so
 * the caller cannot leak files on an early return or an exception.
 *
 * Names passed to file() MUST be compile-time literals chosen by the
 * caller — never request data.  Keeping user input out of the path is
 * the whole point of this class.
 */
class SecureTempDir {
public:
    /// @throws std::runtime_error if the directory cannot be created.
    SecureTempDir();
    ~SecureTempDir();

    SecureTempDir(const SecureTempDir&)            = delete;
    SecureTempDir& operator=(const SecureTempDir&) = delete;

    /// Absolute path of the directory (no trailing slash).
    const std::string& path() const { return path_; }

    /// Absolute path of a file inside the directory. `name` must be a literal.
    std::string file(const std::string& name) const { return path_ + "/" + name; }

private:
    std::string path_;
};


/// Result of runProcess().
enum class ProcessResult {
    Ok,          ///< Child exited 0
    NonZeroExit, ///< Child ran but exited non-zero
    Timeout,     ///< Child exceeded timeoutMs and was killed
    SpawnFailed, ///< fork()/execvp() failed
};

/**
 * @brief Run a program with an argv array. No shell is involved.
 *
 * argv[0] is the executable, resolved via PATH by execvp().
 * The child's stdout and stderr are sent to /dev/null (this replaces the
 * old "2>/dev/null" shell redirection).
 *
 * A timeout is enforced because wkhtmltopdf can hang indefinitely on
 * malformed HTML; std::system() had no timeout and would block a Drogon
 * worker thread forever.
 *
 * @param argv       Program and arguments. Must be non-empty.
 * @param exitCode   Out-param receiving the child's exit status (may be null).
 * @param timeoutMs  Wall-clock limit; the child is SIGKILLed past it.
 */
ProcessResult runProcess(const std::vector<std::string>& argv,
                         int*                            exitCode  = nullptr,
                         int                             timeoutMs = 30000);

} // namespace odoo::infrastructure

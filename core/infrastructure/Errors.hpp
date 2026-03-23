#pragma once
#include <stdexcept>

namespace odoo::infrastructure {

/**
 * @brief Thrown when an authenticated user lacks permission for the requested operation.
 *
 * Unlike std::runtime_error, this exception is ALWAYS passed through to the client
 * response regardless of devMode — the user needs to know why their request was denied.
 *
 * Generic exceptions (SQL errors, JSON parse failures, etc.) are gated behind devMode
 * to prevent information disclosure (SEC-25).
 *
 * Usage in ViewModel handlers:
 * @code
 *   if (!session.isAdmin)
 *       throw AccessDeniedError("Access denied: Administrator required");
 * @endcode
 */
class AccessDeniedError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

} // namespace odoo::infrastructure

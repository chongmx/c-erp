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

/**
 * @brief Thrown by DbConnection::acquire() when all pool connections are busy
 *        and the timeout expires.
 *
 * Caught by JsonRpcDispatcher to return HTTP 503 Service Unavailable, and by
 * HTTP route lambdas that call htmlError(503, ...).  Never gate the message
 * behind devMode — "pool exhausted" contains no schema information.
 *
 * @see DbConnection::acquire()
 */
class PoolExhaustedException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * @brief Thrown by BaseModel::write() when the record's write_date does not
 *        match the client's expected value (another user saved first).
 *
 * Caught by JsonRpcDispatcher to return a 409-coded JSON-RPC error
 * (error.data.name = "odoo.exceptions.ConcurrencyConflict") so the frontend
 * can distinguish it from generic failures and show a conflict banner.
 *
 * Only raised when the caller supplies __expected_write_date in the values
 * dict AND the resulting UPDATE affects 0 rows.
 */
class ConcurrencyConflictException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

} // namespace odoo::infrastructure

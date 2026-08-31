#pragma once
// =============================================================
// modules/hr/HrKiosk.hpp — the clock-in tablet by the door (docs/113 §3a)
//
// A shared device in a public place, so the security model is the whole
// design:
//
//   * the page itself is UNAUTHENTICATED and carries no session. A PIN
//     authorises ONE action — toggle the attendance of the employee that PIN
//     belongs to — and nothing else. A stolen tablet is worth one person's
//     clock, not the company's data.
//   * the PIN is stored as a PBKDF2 hash, the same way a password is. It is
//     never returned by any read, and an admin sets it without being able to
//     read the old one back.
//   * punches are rate-limited PER IP, because the kiosk is the one endpoint
//     where an attacker can stand and guess 4-digit PINs all day.
//
// The kiosk deliberately cannot list employees. Answering "who works here" to
// an unauthenticated caller would turn the PIN into the only secret, and a
// 4-digit secret is not one.
// =============================================================
#include <memory>
#include <string>

namespace cerp::core { class ModelFactory; class ServiceFactory; }
namespace cerp::infrastructure { class DbConnection; }
namespace pqxx { class transaction_base; }

namespace cerp::modules::hr {

class HrKiosk {
public:
    /// pin_hash on hr_employee. Idempotent.
    static void ensureSchema(pqxx::transaction_base& txn);

    /// GET /kiosk and POST /kiosk/api/punch.
    static void registerRoutes(std::shared_ptr<infrastructure::DbConnection> db,
                               bool devMode,
                               const std::string& trustedProxies);

    /// Hash a PIN for storage. Exposed so hr.employee's set_pin uses exactly
    /// the same derivation the punch route verifies against.
    static std::string hashPin(const std::string& pin);
};

} // namespace cerp::modules::hr

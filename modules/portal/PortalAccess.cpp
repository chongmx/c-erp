// =============================================================
// modules/portal/PortalAccess.cpp — implementation (docs/114 W1)
// =============================================================
#include "PortalAccess.hpp"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <pqxx/pqxx>
#include <map>
#include <stdexcept>
#include <string>

namespace cerp::modules::portal {

namespace {

// model -> (table, the column holding the owning partner). Allow-listed, not
// validated: the model name selects a table and a column, so a charset check
// would still let a caller name any table in the database (S-49).
const std::map<std::string, std::pair<const char*, const char*>>& shareable() {
    static const std::map<std::string, std::pair<const char*, const char*>> m = {
        {"account.move",  {"account_move",  "partner_id"}},
        {"sale.order",    {"sale_order",    "partner_id"}},
        {"stock.picking", {"stock_picking", "partner_id"}},
    };
    return m;
}

} // anonymous namespace

bool PortalAccess::isShareableModel(const std::string& model) {
    return shareable().count(model) != 0;
}

bool PortalAccess::secureEquals(const std::string& a, const std::string& b) {
    // Length is compared first and NOT in constant time — that is fine and is
    // what every implementation does, including the reference ERP's consteq: the length of a
    // token is not a secret, its contents are.
    if (a.size() != b.size()) return false;
    if (a.empty()) return false;   // two empty strings are never "equal" here
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string PortalAccess::randomToken(std::size_t nbytes) {
    std::vector<unsigned char> buf(nbytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1)
        throw std::runtime_error("PortalAccess: RAND_bytes failed");
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(nbytes * 2);
    for (unsigned char c : buf) { out += hexd[c >> 4]; out += hexd[c & 0x0F]; }
    return out;
}

void PortalAccess::ensureSchema(pqxx::transaction_base& txn) {
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS portal_access_token (
            id          SERIAL PRIMARY KEY,
            model       VARCHAR NOT NULL,
            res_id      INTEGER NOT NULL,
            token       VARCHAR NOT NULL,
            created_at  TIMESTAMP NOT NULL DEFAULT now(),
            expires_at  TIMESTAMP,
            revoked     BOOLEAN NOT NULL DEFAULT FALSE,
            created_uid INTEGER
        )
    )");
    // One LIVE token per record. Re-sharing replaces rather than adds, so a
    // leaked link cannot survive alongside its replacement.
    txn.exec(R"(
        CREATE UNIQUE INDEX IF NOT EXISTS portal_access_token_live_uniq
            ON portal_access_token (model, res_id) WHERE revoked = FALSE
    )");
    // The lookup the /portal/doc route makes on every request.
    txn.exec(R"(
        CREATE INDEX IF NOT EXISTS portal_access_token_lookup_idx
            ON portal_access_token (model, res_id, revoked)
    )");
}

bool PortalAccess::owns(pqxx::transaction_base& txn,
                        const std::string& model,
                        int resId,
                        int partnerId)
{
    if (partnerId <= 0 || resId <= 0) return false;
    auto it = shareable().find(model);
    if (it == shareable().end()) return false;

    // The table and column come from the allow-list above, never from the
    // caller; the VALUES are bound.
    const std::string sql =
        std::string("SELECT 1 FROM ") + it->second.first +
        " WHERE id = $1 AND " + it->second.second + " = $2 LIMIT 1";
    return !txn.exec(sql, pqxx::params{resId, partnerId}).empty();
}

AccessKind PortalAccess::check(pqxx::transaction_base& txn,
                               const std::string& model,
                               int                resId,
                               int                partnerId,
                               const std::string& token)
{
    if (!isShareableModel(model) || resId <= 0) return AccessKind::Denied;

    // 1. Ownership. The ordinary case, and the only one that may write.
    if (owns(txn, model, resId, partnerId)) return AccessKind::Owner;

    // 2. A share token, bound to THIS record. Fetching by (model, res_id)
    //    rather than by token is what stops document A's token opening
    //    document B — the token is never used as a lookup key.
    if (token.empty()) return AccessKind::Denied;
    auto rows = txn.exec(
        "SELECT token FROM portal_access_token "
        " WHERE model=$1 AND res_id=$2 AND revoked=FALSE "
        "   AND (expires_at IS NULL OR expires_at > now()) "
        " LIMIT 1",
        pqxx::params{model, resId});
    if (rows.empty()) return AccessKind::Denied;

    if (secureEquals(std::string(rows[0][0].c_str()), token))
        return AccessKind::Token;
    return AccessKind::Denied;
}

std::string PortalAccess::share(pqxx::transaction_base& txn,
                                const std::string& model,
                                int resId,
                                int createdUid,
                                int days)
{
    if (!isShareableModel(model))
        throw std::runtime_error("That kind of document cannot be shared.");
    auto it = shareable().find(model);
    auto exists = txn.exec(
        std::string("SELECT 1 FROM ") + it->second.first + " WHERE id=$1",
        pqxx::params{resId});
    if (exists.empty())
        throw std::runtime_error("That document does not exist.");

    // Replace, never accumulate: the old link must stop working.
    txn.exec("UPDATE portal_access_token SET revoked=TRUE "
             " WHERE model=$1 AND res_id=$2 AND revoked=FALSE",
             pqxx::params{model, resId});

    const std::string token = randomToken();
    txn.exec(
        "INSERT INTO portal_access_token (model, res_id, token, expires_at, created_uid) "
        "VALUES ($1, $2, $3, now() + make_interval(days => $4), NULLIF($5,0))",
        pqxx::params{model, resId, token, days, createdUid});
    return token;
}

bool PortalAccess::revoke(pqxx::transaction_base& txn,
                          const std::string& model,
                          int resId)
{
    auto r = txn.exec(
        "UPDATE portal_access_token SET revoked=TRUE "
        " WHERE model=$1 AND res_id=$2 AND revoked=FALSE RETURNING id",
        pqxx::params{model, resId});
    return !r.empty();
}

} // namespace cerp::modules::portal

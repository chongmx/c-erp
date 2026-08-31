#pragma once
// =============================================================
// modules/portal/PortalAccess.hpp — the ONE authorization funnel (docs/114 W1)
//
// Adapted from the reference ERP's portal.mixin + _document_check_access. The idea is
// theirs; the shape is ours.
//
// the reference ERP asks two questions in one place for every portal document route:
//   1. does the caller own this record?  else
//   2. did they present a valid per-record access token?
// and nothing reaches a document except through that function. c-erp had the
// same rule spelled out twelve times, once per route — correct today (journey
// 09-portal proves it) but twelve chances to drift.
//
// Where we deliberately differ from the reference ERP:
//
//   * the reference ERP hangs `access_token` on each model through a mixin. We have no
//     mixin machinery, and adding a column to every shareable table means a
//     migration per model. One `portal_access_token` table keyed (model,
//     res_id) adds no columns and makes "revoke every link we ever shared" a
//     single statement — which is the operation you actually want during an
//     incident.
//   * Tokens EXPIRE. the reference ERP's live until someone clears the field.
//   * Comparison is constant-time. A token compared with == leaks its prefix
//     through response timing, one byte at a time.
// =============================================================
#include <string>

namespace pqxx { class transaction_base; }

namespace cerp::modules::portal {

/// What a document route is allowed to do with a record.
enum class AccessKind {
    Denied,   ///< neither owner nor a valid token
    Owner,    ///< the authenticated portal partner owns it
    Token,    ///< opened via a valid share link — READ ONLY
};

class PortalAccess {
public:
    /// Table + indexes. Idempotent.
    static void ensureSchema(pqxx::transaction_base& txn);

    /**
     * The funnel. Owner first, then token.
     *
     * @param model     "account.move" | "sale.order" | "stock.picking"
     * @param resId     the record
     * @param partnerId the authenticated portal partner, or 0 for none
     * @param token     the ?token= from the URL, or ""
     *
     * A token is bound to ONE (model, res_id): presenting document A's token
     * while asking for document B is Denied, which is the mistake that turns a
     * share link into a directory listing.
     */
    static AccessKind check(pqxx::transaction_base& txn,
                            const std::string& model,
                            int                resId,
                            int                partnerId,
                            const std::string& token);

    /// True when this partner owns the record. Used by check(), and directly
    /// by the list routes, which are scoped rather than per-record.
    static bool owns(pqxx::transaction_base& txn,
                     const std::string& model,
                     int resId,
                     int partnerId);

    /**
     * Mint (or re-mint) a share token for a record. Staff-only — the caller
     * checks that. Returns the token; `days` is its life.
     *
     * Re-sharing a record REPLACES its token, so the previous link stops
     * working. That is the safe default: "share again" after a leak should
     * invalidate what leaked, not add a second door.
     */
    static std::string share(pqxx::transaction_base& txn,
                             const std::string& model,
                             int resId,
                             int createdUid,
                             int days = 30);

    /// Revoke the token for one record. Returns true if there was one.
    static bool revoke(pqxx::transaction_base& txn,
                       const std::string& model,
                       int resId);

    /// Constant-time equality. Exposed because CSRF needs it too (W2).
    static bool secureEquals(const std::string& a, const std::string& b);

    /// Cryptographically random hex token.
    static std::string randomToken(std::size_t nbytes = 24);

    /// The models a share link may ever address. Anything else is refused
    /// before it reaches SQL — the model name is interpolated into a table
    /// lookup, so it is allow-listed rather than validated (S-49).
    static bool isShareableModel(const std::string& model);
};

} // namespace cerp::modules::portal

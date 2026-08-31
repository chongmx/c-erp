#pragma once
// ============================================================
// core/ControlPlane.hpp  —  cross-tenant identity directory (docs/072 Phase 2)
//
// The "control plane": a small database, separate from the per-company data
// planes, that answers "which companies can this person reach, and what is
// their login in each". It powers the login company-chooser and the top-bar
// company switcher (cross-tenant SSO): once a person has proved their identity
// in one company, the control plane vouches for them in the others.
//
// Singleton, opened once at boot from a DbConfig (Container). Holds its own
// connection pool to the control database — it is NOT a company tenant and is
// never provisioned with the ERP schema.
// ============================================================
#include "infrastructure/DbConnection.hpp"
#include <memory>
#include <string>
#include <vector>

namespace cerp::core {

class ControlPlane {
public:
    struct Membership {
        std::string tenantDb;    ///< the company database
        std::string localLogin;  ///< this identity's login in that company
    };

    // A product in the shared catalogue (docs/072 Phase 3). Independent tenants
    // opt in to pull these into their own product tables.
    struct SharedProduct {
        std::string code;        ///< default_code (the shared key)
        std::string name;
        long long   listPrice;   ///< micro-units (scale 6), like the ERP
    };

    /// Open the control database pool + ensure its schema. Idempotent.
    static void initialize(const infrastructure::DbConfig& controlCfg);
    static bool ready();
    static ControlPlane& instance();

    /// Every company this identity can access.
    std::vector<Membership> membershipsFor(const std::string& identity) const;

    /// The global identity that owns (tenantDb, localLogin), or "" if none.
    std::string identityFor(const std::string& tenantDb,
                            const std::string& localLogin) const;

    /// The local login for `identity` in `tenantDb`, or "" if not a member.
    std::string loginFor(const std::string& identity,
                         const std::string& tenantDb) const;

    /// Register/refresh a membership (provisioning / admin).
    void upsertMembership(const std::string& identity,
                          const std::string& tenantDb,
                          const std::string& localLogin);
    void removeMembership(const std::string& identity, const std::string& tenantDb);

    struct MembershipRow { std::string identity, tenantDb, localLogin; bool active; };
    /// Every membership (admin listing).
    std::vector<MembershipRow> allMemberships() const;

    /// The shared catalogue (docs/072 Phase 3).
    std::vector<SharedProduct> sharedProducts() const;
    void upsertSharedProduct(const std::string& code, const std::string& name,
                             long long listPriceMicros);
    void removeSharedProduct(const std::string& code);

private:
    explicit ControlPlane(std::shared_ptr<infrastructure::DbConnection> db);
    void ensureSchema_();

    std::shared_ptr<infrastructure::DbConnection> db_;
    static std::unique_ptr<ControlPlane>          s_instance_;
};

} // namespace cerp::core

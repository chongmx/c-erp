#pragma once
// ============================================================
// core/UserContext.hpp  —  S-30: Record-Level Authorization
//
// Lightweight value type carrying the calling user's identity
// for record-rule evaluation.  Populated from Session fields
// in JsonRpcDispatcher and propagated to BaseModel via
// IModel::setUserContext() before each CRUD/search call.
// ============================================================
#include <vector>

namespace odoo::core {

struct UserContext {
    int              uid       = 0;   ///< res.users id  (0 = anonymous)
    int              companyId = 0;   ///< res.company.id — the ACTIVE company
    int              partnerId = 0;   ///< res.partner.id
    std::vector<int> groupIds;        ///< all res_groups ids this user belongs to
    bool             isAdmin   = false; ///< bypasses all record rules
    /// docs/094 — every company this user may switch to (res_company_users_rel).
    /// `companyId` is whichever of these is currently selected; records are
    /// only ever read from and written to that one.
    std::vector<int> allowedCompanyIds;

    bool isAuthenticated() const { return uid > 0; }

    bool hasGroup(int gid) const {
        for (int g : groupIds) if (g == gid) return true;
        return false;
    }

    /// May this user act for `cid`? Note this is NOT what controls visibility —
    /// visibility is always the single active company. This gates switching and
    /// creating on behalf of a company.
    bool mayUseCompany(int cid) const {
        if (cid <= 0) return false;
        for (int c : allowedCompanyIds) if (c == cid) return true;
        return false;
    }
};

// ============================================================
// CurrentUser — the calling user, per request thread (docs/094)
// ============================================================
/**
 * @brief Ambient user context for the duration of one HTTP request.
 *
 * Models receive their context via IModel::setUserContext(), which
 * GenericViewModel calls for every model on the generic path. Hand-written
 * ViewModels are a different story: PartnerViewModel, for one, never called it,
 * so res.partner reached BaseModel with an anonymous context. That silently
 * disabled company scoping for exactly that model — and the same defect class
 * has bitten this codebase four times before (S-35 record rules, S-37 audit,
 * S-38 CSV rules, S-47 privilege audit), each time because a cross-cutting
 * concern was wired into GenericViewModel and absent from the bespoke ones.
 *
 * So company scoping does not depend on a ViewModel remembering. The dispatcher
 * publishes the session here for the request, and BaseModel falls back to it
 * when its own context is anonymous. Drogon runs a handler on one worker thread
 * start to finish, which is the same assumption TenantScope already relies on.
 *
 * Outside a request — cron, migrations, startup seeding — nothing is published,
 * so the context is anonymous and no scoping applies. That is intended: those
 * callers legitimately work across companies.
 */
class CurrentUser {
public:
    static const UserContext& get() { return slot_(); }

    /// RAII: publish `ctx` for this thread, restore the previous value on exit.
    class Scope {
    public:
        explicit Scope(const UserContext& ctx) : saved_(slot_()) { slot_() = ctx; }
        ~Scope() { slot_() = saved_; }
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        UserContext saved_;
    };

private:
    static UserContext& slot_() {
        static thread_local UserContext ctx;
        return ctx;
    }
};

} // namespace odoo::core

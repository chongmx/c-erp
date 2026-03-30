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
    int              companyId = 0;   ///< res.company.id
    int              partnerId = 0;   ///< res.partner.id
    std::vector<int> groupIds;        ///< all res_groups ids this user belongs to
    bool             isAdmin   = false; ///< bypasses all record rules

    bool isAuthenticated() const { return uid > 0; }

    bool hasGroup(int gid) const {
        for (int g : groupIds) if (g == gid) return true;
        return false;
    }
};

} // namespace odoo::core

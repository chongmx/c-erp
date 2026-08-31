// =============================================================
// modules/rental/RentalEvents.cpp
// =============================================================
#include "RentalEvents.hpp"

#include <pqxx/pqxx>

namespace cerp::modules::rental {

void RentalEvents::emit(pqxx::transaction_base& txn,
                        const std::string&      type,
                        const EventCtx&         ctx,
                        const std::string&      summary,
                        const nlohmann::json&   detail) {
    pqxx::params p;
    p.append(type);
    // Zero means "not applicable" and must reach the DB as NULL, not as 0
    // — a 0 would join to nothing and read as a real id in the feed.
    auto opt = [&p](int v) { if (v > 0) p.append(v); else p.append(nullptr); };
    opt(ctx.contractId);
    opt(ctx.lineId);
    opt(ctx.unitId);
    opt(ctx.partnerId);
    opt(ctx.userId);
    p.append(summary);
    if (detail.is_null()) p.append(nullptr);
    else                  p.append(detail.dump());
    if (ctx.refModel.empty()) p.append(nullptr); else p.append(ctx.refModel);
    opt(ctx.refId);
    p.append(ctx.companyId);

    txn.exec(
        "INSERT INTO rental_event "
        "(event_type, contract_id, line_id, unit_id, partner_id, user_id, "
        " summary, detail, ref_model, ref_id, company_id) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8::jsonb,$9,$10,$11)", p);
}

} // namespace cerp::modules::rental

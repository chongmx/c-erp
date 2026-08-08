// =============================================================
// core/IrModelData.cpp
// =============================================================
#include "IrModelData.hpp"

#include <pqxx/pqxx>

namespace odoo::core {

int IrModelData::ensure(pqxx::transaction_base& txn,
                        const std::string& module,
                        const std::string& name,
                        const std::string& model,
                        int                resId,
                        bool               noupdate) {
    // ON CONFLICT on the (module, name) unique key. When noupdate, the
    // WHERE on the DO UPDATE is false, so an existing row is preserved;
    // RETURNING still yields its id via the trick of a no-op update.
    //
    // Two statements rather than one clever upsert: the "leave it alone"
    // path must not touch write_date either, and expressing that in a
    // single ON CONFLICT clause is harder to read than it is worth.
    auto existing = txn.exec(
        "SELECT id FROM ir_model_data WHERE module = $1 AND name = $2",
        pqxx::params{module, name});

    if (!existing.empty()) {
        const int rowId = existing[0][0].as<int>();
        if (!noupdate) {
            txn.exec(
                "UPDATE ir_model_data "
                "   SET model = $2, res_id = $3, write_date = now() "
                " WHERE id = $1",
                pqxx::params{rowId, model, resId});
        }
        return rowId;
    }

    auto ins = txn.exec(
        "INSERT INTO ir_model_data (module, name, model, res_id, noupdate) "
        "VALUES ($1,$2,$3,$4,$5) RETURNING id",
        pqxx::params{module, name, model, resId, noupdate});
    return ins[0][0].as<int>();
}

std::optional<XmlRef> IrModelData::lookup(pqxx::transaction_base& txn,
                                          const std::string& module,
                                          const std::string& name) {
    auto r = txn.exec(
        "SELECT model, res_id FROM ir_model_data WHERE module = $1 AND name = $2",
        pqxx::params{module, name});
    if (r.empty()) return std::nullopt;
    return XmlRef{r[0][0].c_str(), r[0][1].as<int>()};
}

std::optional<XmlRef> IrModelData::lookup(pqxx::transaction_base& txn,
                                          const std::string& xmlId) {
    const auto dot = xmlId.find('.');
    if (dot == std::string::npos) return std::nullopt;   // must be module.name
    return lookup(txn, xmlId.substr(0, dot), xmlId.substr(dot + 1));
}

int IrModelData::refId(pqxx::transaction_base& txn, const std::string& xmlId) {
    auto ref = lookup(txn, xmlId);
    return ref ? ref->resId : 0;
}

std::string IrModelData::xmlIdOf(pqxx::transaction_base& txn,
                                 const std::string& model, int resId) {
    auto r = txn.exec(
        "SELECT module || '.' || name FROM ir_model_data "
        " WHERE model = $1 AND res_id = $2 LIMIT 1",
        pqxx::params{model, resId});
    if (r.empty() || r[0][0].is_null()) return "";
    return r[0][0].c_str();
}

} // namespace odoo::core

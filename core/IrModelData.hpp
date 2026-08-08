#pragma once
// =============================================================
// core/IrModelData.hpp — external identifiers (ir.model.data)
//
// Maps a stable "module.name" xml_id to a concrete (model, res_id), so a
// seeded record can be referenced by a name that never changes even as
// serial ids do. The standard Odoo mechanism; a number of later features
// (data export, module uninstall, referencing config rows) assume it.
//
// All operations take the CALLER'S transaction: an xml_id and the record
// it names must be created or destroyed together, or the mapping dangles.
// =============================================================
#include <optional>
#include <string>

namespace pqxx { class transaction_base; }

namespace odoo::core {

struct XmlRef {
    std::string model;
    int         resId = 0;
};

class IrModelData {
public:
    /**
     * Record (or update) an xml_id -> (model, res_id) mapping.
     *
     * @param noupdate  when true, an existing mapping is LEFT ALONE — the
     *                  record has been hand-edited and the seed is only a
     *                  starting point. When false, the target is refreshed.
     * @returns the ir_model_data row id.
     */
    static int ensure(pqxx::transaction_base& txn,
                      const std::string& module,
                      const std::string& name,
                      const std::string& model,
                      int                resId,
                      bool               noupdate = false);

    /// Resolve "module.name" (or module + name) to its (model, res_id).
    static std::optional<XmlRef> lookup(pqxx::transaction_base& txn,
                                        const std::string& module,
                                        const std::string& name);
    static std::optional<XmlRef> lookup(pqxx::transaction_base& txn,
                                        const std::string& xmlId /* "module.name" */);

    /// Resolve straight to the res_id, or 0 if unknown.
    static int refId(pqxx::transaction_base& txn, const std::string& xmlId);

    /// The xml_id of a record, or "" if it has none.
    static std::string xmlIdOf(pqxx::transaction_base& txn,
                               const std::string& model, int resId);
};

} // namespace odoo::core

#pragma once
#include "interfaces/IView.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace odoo::core {

// ============================================================
// BaseView
// ============================================================
/**
 * @brief Non-templated convenience base for JSON view serializers.
 *
 * Provides default implementations of renderList() and renderFields()
 * built on top of the abstract render() so concrete views only need to
 * implement three methods: viewName(), arch(), fields(), and render().
 *
 * Design note:
 *   BaseView is intentionally NOT templated on a model type. Views
 *   speak nlohmann::json at every boundary — the typed model objects
 *   are the concern of the ViewModel and Service layers. This avoids
 *   the ViewAdapter / type-erasure complexity entirely.
 *
 * Minimal concrete view:
 * @code
 *   class PartnerFormView : public BaseView {
 *   public:
 *       std::string viewName() const override { return "res.partner.form"; }
 *
 *       std::string arch() const override {
 *           return R"(<form><field name="name"/><field name="email"/></form>)";
 *       }
 *
 *       nlohmann::json fields() const override {
 *           return {
 *               {"name",  {{"type","char"},  {"string","Name"},  {"required",true}}},
 *               {"email", {{"type","char"},  {"string","Email"}}},
 *           };
 *       }
 *
 *       nlohmann::json render(const nlohmann::json& record) const override {
 *           return {
 *               {"arch",   arch()},
 *               {"fields", fields()},
 *               {"record", record},
 *           };
 *       }
 *   };
 * @endcode
 */
class BaseView : public IView {
public:
    // ----------------------------------------------------------
    // Default implementations — override for custom behaviour
    // ----------------------------------------------------------

    /**
     * @brief Render a collection using render() per record.
     *
     * Default wraps each record with render() and assembles:
     * @code
     * { "arch": "...", "fields": {...}, "records": [...], "length": <total> }
     * @endcode
     */
    nlohmann::json renderList(const nlohmann::json& records,
                              int total = 0) const override {
        nlohmann::json result = nlohmann::json::array();
        if (records.is_array()) {
            for (const auto& rec : records)
                result.push_back(render(rec));
        }
        return {
            {"arch",    arch()},
            {"fields",  fields()},
            {"records", std::move(result)},
            {"length",  total > 0 ? total : static_cast<int>(
                            records.is_array() ? records.size() : 0)},
        };
    }

    /**
     * @brief Render a record filtered to a subset of fields.
     *
     * Default calls render() then strips keys not in the filter list.
     * Override for more efficient field projection.
     */
    nlohmann::json renderFields(const nlohmann::json&           record,
                                const std::vector<std::string>& fieldNames) const override {
        if (fieldNames.empty()) return render(record);

        // Project the record JSON down to the requested fields
        nlohmann::json projected = nlohmann::json::object();
        projected["id"] = record.value("id", 0);
        for (const auto& f : fieldNames)
            if (record.contains(f)) projected[f] = record[f];

        return render(projected);
    }
};

} // namespace odoo::core
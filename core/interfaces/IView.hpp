#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace odoo::core {

// ============================================================
// ViewType constants
// ============================================================
/**
 * @brief Standard Odoo view type strings.
 *
 * Use these constants as the viewType argument to ViewFactory::getView()
 * and as the suffix in ViewFactory registration keys.
 *
 * Custom view types (e.g. "map", "cohort", "pivot") are plain strings
 * and do not need to be listed here.
 */
namespace ViewType {
    inline constexpr const char* Form   = "form";
    inline constexpr const char* List   = "list";
    inline constexpr const char* Kanban = "kanban";
    inline constexpr const char* Tree   = "tree";   // alias for list in older Odoo
    inline constexpr const char* Search = "search";
    inline constexpr const char* Graph  = "graph";
    inline constexpr const char* Pivot  = "pivot";
    inline constexpr const char* Calendar = "calendar";
    inline constexpr const char* Activity = "activity";
} // namespace ViewType


// ============================================================
// IView
// ============================================================
/**
 * @brief Interface every JSON view serializer must satisfy.
 *
 * Views are pure JSON shape descriptors. They receive record data that
 * has already been fetched by the ViewModel/Service layer and transform
 * it into an Odoo-compatible JSON payload for the OWL/JS frontend.
 *
 * Design decisions:
 *   - NO template parameter. Views speak nlohmann::json at every boundary,
 *     matching the wire format the frontend already expects. There is no
 *     IView<TModel>, no ViewAdapter, and no type erasure overhead.
 *   - Views are stateless. All input arrives via method parameters.
 *   - Views are Singletons in ViewFactory — one shared instance per key.
 *
 * Key format in ViewFactory: "<model_name>.<view_type>"
 *   e.g. "res.partner.form", "account.move.list", "res.partner.kanban"
 *
 * Concrete views inherit from BaseView, which provides sensible defaults
 * for renderList() and renderFields() built on top of render().
 *
 * Typical call chain:
 * @code
 *   // Inside PartnerViewModel::callKw, method = "web_read"
 *   auto records = partnerService_->read(ids, fields);   // JSON array
 *   auto view    = viewFactory_.getView("res.partner", "form");
 *   return view->render(records[0]);
 * @endcode
 */
class IView {
public:
    virtual ~IView() = default;

    // ----------------------------------------------------------
    // Identity
    // ----------------------------------------------------------

    /**
     * @brief Full view key, e.g. "res.partner.form".
     * Must match the key used at ViewFactory registration.
     */
    virtual std::string viewName() const = 0;

    /**
     * @brief Odoo model name this view belongs to, e.g. "res.partner".
     * Derived from viewName() by default; override if key format differs.
     */
    virtual std::string modelName() const {
        const auto& n = viewName();
        const auto  dot = n.rfind('.');
        return (dot == std::string::npos) ? n : n.substr(0, dot);
    }

    /**
     * @brief View type suffix, e.g. "form", "list", "kanban".
     * Derived from viewName() by default; override if key format differs.
     */
    virtual std::string viewType() const {
        const auto& n = viewName();
        const auto  dot = n.rfind('.');
        return (dot == std::string::npos) ? n : n.substr(dot + 1);
    }

    // ----------------------------------------------------------
    // Architecture descriptor
    // ----------------------------------------------------------

    /**
     * @brief Return the view's XML architecture as a JSON string.
     *
     * Odoo's frontend expects a "arch" field containing the view XML
     * (e.g. <form><field name="name"/>...</form>).
     * This method returns the raw XML string that will be placed in arch.
     *
     * For server-side-only views (API-only use cases), returning an empty
     * string is acceptable.
     */
    virtual std::string arch() const = 0;

    /**
     * @brief Return field metadata for this view's visible fields.
     *
     * Format mirrors Odoo's fields_get() response:
     * @code
     * {
     *   "name":  { "type": "char",    "string": "Name",  "required": true },
     *   "email": { "type": "char",    "string": "Email"                    },
     *   "phone": { "type": "char",    "string": "Phone"                    }
     * }
     * @endcode
     *
     * BaseView delegates this to the model's fieldsGet() filtered to
     * the fields declared in arch(). Concrete views may override to add
     * view-specific attributes (invisible, readonly, required overrides).
     */
    virtual nlohmann::json fields() const = 0;

    // ----------------------------------------------------------
    // Record rendering
    // ----------------------------------------------------------

    /**
     * @brief Render a single record for this view type.
     *
     * @param record  JSON object with field name → value pairs as returned
     *                by IModel::read() or IService read methods.
     * @returns       JSON object shaped for the OWL frontend, typically:
     * @code
     * {
     *   "arch":   "<form>...</form>",
     *   "fields": { ... },       // field metadata
     *   "record": { ... }        // field values
     * }
     * @endcode
     */
    virtual nlohmann::json render(const nlohmann::json& record) const = 0;

    /**
     * @brief Render a collection of records (list / tree / kanban).
     *
     * @param records  JSON array of record objects.
     * @param total    Total match count before pagination (for pager widget).
     * @returns        JSON object, typically:
     * @code
     * {
     *   "arch":    "<list>...</list>",
     *   "fields":  { ... },
     *   "records": [ ... ],
     *   "length":  <total>
     * }
     * @endcode
     */
    virtual nlohmann::json renderList(const nlohmann::json& records,
                                      int total = 0) const = 0;

    /**
     * @brief Render a record filtered to a specific subset of fields.
     *
     * Used by optional field panels, quick-info popovers, and mobile
     * views that show fewer fields than the full form.
     *
     * @param record  JSON record object (may contain more fields than needed).
     * @param fields  Field names to include in the output.
     * @returns       JSON object like render() but scoped to fields.
     */
    virtual nlohmann::json renderFields(const nlohmann::json&           record,
                                        const std::vector<std::string>& fields) const = 0;

    // ----------------------------------------------------------
    // View descriptor (web_load_views RPC)
    // ----------------------------------------------------------

    /**
     * @brief Return the full view descriptor used by web_load_views.
     *
     * The OWL client calls web_load_views once per model/view-type pair.
     * This bundles arch + fields into a single response object.
     *
     * Default implementation composes arch() + fields() — concrete views
     * may override to add extra keys (e.g. "toolbar", "custom_view_id").
     *
     * @returns JSON object:
     * @code
     * {
     *   "id":       0,
     *   "type":     "form",
     *   "model":    "res.partner",
     *   "arch":     "<form>...</form>",
     *   "fields":   { ... },
     *   "toolbar":  {}
     * }
     * @endcode
     */
    virtual nlohmann::json viewDescriptor() const {
        return {
            {"id",      0},
            {"type",    viewType()},
            {"model",   modelName()},
            {"arch",    arch()},
            {"fields",  fields()},
            {"toolbar", nlohmann::json::object()},
        };
    }
};

} // namespace odoo::core
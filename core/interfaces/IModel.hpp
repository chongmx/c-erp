#pragma once
#include "UserContext.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>

namespace odoo::core {

// ============================================================
// IModel
// ============================================================
/**
 * @brief Interface every ORM model must satisfy.
 *
 * Mirrors Odoo's Python model contract:
 *   - Identity    : name(), id()
 *   - Persistence : create(), read(), write(), unlink()
 *   - Introspection: fieldsGet(), toJson(), fromJson()
 *   - Search      : search(), searchRead()
 *
 * All JSON payloads use nlohmann::json and follow Odoo's JSON-RPC
 * wire format so the OWL/JS frontend needs zero adaptation.
 *
 * Concrete models inherit from BaseModel, which provides default
 * implementations backed by DbConnection + FieldRegistry.
 *
 * Lifetime: Transient (one blank instance per request; ModelFactory
 * constructs fresh objects that are then populated via read() or create()).
 */
class IModel {
public:
    virtual ~IModel() = default;

    // ----------------------------------------------------------
    // Record-level authorization (S-30)
    // ----------------------------------------------------------
    /**
     * @brief Set the calling user's context for record-rule filtering.
     *
     * Must be called before any CRUD or search method when record-level
     * authorization is required.  BaseModel stores the context and injects
     * applicable ir.rule WHERE clauses into every subsequent query.
     *
     * GenericViewModel calls this automatically from callKw; custom
     * ViewModels should call it before delegating to the model.
     */
    virtual void setUserContext(const UserContext& ctx) = 0;

    // ----------------------------------------------------------
    // Identity
    // ----------------------------------------------------------

    /**
     * @brief Odoo technical model name, e.g. "res.partner".
     * Must be static-like — same value for all instances of a type.
     */
    virtual std::string name() const = 0;

    /**
     * @brief Database row id, or 0 / nullopt when the record is unsaved.
     */
    virtual int id() const = 0;

    // ----------------------------------------------------------
    // CRUD
    // ----------------------------------------------------------

    /**
     * @brief Persist this record to the database.
     * Sets id() on success.
     * @param values  Field name → value map (JSON object).
     * @returns       New record id.
     * @throws        std::runtime_error on DB error or validation failure.
     */
    virtual int create(const nlohmann::json& values) = 0;

    /**
     * @brief Load field values for a set of ids from the database.
     * @param ids     Record ids to fetch.
     * @param fields  Field names to include; empty = all fields.
     * @returns       Array of JSON objects, one per id.
     */
    virtual nlohmann::json read(const std::vector<int>&         ids,
                                const std::vector<std::string>& fields = {}) = 0;

    /**
     * @brief Update fields on a set of existing records.
     * @param ids     Records to update.
     * @param values  Field name → value map (JSON object).
     * @returns       true on success.
     */
    virtual bool write(const std::vector<int>&  ids,
                       const nlohmann::json&    values) = 0;

    /**
     * @brief Delete records by id.
     * @param ids  Records to delete.
     * @returns    true on success.
     */
    virtual bool unlink(const std::vector<int>& ids) = 0;

    // ----------------------------------------------------------
    // Search
    // ----------------------------------------------------------

    /**
     * @brief Return ids matching domain.
     * @param domain  Odoo domain expression encoded as JSON array.
     *                e.g. [["active","=",true],["name","ilike","acme"]]
     * @param limit   Max results; 0 = no limit.
     * @param offset  Pagination offset.
     * @param order   SQL ORDER BY clause, e.g. "name ASC".
     * @returns       Array of matching ids.
     */
    virtual std::vector<int> search(const nlohmann::json& domain,
                                    int                   limit  = 0,
                                    int                   offset = 0,
                                    const std::string&    order  = "") = 0;

    /**
     * @brief Combined search + read in a single round-trip.
     * @param domain  Domain expression (see search()).
     * @param fields  Field names to include; empty = all fields.
     * @param limit   Max results; 0 = no limit.
     * @param offset  Pagination offset.
     * @param order   SQL ORDER BY clause.
     * @returns       Array of JSON record objects.
     */
    virtual nlohmann::json searchRead(const nlohmann::json&           domain,
                                      const std::vector<std::string>& fields = {},
                                      int                             limit  = 0,
                                      int                             offset = 0,
                                      const std::string&              order  = "") = 0;

    // ----------------------------------------------------------
    // Introspection
    // ----------------------------------------------------------

    /**
     * @brief Return Odoo-compatible fields_get() metadata.
     * @param fields      Subset of field names; empty = all fields.
     * @param attributes  Field attribute names to include (e.g. "string", "type").
     * @returns           JSON object: { field_name: { type, string, ... }, ... }
     */
    virtual nlohmann::json fieldsGet(
        const std::vector<std::string>& fields     = {},
        const std::vector<std::string>& attributes = {}) const = 0;

    /**
     * @brief Serialize this record's current state to JSON.
     * Used by views and services to pass data through the pipeline.
     * @returns JSON object with all currently-loaded field values.
     */
    virtual nlohmann::json toJson() const = 0;

    /**
     * @brief Populate this record's fields from a JSON object.
     * Used when reconstructing a model from a stored/transmitted payload.
     * @param j  JSON object with field name → value pairs.
     */
    virtual void fromJson(const nlohmann::json& j) = 0;

    // ----------------------------------------------------------
    // Counting (convenience, avoids pulling a full search result)
    // ----------------------------------------------------------

    /**
     * @brief Count records matching domain without fetching them.
     * @param domain  Domain expression (see search()).
     * @returns       Match count.
     */
    virtual int searchCount(const nlohmann::json& domain) = 0;
};

} // namespace odoo::core
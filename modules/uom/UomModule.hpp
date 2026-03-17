#pragma once
// =============================================================
// modules/uom/UomModule.hpp
//
// Phase 7 — Units of Measure
//
// Model:  uom.uom  (table: uom_uom)
// Seeds:  15 standard UOM rows (Units, kg, L, Hours, m, …)
// Menus:  Creates "Products" app tile (id=50) with a
//         Configuration section (id=52) → Units of Measure leaf (id=53)
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include "BaseModel.hpp"
#include "BaseView.hpp"
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::uom {

using namespace odoo::infrastructure;
using namespace odoo::core;

// ================================================================
// 1. MODEL
// ================================================================

class UomUom : public BaseModel<UomUom> {
public:
    static constexpr const char* MODEL_NAME = "uom.uom";
    static constexpr const char* TABLE_NAME = "uom_uom";

    explicit UomUom(std::shared_ptr<DbConnection> db)
        : BaseModel(std::move(db))
    {}

    std::string name, uomType, category;
    double      factor   = 1.0;
    double      rounding = 0.01;
    bool        active   = true;

    void registerFields() {
        fieldRegistry_.add({"name",     FieldType::Char,    "Name",      true});
        fieldRegistry_.add({"category", FieldType::Char,    "Category"});
        fieldRegistry_.add({"uom_type", FieldType::Char,    "Type"});
        fieldRegistry_.add({"factor",   FieldType::Float,   "Factor"});
        fieldRegistry_.add({"rounding", FieldType::Float,   "Rounding"});
        fieldRegistry_.add({"active",   FieldType::Boolean, "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]     = name;
        j["category"] = category;
        j["uom_type"] = uomType;
        j["factor"]   = factor;
        j["rounding"] = rounding;
        j["active"]   = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")     && j["name"].is_string())     name     = j["name"].get<std::string>();
        if (j.contains("category") && j["category"].is_string()) category = j["category"].get<std::string>();
        if (j.contains("uom_type") && j["uom_type"].is_string()) uomType  = j["uom_type"].get<std::string>();
        if (j.contains("factor")   && j["factor"].is_number())   factor   = j["factor"].get<double>();
        if (j.contains("rounding") && j["rounding"].is_number()) rounding = j["rounding"].get<double>();
        if (j.contains("active")   && j["active"].is_boolean())  active   = j["active"].get<bool>();
    }

    nlohmann::json toJson() const override {
        nlohmann::json j;
        serializeFields(j);
        j["id"]           = getId();
        j["display_name"] = name;
        return j;
    }

    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("name is required");
        return e;
    }
};


// ================================================================
// 2. VIEWS
// ================================================================

class UomUomListView : public core::BaseView {
public:
    std::string viewName() const override { return "uom.uom.list"; }
    std::string modelName() const override { return "uom.uom"; }
    std::string viewType() const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Units of Measure\">"
               "<field name=\"name\"/>"
               "<field name=\"category\"/>"
               "<field name=\"uom_type\"/>"
               "<field name=\"factor\"/>"
               "<field name=\"active\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",     {{"type","char"},    {"string","Name"}}},
            {"category", {{"type","char"},    {"string","Category"}}},
            {"uom_type", {{"type","char"},    {"string","Type"}}},
            {"factor",   {{"type","float"},   {"string","Factor"}}},
            {"active",   {{"type","boolean"}, {"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class UomUomFormView : public core::BaseView {
public:
    std::string viewName() const override { return "uom.uom.form"; }
    std::string modelName() const override { return "uom.uom"; }
    std::string viewType() const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Unit of Measure\">"
               "<field name=\"name\"/>"
               "<field name=\"category\"/>"
               "<field name=\"uom_type\"/>"
               "<field name=\"factor\"/>"
               "<field name=\"rounding\"/>"
               "<field name=\"active\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",     {{"type","char"},    {"string","Name"}}},
            {"category", {{"type","char"},    {"string","Category"}}},
            {"uom_type", {{"type","char"},    {"string","Type"}}},
            {"factor",   {{"type","float"},   {"string","Factor"}}},
            {"rounding", {{"type","float"},   {"string","Rounding"}}},
            {"active",   {{"type","boolean"}, {"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};


// ================================================================
// 3. MODULE
// ================================================================

class UomModule : public core::IModule {
public:
    explicit UomModule(core::ModelFactory&     models,
                       core::ServiceFactory&   services,
                       core::ViewModelFactory& viewModels,
                       core::ViewFactory&      views)
        : models_(models), services_(services),
          viewModels_(viewModels), views_(views)
    {}

    static constexpr const char* staticModuleName() { return "uom"; }
    std::string moduleName() const override { return "uom"; }

    void registerModels() override {
        auto db = services_.db();
        models_.registerCreator("uom.uom", [db]{ return std::make_shared<UomUom>(db); });
    }

    void registerServices() override {}

    void registerViewModels() override {
        auto db = services_.db();
        viewModels_.registerCreator("uom.uom", [db]{
            return std::make_shared<GenericViewModel<UomUom>>(db);
        });
    }

    void registerViews() override {
        views_.registerCreator("uom.uom.list", []{ return std::make_shared<UomUomListView>(); });
        views_.registerCreator("uom.uom.form", []{ return std::make_shared<UomUomFormView>(); });
    }

    void registerRoutes() override {}

    void initialize() override {
        ensureSchema_();
        seedUom_();
        seedMenus_();
    }

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    // ----------------------------------------------------------
    // Schema
    // ----------------------------------------------------------
    void ensureSchema_() {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS uom_uom (
                id          SERIAL PRIMARY KEY,
                name        VARCHAR NOT NULL,
                category    VARCHAR NOT NULL DEFAULT 'Unit',
                uom_type    VARCHAR NOT NULL DEFAULT 'reference',
                factor      NUMERIC(12,6) NOT NULL DEFAULT 1.0,
                rounding    NUMERIC(12,6) NOT NULL DEFAULT 0.01,
                active      BOOLEAN NOT NULL DEFAULT TRUE,
                create_date TIMESTAMP DEFAULT now(),
                write_date  TIMESTAMP DEFAULT now()
            )
        )");
        txn.commit();
    }

    // ----------------------------------------------------------
    // Seeds — 15 standard UOM rows
    // ----------------------------------------------------------
    void seedUom_() {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(R"(
            INSERT INTO uom_uom (id, name, category, uom_type, factor, rounding) VALUES
                (1,  'Units',   'Unit',   'reference', 1.0,    0.01),
                (2,  'Dozens',  'Unit',   'bigger',    12.0,   0.01),
                (3,  'kg',      'Weight', 'reference', 1.0,    0.001),
                (4,  'g',       'Weight', 'smaller',   0.001,  0.001),
                (5,  'Ton',     'Weight', 'bigger',    1000.0, 0.001),
                (6,  'L',       'Volume', 'reference', 1.0,    0.001),
                (7,  'ml',      'Volume', 'smaller',   0.001,  0.001),
                (8,  'm3',      'Volume', 'bigger',    1000.0, 0.001),
                (9,  'Hours',   'Time',   'reference', 1.0,    0.01),
                (10, 'Days',    'Time',   'bigger',    8.0,    0.01),
                (11, 'Minutes', 'Time',   'smaller',   0.01667,0.01),
                (12, 'm',       'Length', 'reference', 1.0,    0.01),
                (13, 'cm',      'Length', 'smaller',   0.01,   0.01),
                (14, 'km',      'Length', 'bigger',    1000.0, 0.01),
                (15, 'm2',      'Area',   'reference', 1.0,    0.01)
            ON CONFLICT (id) DO NOTHING
        )");
        txn.exec("SELECT setval('uom_uom_id_seq', (SELECT MAX(id) FROM uom_uom), true)");

        // ir_act_window for uom.uom
        txn.exec(R"(
            INSERT INTO ir_act_window (id, name, res_model, view_mode, path, context) VALUES
                (8, 'Units of Measure', 'uom.uom', 'list,form', 'uom', '{}')
            ON CONFLICT (id) DO NOTHING
        )");
        txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
        txn.commit();
    }

    // ----------------------------------------------------------
    // Menus — creates "Products" app tile + Configuration section
    // ----------------------------------------------------------
    void seedMenus_() {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};

        // Level 0: Products app tile
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id, web_icon) VALUES
                (50, 'Products', NULL, 50, NULL, 'products')
            ON CONFLICT (id) DO NOTHING
        )");

        // Level 1: Configuration section (no action — will have dropdown)
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
                (52, 'Configuration', 50, 20, NULL)
            ON CONFLICT (id) DO NOTHING
        )");

        // Level 2: Units of Measure under Configuration
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
                (53, 'Units of Measure', 52, 10, 8)
            ON CONFLICT (id) DO NOTHING
        )");

        txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");
        txn.commit();
    }
};

} // namespace odoo::modules::uom

// =============================================================
// modules/mrp/MrpModule.cpp
// =============================================================
#include "MrpModule.hpp"
#include "BaseModel.hpp"
#include "BaseView.hpp"
#include "BaseViewModel.hpp"
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include "AuditService.hpp"
#include "IrSequence.hpp"
#include "StockQuant.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace odoo::modules::mrp {

using namespace odoo::infrastructure;
using namespace odoo::core;

// ── helpers ─────────────────────────────────────────────────
static int mrpM2oId(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer()) return v[0].get<int>();
    if (v.is_string()) { try { return std::stoi(v.get<std::string>()); } catch (...) {} }
    return 0;
}

// ================================================================
// 1. MODELS
// ================================================================

// ----------------------------------------------------------------
// MrpBom — mrp.bom
// ----------------------------------------------------------------
class MrpBom : public BaseModel<MrpBom> {
public:
    static constexpr const char* MODEL_NAME = "mrp.bom";
    static constexpr const char* TABLE_NAME = "mrp_bom";

    int         productId      = 0;
    std::string code;          // reference / internal reference
    std::string bomType        = "normal";  // normal | phantom | subcontract
    double      productQty     = 1.0;
    int         productUomId   = 0;
    int         companyId      = 0;
    int         subcontractorId = 0;         // res.partner, when bom_type='subcontract'
    bool        active         = true;

    explicit MrpBom(std::shared_ptr<DbConnection> db)
        : BaseModel<MrpBom>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"product_id",     FieldType::Many2one, "Product",       true,  false, true, false, "product.product"});
        fieldRegistry_.add({"code",           FieldType::Char,    "Reference"});
        fieldRegistry_.add({"bom_type",       FieldType::Char,    "BOM Type"});
        fieldRegistry_.add({"product_qty",    FieldType::Float,   "Quantity"});
        fieldRegistry_.add({"product_uom_id", FieldType::Many2one,"Unit of Measure",false, false, true, false, "uom.uom"});
        fieldRegistry_.add({"company_id",     FieldType::Many2one,"Company",         false, false, true, false, "res.company"});
        fieldRegistry_.add({"subcontractor_id",FieldType::Many2one,"Subcontractor",  false, false, true, false, "res.partner"});
        fieldRegistry_.add({"active",         FieldType::Boolean, "Active"});
        fieldRegistry_.markScaled({"product_qty"});   // P2: migration 972
    }

    void serializeFields(nlohmann::json& j) const override {
        j["product_id"]     = productId    > 0 ? nlohmann::json(productId)    : nlohmann::json(false);
        j["code"]           = code;
        j["bom_type"]       = bomType;
        j["product_qty"]    = productQty;
        j["product_uom_id"]  = productUomId > 0 ? nlohmann::json(productUomId) : nlohmann::json(false);
        j["company_id"]      = companyId    > 0 ? nlohmann::json(companyId)    : nlohmann::json(false);
        j["subcontractor_id"]= subcontractorId > 0 ? nlohmann::json(subcontractorId) : nlohmann::json(false);
        j["active"]          = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("product_id"))     productId    = mrpM2oId(j["product_id"]);
        if (j.contains("code")           && j["code"].is_string())           code        = j["code"].get<std::string>();
        if (j.contains("bom_type")       && j["bom_type"].is_string())       bomType     = j["bom_type"].get<std::string>();
        if (j.contains("product_qty")    && j["product_qty"].is_number())    productQty  = j["product_qty"].get<double>();
        if (j.contains("product_uom_id")) productUomId = mrpM2oId(j["product_uom_id"]);
        if (j.contains("company_id"))     companyId    = mrpM2oId(j["company_id"]);
        if (j.contains("subcontractor_id")) subcontractorId = mrpM2oId(j["subcontractor_id"]);
        if (j.contains("active")         && j["active"].is_boolean())        active      = j["active"].get<bool>();
    }

    nlohmann::json toJson() const override {
        nlohmann::json j;
        serializeFields(j);
        j["id"] = getId();
        j["display_name"] = code.empty() ? ("BOM #" + std::to_string(getId())) : code;
        return j;
    }

    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (productId <= 0) e.push_back("product_id is required");
        return e;
    }
};

// ----------------------------------------------------------------
// MrpBomLine — mrp.bom.line
// ----------------------------------------------------------------
class MrpBomLine : public BaseModel<MrpBomLine> {
public:
    static constexpr const char* MODEL_NAME = "mrp.bom.line";
    static constexpr const char* TABLE_NAME = "mrp_bom_line";

    int    bomId        = 0;
    int    productId    = 0;
    double productQty   = 1.0;
    int    productUomId = 0;
    int    sequence     = 10;

    explicit MrpBomLine(std::shared_ptr<DbConnection> db)
        : BaseModel<MrpBomLine>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"bom_id",         FieldType::Many2one, "BOM",            true,  false, true, false, "mrp.bom"});
        fieldRegistry_.add({"product_id",     FieldType::Many2one, "Component",      true,  false, true, false, "product.product"});
        fieldRegistry_.add({"product_qty",    FieldType::Float,   "Quantity"});
        fieldRegistry_.add({"product_uom_id", FieldType::Many2one,"Unit of Measure", false, false, true, false, "uom.uom"});
        fieldRegistry_.add({"sequence",       FieldType::Integer, "Sequence"});
        fieldRegistry_.markScaled({"product_qty"});   // P2: migration 960
    }

    void serializeFields(nlohmann::json& j) const override {
        j["bom_id"]         = bomId        > 0 ? nlohmann::json(bomId)        : nlohmann::json(false);
        j["product_id"]     = productId    > 0 ? nlohmann::json(productId)    : nlohmann::json(false);
        j["product_qty"]    = productQty;
        j["product_uom_id"] = productUomId > 0 ? nlohmann::json(productUomId) : nlohmann::json(false);
        j["sequence"]       = sequence;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("bom_id"))         bomId        = mrpM2oId(j["bom_id"]);
        if (j.contains("product_id"))     productId    = mrpM2oId(j["product_id"]);
        if (j.contains("product_qty")    && j["product_qty"].is_number())   productQty   = j["product_qty"].get<double>();
        if (j.contains("product_uom_id")) productUomId = mrpM2oId(j["product_uom_id"]);
        if (j.contains("sequence")       && j["sequence"].is_number())      sequence     = j["sequence"].get<int>();
    }

    nlohmann::json toJson() const override {
        nlohmann::json j;
        serializeFields(j);
        j["id"] = getId();
        return j;
    }

    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (bomId     <= 0) e.push_back("bom_id is required");
        if (productId <= 0) e.push_back("product_id is required");
        return e;
    }
};

// ----------------------------------------------------------------
// MrpWorkcenter — mrp.workcenter (a machine / station)
// ----------------------------------------------------------------
class MrpWorkcenter : public BaseModel<MrpWorkcenter> {
public:
    static constexpr const char* MODEL_NAME = "mrp.workcenter";
    static constexpr const char* TABLE_NAME = "mrp_workcenter";

    std::string name;
    std::string code;
    double      costsHour      = 0.0;
    double      timeEfficiency = 100.0;
    double      capacity       = 1.0;
    int         companyId      = 0;
    bool        active         = true;

    explicit MrpWorkcenter(std::shared_ptr<DbConnection> db)
        : BaseModel<MrpWorkcenter>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",            FieldType::Char,    "Work Center", true});
        fieldRegistry_.add({"code",            FieldType::Char,    "Code"});
        fieldRegistry_.add({"costs_hour",      FieldType::Monetary,"Cost per Hour"});
        fieldRegistry_.add({"time_efficiency", FieldType::Float,   "Efficiency (%)"});
        fieldRegistry_.add({"capacity",        FieldType::Float,   "Capacity"});
        fieldRegistry_.add({"company_id",      FieldType::Many2one,"Company", false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",          FieldType::Boolean, "Active"});
        fieldRegistry_.markScaled({"costs_hour"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]            = name;
        j["code"]            = code.empty() ? nlohmann::json(false) : nlohmann::json(code);
        j["costs_hour"]      = costsHour;
        j["time_efficiency"] = timeEfficiency;
        j["capacity"]        = capacity;
        j["company_id"]      = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["active"]          = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")            && j["name"].is_string())            name = j["name"].get<std::string>();
        if (j.contains("code")            && j["code"].is_string())            code = j["code"].get<std::string>();
        if (j.contains("costs_hour")      && j["costs_hour"].is_number())      costsHour = j["costs_hour"].get<double>();
        if (j.contains("time_efficiency") && j["time_efficiency"].is_number()) timeEfficiency = j["time_efficiency"].get<double>();
        if (j.contains("capacity")        && j["capacity"].is_number())        capacity = j["capacity"].get<double>();
        if (j.contains("active")          && j["active"].is_boolean())         active = j["active"].get<bool>();
        if (j.contains("company_id")) companyId = mrpM2oId(j["company_id"]);
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Work center name is required");
        return e;
    }
};

// ----------------------------------------------------------------
// MrpRoutingWorkcenter — mrp.routing.workcenter (a BOM operation)
// Odoo 14 attaches operations directly to the BOM; there is no
// standalone routing model. Each row is one step, on one work center.
// ----------------------------------------------------------------
class MrpRoutingWorkcenter : public BaseModel<MrpRoutingWorkcenter> {
public:
    static constexpr const char* MODEL_NAME = "mrp.routing.workcenter";
    static constexpr const char* TABLE_NAME = "mrp_routing_workcenter";

    int         bomId           = 0;
    int         workcenterId    = 0;
    std::string name;
    int         sequence        = 10;
    double      timeCycleManual = 60.0;   // minutes per unit
    int         companyId       = 0;

    explicit MrpRoutingWorkcenter(std::shared_ptr<DbConnection> db)
        : BaseModel<MrpRoutingWorkcenter>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"bom_id",            FieldType::Many2one,"BOM",          true,  false, true, false, "mrp.bom"});
        fieldRegistry_.add({"workcenter_id",     FieldType::Many2one,"Work Center",  true,  false, true, false, "mrp.workcenter"});
        fieldRegistry_.add({"name",              FieldType::Char,    "Operation",    true});
        fieldRegistry_.add({"sequence",          FieldType::Integer, "Sequence"});
        fieldRegistry_.add({"time_cycle_manual", FieldType::Float,   "Duration (min)"});
        fieldRegistry_.add({"company_id",        FieldType::Many2one,"Company",      false, false, true, false, "res.company"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["bom_id"]            = bomId        > 0 ? nlohmann::json(bomId)        : nlohmann::json(false);
        j["workcenter_id"]     = workcenterId > 0 ? nlohmann::json(workcenterId) : nlohmann::json(false);
        j["name"]              = name;
        j["sequence"]          = sequence;
        j["time_cycle_manual"] = timeCycleManual;
        j["company_id"]        = companyId    > 0 ? nlohmann::json(companyId)    : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("bom_id"))        bomId        = mrpM2oId(j["bom_id"]);
        if (j.contains("workcenter_id")) workcenterId = mrpM2oId(j["workcenter_id"]);
        if (j.contains("name")              && j["name"].is_string())              name = j["name"].get<std::string>();
        if (j.contains("sequence")          && j["sequence"].is_number())          sequence = j["sequence"].get<int>();
        if (j.contains("time_cycle_manual") && j["time_cycle_manual"].is_number()) timeCycleManual = j["time_cycle_manual"].get<double>();
        if (j.contains("company_id")) companyId = mrpM2oId(j["company_id"]);
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (bomId <= 0)        e.push_back("bom_id is required");
        if (name.empty())      e.push_back("Operation name is required");
        return e;
    }
};

// ----------------------------------------------------------------
// MrpProduction — mrp.production (a Manufacturing Order)
// ----------------------------------------------------------------
class MrpProduction : public BaseModel<MrpProduction> {
public:
    static constexpr const char* MODEL_NAME = "mrp.production";
    static constexpr const char* TABLE_NAME = "mrp_production";

    std::string name           = "New";
    int         productId      = 0;
    double      productQty     = 1.0;
    int         productUomId   = 0;
    int         bomId          = 0;
    std::string state          = "draft"; // draft|confirmed|progress|to_close|done|cancel
    int         locationSrcId  = 0;
    int         locationDestId = 0;
    std::string datePlannedStart;
    double      qtyProducing   = 0.0;
    std::string origin;
    int         userId         = 0;
    int         companyId      = 0;

    explicit MrpProduction(std::shared_ptr<DbConnection> db)
        : BaseModel<MrpProduction>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",               FieldType::Char,    "Reference"});
        fieldRegistry_.add({"product_id",         FieldType::Many2one,"Product",   true,  false, true, false, "product.product"});
        fieldRegistry_.add({"product_qty",        FieldType::Float,   "Quantity"});
        fieldRegistry_.add({"product_uom_id",     FieldType::Many2one,"Unit of Measure", false, false, true, false, "uom.uom"});
        fieldRegistry_.add({"bom_id",             FieldType::Many2one,"Bill of Materials", false, false, true, false, "mrp.bom"});
        fieldRegistry_.add({"state",              FieldType::Char,    "Status"});
        fieldRegistry_.add({"location_src_id",    FieldType::Many2one,"Components Location", false, false, true, false, "stock.location"});
        fieldRegistry_.add({"location_dest_id",   FieldType::Many2one,"Finished Location",   false, false, true, false, "stock.location"});
        fieldRegistry_.add({"date_planned_start", FieldType::Datetime,"Planned Date"});
        fieldRegistry_.add({"qty_producing",      FieldType::Float,   "Quantity Producing"});
        fieldRegistry_.add({"origin",             FieldType::Char,    "Source Document"});
        fieldRegistry_.add({"user_id",            FieldType::Many2one,"Responsible", false, false, true, false, "res.users"});
        fieldRegistry_.add({"company_id",         FieldType::Many2one,"Company",     false, false, true, false, "res.company"});
        fieldRegistry_.markScaled({"product_qty", "qty_producing"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]               = name;
        j["product_id"]         = productId      > 0 ? nlohmann::json(productId)      : nlohmann::json(false);
        j["product_qty"]        = productQty;
        j["product_uom_id"]     = productUomId   > 0 ? nlohmann::json(productUomId)   : nlohmann::json(false);
        j["bom_id"]             = bomId          > 0 ? nlohmann::json(bomId)          : nlohmann::json(false);
        j["state"]              = state;
        j["location_src_id"]    = locationSrcId  > 0 ? nlohmann::json(locationSrcId)  : nlohmann::json(false);
        j["location_dest_id"]   = locationDestId > 0 ? nlohmann::json(locationDestId) : nlohmann::json(false);
        j["date_planned_start"] = datePlannedStart.empty() ? nlohmann::json(false) : nlohmann::json(datePlannedStart);
        j["qty_producing"]      = qtyProducing;
        j["origin"]             = origin.empty() ? nlohmann::json(false) : nlohmann::json(origin);
        j["user_id"]            = userId         > 0 ? nlohmann::json(userId)         : nlohmann::json(false);
        j["company_id"]         = companyId      > 0 ? nlohmann::json(companyId)      : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")               && j["name"].is_string())               name = j["name"].get<std::string>();
        if (j.contains("state")              && j["state"].is_string())              state = j["state"].get<std::string>();
        if (j.contains("date_planned_start") && j["date_planned_start"].is_string()) datePlannedStart = j["date_planned_start"].get<std::string>();
        if (j.contains("origin")             && j["origin"].is_string())             origin = j["origin"].get<std::string>();
        if (j.contains("product_qty")        && j["product_qty"].is_number())        productQty = j["product_qty"].get<double>();
        if (j.contains("qty_producing")      && j["qty_producing"].is_number())      qtyProducing = j["qty_producing"].get<double>();
        if (j.contains("product_id"))       productId      = mrpM2oId(j["product_id"]);
        if (j.contains("product_uom_id"))   productUomId   = mrpM2oId(j["product_uom_id"]);
        if (j.contains("bom_id"))           bomId          = mrpM2oId(j["bom_id"]);
        if (j.contains("location_src_id"))  locationSrcId  = mrpM2oId(j["location_src_id"]);
        if (j.contains("location_dest_id")) locationDestId = mrpM2oId(j["location_dest_id"]);
        if (j.contains("user_id"))          userId         = mrpM2oId(j["user_id"]);
        if (j.contains("company_id"))       companyId      = mrpM2oId(j["company_id"]);
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (productId <= 0) e.push_back("Product is required");
        return e;
    }
};

// ----------------------------------------------------------------
// MrpWorkorder — mrp.workorder (one execution step of an MO)
// ----------------------------------------------------------------
class MrpWorkorder : public BaseModel<MrpWorkorder> {
public:
    static constexpr const char* MODEL_NAME = "mrp.workorder";
    static constexpr const char* TABLE_NAME = "mrp_workorder";

    int         productionId     = 0;
    int         workcenterId     = 0;
    int         operationId      = 0;
    std::string name;
    int         sequence         = 10;
    std::string state            = "pending"; // pending|ready|progress|done|cancel
    double      durationExpected = 0.0;       // minutes
    double      duration         = 0.0;       // minutes actually logged
    double      qtyProduced      = 0.0;
    std::string dateStart;
    std::string dateFinished;
    int         companyId        = 0;

    explicit MrpWorkorder(std::shared_ptr<DbConnection> db)
        : BaseModel<MrpWorkorder>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"production_id",     FieldType::Many2one,"Manufacturing Order", true, false, true, false, "mrp.production"});
        fieldRegistry_.add({"workcenter_id",     FieldType::Many2one,"Work Center", false, false, true, false, "mrp.workcenter"});
        fieldRegistry_.add({"operation_id",      FieldType::Many2one,"Operation",   false, false, true, false, "mrp.routing.workcenter"});
        fieldRegistry_.add({"name",              FieldType::Char,    "Operation",   true});
        fieldRegistry_.add({"sequence",          FieldType::Integer, "Sequence"});
        fieldRegistry_.add({"state",             FieldType::Char,    "Status"});
        fieldRegistry_.add({"duration_expected", FieldType::Float,   "Expected Duration (min)"});
        fieldRegistry_.add({"duration",          FieldType::Float,   "Real Duration (min)"});
        fieldRegistry_.add({"qty_produced",      FieldType::Float,   "Quantity Produced"});
        fieldRegistry_.add({"date_start",        FieldType::Datetime,"Start Date"});
        fieldRegistry_.add({"date_finished",     FieldType::Datetime,"End Date"});
        fieldRegistry_.add({"company_id",        FieldType::Many2one,"Company", false, false, true, false, "res.company"});
        fieldRegistry_.markScaled({"qty_produced"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["production_id"]     = productionId > 0 ? nlohmann::json(productionId) : nlohmann::json(false);
        j["workcenter_id"]     = workcenterId > 0 ? nlohmann::json(workcenterId) : nlohmann::json(false);
        j["operation_id"]      = operationId  > 0 ? nlohmann::json(operationId)  : nlohmann::json(false);
        j["name"]              = name;
        j["sequence"]          = sequence;
        j["state"]             = state;
        j["duration_expected"] = durationExpected;
        j["duration"]          = duration;
        j["qty_produced"]      = qtyProduced;
        j["date_start"]        = dateStart.empty()    ? nlohmann::json(false) : nlohmann::json(dateStart);
        j["date_finished"]     = dateFinished.empty() ? nlohmann::json(false) : nlohmann::json(dateFinished);
        j["company_id"]        = companyId    > 0 ? nlohmann::json(companyId)    : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("production_id")) productionId = mrpM2oId(j["production_id"]);
        if (j.contains("workcenter_id")) workcenterId = mrpM2oId(j["workcenter_id"]);
        if (j.contains("operation_id"))  operationId  = mrpM2oId(j["operation_id"]);
        if (j.contains("name")              && j["name"].is_string())              name = j["name"].get<std::string>();
        if (j.contains("state")             && j["state"].is_string())             state = j["state"].get<std::string>();
        if (j.contains("sequence")          && j["sequence"].is_number())          sequence = j["sequence"].get<int>();
        if (j.contains("duration_expected") && j["duration_expected"].is_number()) durationExpected = j["duration_expected"].get<double>();
        if (j.contains("duration")          && j["duration"].is_number())          duration = j["duration"].get<double>();
        if (j.contains("qty_produced")      && j["qty_produced"].is_number())      qtyProduced = j["qty_produced"].get<double>();
        if (j.contains("date_start")        && j["date_start"].is_string())        dateStart = j["date_start"].get<std::string>();
        if (j.contains("date_finished")     && j["date_finished"].is_string())     dateFinished = j["date_finished"].get<std::string>();
        if (j.contains("company_id")) companyId = mrpM2oId(j["company_id"]);
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (productionId <= 0) e.push_back("production_id is required");
        if (name.empty())      e.push_back("Operation name is required");
        return e;
    }
};

// ----------------------------------------------------------------
// MrpProductionSchedule — mrp.production.schedule (an MPS row / product)
// ----------------------------------------------------------------
class MrpProductionSchedule : public BaseModel<MrpProductionSchedule> {
public:
    static constexpr const char* MODEL_NAME = "mrp.production.schedule";
    static constexpr const char* TABLE_NAME = "mrp_production_schedule";

    int    productId      = 0;
    double minToReplenish = 0.0;
    int    companyId      = 0;

    explicit MrpProductionSchedule(std::shared_ptr<DbConnection> db)
        : BaseModel<MrpProductionSchedule>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"product_id",       FieldType::Many2one,"Product", true, false, true, false, "product.product"});
        fieldRegistry_.add({"min_to_replenish", FieldType::Float,   "Safety Stock"});
        fieldRegistry_.add({"company_id",        FieldType::Many2one,"Company", false, false, true, false, "res.company"});
        fieldRegistry_.markScaled({"min_to_replenish"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["product_id"]       = productId > 0 ? nlohmann::json(productId) : nlohmann::json(false);
        j["min_to_replenish"] = minToReplenish;
        j["company_id"]       = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("product_id")) productId = mrpM2oId(j["product_id"]);
        if (j.contains("min_to_replenish") && j["min_to_replenish"].is_number()) minToReplenish = j["min_to_replenish"].get<double>();
        if (j.contains("company_id")) companyId = mrpM2oId(j["company_id"]);
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (productId <= 0) e.push_back("Product is required");
        return e;
    }
};

// ----------------------------------------------------------------
// MrpForecast — mrp.forecast (one period's forecasted demand)
// ----------------------------------------------------------------
class MrpForecast : public BaseModel<MrpForecast> {
public:
    static constexpr const char* MODEL_NAME = "mrp.forecast";
    static constexpr const char* TABLE_NAME = "mrp_forecast";

    int         productId   = 0;
    std::string date;
    double      forecastQty = 0.0;
    int         companyId   = 0;

    explicit MrpForecast(std::shared_ptr<DbConnection> db)
        : BaseModel<MrpForecast>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"product_id",   FieldType::Many2one,"Product", true, false, true, false, "product.product"});
        fieldRegistry_.add({"date",         FieldType::Date,    "Period"});
        fieldRegistry_.add({"forecast_qty", FieldType::Float,   "Forecasted Demand"});
        fieldRegistry_.add({"company_id",    FieldType::Many2one,"Company", false, false, true, false, "res.company"});
        fieldRegistry_.markScaled({"forecast_qty"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["product_id"]   = productId > 0 ? nlohmann::json(productId) : nlohmann::json(false);
        j["date"]         = date.empty() ? nlohmann::json(false) : nlohmann::json(date);
        j["forecast_qty"] = forecastQty;
        j["company_id"]   = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("product_id")) productId = mrpM2oId(j["product_id"]);
        if (j.contains("date") && j["date"].is_string()) date = j["date"].get<std::string>();
        if (j.contains("forecast_qty") && j["forecast_qty"].is_number()) forecastQty = j["forecast_qty"].get<double>();
        if (j.contains("company_id")) companyId = mrpM2oId(j["company_id"]);
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (productId <= 0) e.push_back("Product is required");
        if (date.empty())   e.push_back("Period is required");
        return e;
    }
};

// ================================================================
// 2. VIEWS
// ================================================================

class MrpBomListView : public core::BaseView {
public:
    std::string viewName() const override { return "mrp.bom.list"; }
    std::string modelName() const override { return "mrp.bom"; }
    std::string viewType() const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Bills of Materials\">"
               "<field name=\"product_id\"/>"
               "<field name=\"code\"/>"
               "<field name=\"bom_type\"/>"
               "<field name=\"product_qty\"/>"
               "<field name=\"product_uom_id\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"product_id",     {{"type","many2one"}, {"string","Product"},        {"relation","product.product"}}},
            {"code",           {{"type","char"},     {"string","Reference"}}},
            {"bom_type",       {{"type","char"},     {"string","BOM Type"}}},
            {"product_qty",    {{"type","float"},    {"string","Quantity"}}},
            {"product_uom_id", {{"type","many2one"}, {"string","Unit of Measure"},{"relation","uom.uom"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class MrpBomFormView : public core::BaseView {
public:
    std::string viewName() const override { return "mrp.bom.form"; }
    std::string modelName() const override { return "mrp.bom"; }
    std::string viewType() const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Bill of Materials\">"
               "<field name=\"product_id\"/>"
               "<field name=\"code\"/>"
               "<field name=\"bom_type\"/>"
               "<field name=\"product_qty\"/>"
               "<field name=\"product_uom_id\"/>"
               "<field name=\"active\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"product_id",     {{"type","many2one"}, {"string","Product"},        {"relation","product.product"}}},
            {"code",           {{"type","char"},     {"string","Reference"}}},
            {"bom_type",       {{"type","char"},     {"string","BOM Type"}}},
            {"product_qty",    {{"type","float"},    {"string","Quantity"}}},
            {"product_uom_id", {{"type","many2one"}, {"string","Unit of Measure"},{"relation","uom.uom"}}},
            {"active",         {{"type","boolean"},  {"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class MrpProductionListView : public core::BaseView {
public:
    std::string viewName() const override { return "mrp.production.list"; }
    std::string modelName() const override { return "mrp.production"; }
    std::string viewType() const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Manufacturing Orders\">"
               "<field name=\"name\"/>"
               "<field name=\"product_id\"/>"
               "<field name=\"product_qty\"/>"
               "<field name=\"date_planned_start\"/>"
               "<field name=\"state\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",               {{"type","char"},      {"string","Reference"}}},
            {"product_id",         {{"type","many2one"},  {"string","Product"}, {"relation","product.product"}}},
            {"product_qty",        {{"type","float"},     {"string","Quantity"}}},
            {"date_planned_start", {{"type","datetime"},  {"string","Planned Date"}}},
            {"state",              {{"type","selection"}, {"string","Status"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class MrpProductionFormView : public core::BaseView {
public:
    std::string viewName() const override { return "mrp.production.form"; }
    std::string modelName() const override { return "mrp.production"; }
    std::string viewType() const override { return "form"; }
    std::string arch() const override { return "<form string=\"Manufacturing Order\"/>"; }
    nlohmann::json fields() const override {
        return {
            {"name",               {{"type","char"},      {"string","Reference"}}},
            {"product_id",         {{"type","many2one"},  {"string","Product"},           {"relation","product.product"}}},
            {"product_qty",        {{"type","float"},     {"string","Quantity"}}},
            {"bom_id",             {{"type","many2one"},  {"string","Bill of Materials"}, {"relation","mrp.bom"}}},
            {"location_src_id",    {{"type","many2one"},  {"string","Components Location"},{"relation","stock.location"}}},
            {"location_dest_id",   {{"type","many2one"},  {"string","Finished Location"}, {"relation","stock.location"}}},
            {"date_planned_start", {{"type","datetime"},  {"string","Planned Date"}}},
            {"state",              {{"type","selection"}, {"string","Status"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class MrpWorkcenterListView : public core::BaseView {
public:
    std::string viewName() const override { return "mrp.workcenter.list"; }
    std::string modelName() const override { return "mrp.workcenter"; }
    std::string viewType() const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Work Centers\">"
               "<field name=\"name\"/>"
               "<field name=\"code\"/>"
               "<field name=\"costs_hour\"/>"
               "<field name=\"time_efficiency\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",            {{"type","char"},     {"string","Work Center"}}},
            {"code",            {{"type","char"},     {"string","Code"}}},
            {"costs_hour",      {{"type","monetary"}, {"string","Cost per Hour"}}},
            {"time_efficiency", {{"type","float"},    {"string","Efficiency (%)"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class MrpWorkcenterFormView : public core::BaseView {
public:
    std::string viewName() const override { return "mrp.workcenter.form"; }
    std::string modelName() const override { return "mrp.workcenter"; }
    std::string viewType() const override { return "form"; }
    std::string arch() const override { return "<form string=\"Work Center\"/>"; }
    nlohmann::json fields() const override {
        return {
            {"name",            {{"type","char"},     {"string","Work Center"}}},
            {"code",            {{"type","char"},     {"string","Code"}}},
            {"costs_hour",      {{"type","monetary"}, {"string","Cost per Hour"}}},
            {"time_efficiency", {{"type","float"},    {"string","Efficiency (%)"}}},
            {"capacity",        {{"type","float"},    {"string","Capacity"}}},
            {"active",          {{"type","boolean"},  {"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class MrpWorkorderListView : public core::BaseView {
public:
    std::string viewName() const override { return "mrp.workorder.list"; }
    std::string modelName() const override { return "mrp.workorder"; }
    std::string viewType() const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Work Orders\">"
               "<field name=\"production_id\"/>"
               "<field name=\"name\"/>"
               "<field name=\"workcenter_id\"/>"
               "<field name=\"duration_expected\"/>"
               "<field name=\"state\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"production_id",     {{"type","many2one"},  {"string","Manufacturing Order"}, {"relation","mrp.production"}}},
            {"name",              {{"type","char"},      {"string","Operation"}}},
            {"workcenter_id",     {{"type","many2one"},  {"string","Work Center"}, {"relation","mrp.workcenter"}}},
            {"duration_expected", {{"type","float"},     {"string","Expected Duration (min)"}}},
            {"state",             {{"type","selection"}, {"string","Status"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class MrpProductionScheduleListView : public core::BaseView {
public:
    std::string viewName() const override { return "mrp.production.schedule.list"; }
    std::string modelName() const override { return "mrp.production.schedule"; }
    std::string viewType() const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Master Production Schedule\">"
               "<field name=\"product_id\"/>"
               "<field name=\"on_hand\"/>"
               "<field name=\"min_to_replenish\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"product_id",       {{"type","many2one"}, {"string","Product"}, {"relation","product.product"}}},
            {"on_hand",          {{"type","float"},    {"string","On Hand"}}},
            {"min_to_replenish", {{"type","float"},    {"string","Safety Stock"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ================================================================
// 3. VIEWMODELS
// ================================================================

// ----------------------------------------------------------------
// MrpBomViewModel
// ----------------------------------------------------------------
class MrpBomViewModel : public BaseViewModel {
public:
    explicit MrpBomViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",  handleSearchRead)
        REGISTER_METHOD("read",         handleRead)
        REGISTER_MUTATOR("create",       handleCreate)
        REGISTER_MUTATOR("write",        handleWrite)
        REGISTER_MUTATOR("unlink",       handleUnlink)
        REGISTER_METHOD("fields_get",   handleFieldsGet)
        REGISTER_METHOD("search_count", handleSearchCount)
        REGISTER_METHOD("search",       handleSearch)
    }

    std::string modelName() const override { return "mrp.bom"; }

private:
    std::shared_ptr<DbConnection> db_;

    // search_read: join product_product to get display name
    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        std::string sql = R"(
            SELECT b.id,
                   b.product_id,
                   p.name AS product_name,
                   b.code,
                   b.bom_type,
                   b.product_qty,
                   b.product_uom_id,
                   u.name AS uom_name,
                   b.active
            FROM mrp_bom b
            LEFT JOIN product_product p ON p.id = b.product_id
            LEFT JOIN uom_uom         u ON u.id = b.product_uom_id
            WHERE b.active = TRUE
            ORDER BY b.id
        )";

        int lim = call.limit() > 0 ? call.limit() : 80;
        int off = call.offset();
        sql += " LIMIT " + std::to_string(lim) + " OFFSET " + std::to_string(off);

        auto res = txn.exec(sql);
        txn.commit();

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]             = row["id"].as<int>();
            j["product_id"]     = row["product_id"].is_null()
                                    ? nlohmann::json(false)
                                    : nlohmann::json::array({row["product_id"].as<int>(),
                                                             row["product_name"].c_str()});
            j["code"]           = row["code"].is_null() ? "" : row["code"].c_str();
            j["bom_type"]       = row["bom_type"].is_null() ? "normal" : row["bom_type"].c_str();
            // P2: product_qty is BIGINT micro-units (migration 960). Default
            // 1'000'000 micros = 1.0, matching the previous default of 1.0.
            j["product_qty"]    = core::Money::fromMicros(
                                      row["product_qty"].as<long long>(1000000)).toJson();
            j["product_uom_id"] = row["product_uom_id"].is_null()
                                    ? nlohmann::json(false)
                                    : nlohmann::json::array({row["product_uom_id"].as<int>(),
                                                             row["uom_name"].c_str()});
            j["display_name"]   = row["product_name"].is_null() ? "BOM #" + std::to_string(j["id"].get<int>())
                                                                 : std::string(row["product_name"].c_str());
            arr.push_back(j);
        }
        return arr;
    }

    nlohmann::json handleRead(const CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return nlohmann::json::array();

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        std::string inList;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i) inList += ",";
            inList += std::to_string(ids[i]);
        }

        auto res = txn.exec(R"(
            SELECT b.id, b.product_id, p.name AS product_name,
                   b.code, b.bom_type, b.product_qty,
                   b.product_uom_id, u.name AS uom_name, b.active, b.company_id
            FROM mrp_bom b
            LEFT JOIN product_product p ON p.id = b.product_id
            LEFT JOIN uom_uom         u ON u.id = b.product_uom_id
            WHERE b.id IN ()" + inList + ")");
        txn.commit();

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]             = row["id"].as<int>();
            j["product_id"]     = row["product_id"].is_null()
                                    ? nlohmann::json(false)
                                    : nlohmann::json::array({row["product_id"].as<int>(),
                                                             row["product_name"].c_str()});
            j["code"]           = row["code"].is_null()     ? "" : row["code"].c_str();
            j["bom_type"]       = row["bom_type"].is_null() ? "normal" : row["bom_type"].c_str();
            // P2: product_qty is BIGINT micro-units (migration 960). Default
            // 1'000'000 micros = 1.0, matching the previous default of 1.0.
            j["product_qty"]    = core::Money::fromMicros(
                                      row["product_qty"].as<long long>(1000000)).toJson();
            j["product_uom_id"] = row["product_uom_id"].is_null()
                                    ? nlohmann::json(false)
                                    : nlohmann::json::array({row["product_uom_id"].as<int>(),
                                                             row["uom_name"].c_str()});
            j["active"]         = row["active"].is_null() ? true : (row["active"].c_str() == std::string("t"));
            j["company_id"]     = row["company_id"].is_null() ? nlohmann::json(false) : nlohmann::json(row["company_id"].as<int>());
            arr.push_back(j);
        }
        return arr;
    }

    nlohmann::json handleCreate(const CallKwArgs& call) {
        MrpBom proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const int newId = proto.create(call.arg(0));
        return newId;
    }

    nlohmann::json handleWrite(const CallKwArgs& call) {
        MrpBom proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const auto result = proto.write(call.ids(), call.arg(1));
        return result;
    }

    nlohmann::json handleUnlink(const CallKwArgs& call) {
        // Also remove lines
        const auto ids = call.ids();
        if (ids.empty()) return true;
        const auto ctx = extractContext_(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        std::string inList;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i) inList += ",";
            inList += std::to_string(ids[i]);
        }
        txn.exec("DELETE FROM mrp_bom_line WHERE bom_id IN (" + inList + ")");
        txn.exec("DELETE FROM mrp_bom        WHERE id      IN (" + inList + ")");
        txn.commit();
        return true;
    }

    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        MrpBom proto(db_);
        return proto.fieldsGet(call.fields());  // schema metadata — no rules needed
    }

    nlohmann::json handleSearchCount(const CallKwArgs& call) {
        MrpBom proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.searchCount(call.domain());
    }

    nlohmann::json handleSearch(const CallKwArgs& call) {
        MrpBom proto(db_);
        proto.setUserContext(extractContext_(call));
        auto ids = proto.search(call.domain(), call.limit() > 0 ? call.limit() : 80, call.offset(), "id ASC");
        nlohmann::json arr = nlohmann::json::array();
        for (int id : ids) arr.push_back(id);
        return arr;
    }
};

// ----------------------------------------------------------------
// MrpBomLineViewModel
// ----------------------------------------------------------------
class MrpBomLineViewModel : public BaseViewModel {
public:
    explicit MrpBomLineViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",  handleSearchRead)
        REGISTER_METHOD("read",         handleRead)
        REGISTER_MUTATOR("create",       handleCreate)
        REGISTER_MUTATOR("write",        handleWrite)
        REGISTER_MUTATOR("unlink",       handleUnlink)
        REGISTER_METHOD("fields_get",   handleFieldsGet)
    }

    std::string modelName() const override { return "mrp.bom.line"; }

private:
    std::shared_ptr<DbConnection> db_;

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        // Expect domain [['bom_id','=',N]]
        const auto domain = call.domain();
        int bomId = 0;
        if (domain.is_array()) {
            for (const auto& cond : domain) {
                if (cond.is_array() && cond.size() == 3 &&
                    cond[0].is_string() && cond[0].get<std::string>() == "bom_id") {
                    bomId = cond[2].is_number_integer() ? cond[2].get<int>() : 0;
                }
            }
        }

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        std::string sql = R"(
            SELECT l.id, l.bom_id, l.product_id,
                   p.name AS product_name,
                   l.product_qty, l.product_uom_id, u.name AS uom_name,
                   l.sequence
            FROM mrp_bom_line l
            LEFT JOIN product_product p ON p.id = l.product_id
            LEFT JOIN uom_uom         u ON u.id = l.product_uom_id
        )";

        pqxx::params params;
        if (bomId > 0) {
            sql += " WHERE l.bom_id = $1";
            params.append(bomId);
        }
        sql += " ORDER BY l.sequence, l.id";

        auto res = bomId > 0 ? txn.exec(sql, params) : txn.exec(sql);
        txn.commit();

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]             = row["id"].as<int>();
            j["bom_id"]         = row["bom_id"].as<int>();
            j["product_id"]     = row["product_id"].is_null()
                                    ? nlohmann::json(false)
                                    : nlohmann::json::array({row["product_id"].as<int>(),
                                                             row["product_name"].c_str()});
            // P2: product_qty is BIGINT micro-units (migration 960). Default
            // 1'000'000 micros = 1.0, matching the previous default of 1.0.
            j["product_qty"]    = core::Money::fromMicros(
                                      row["product_qty"].as<long long>(1000000)).toJson();
            j["product_uom_id"] = row["product_uom_id"].is_null()
                                    ? nlohmann::json(false)
                                    : nlohmann::json::array({row["product_uom_id"].as<int>(),
                                                             row["uom_name"].c_str()});
            j["sequence"]       = row["sequence"].as<int>(10);
            arr.push_back(j);
        }
        return arr;
    }

    nlohmann::json handleRead(const CallKwArgs& call) {
        MrpBomLine proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.read(call.ids());
    }

    nlohmann::json handleCreate(const CallKwArgs& call) {
        MrpBomLine proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const int newId = proto.create(call.arg(0));
        return newId;
    }

    nlohmann::json handleWrite(const CallKwArgs& call) {
        MrpBomLine proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const auto result = proto.write(call.ids(), call.arg(1));
        return result;
    }

    nlohmann::json handleUnlink(const CallKwArgs& call) {
        MrpBomLine proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const auto ids = call.ids();
        const auto result = proto.unlink(ids);
        return result;
    }

    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        MrpBomLine proto(db_);
        return proto.fieldsGet(call.fields());  // schema metadata — no rules needed
    }
};

// ----------------------------------------------------------------
// MrpRoutingWorkcenterViewModel — BOM operations, filtered by bom_id
// ----------------------------------------------------------------
class MrpRoutingWorkcenterViewModel : public BaseViewModel {
public:
    explicit MrpRoutingWorkcenterViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",  handleSearchRead)
        REGISTER_METHOD("read",         handleRead)
        REGISTER_MUTATOR("create",       handleCreate)
        REGISTER_MUTATOR("write",        handleWrite)
        REGISTER_MUTATOR("unlink",       handleUnlink)
        REGISTER_METHOD("fields_get",   handleFieldsGet)
    }
    std::string modelName() const override { return "mrp.routing.workcenter"; }
private:
    std::shared_ptr<DbConnection> db_;

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        const auto domain = call.domain();
        int bomId = 0;
        if (domain.is_array())
            for (const auto& cond : domain)
                if (cond.is_array() && cond.size() == 3 && cond[0].is_string() &&
                    cond[0].get<std::string>() == "bom_id" && cond[2].is_number_integer())
                    bomId = cond[2].get<int>();

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT r.id, r.bom_id, r.workcenter_id, w.name AS workcenter_name,
                   r.name, r.sequence, r.time_cycle_manual
            FROM mrp_routing_workcenter r
            LEFT JOIN mrp_workcenter w ON w.id = r.workcenter_id
        )";
        pqxx::params params;
        if (bomId > 0) { sql += " WHERE r.bom_id = $1"; params.append(bomId); }
        sql += " ORDER BY r.sequence, r.id";
        auto res = bomId > 0 ? txn.exec(sql, params) : txn.exec(sql);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]            = row["id"].as<int>();
            j["bom_id"]        = row["bom_id"].as<int>();
            j["workcenter_id"] = row["workcenter_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["workcenter_id"].as<int>(), row["workcenter_name"].c_str()});
            j["name"]              = row["name"].is_null() ? "" : row["name"].c_str();
            j["sequence"]          = row["sequence"].as<int>(10);
            j["time_cycle_manual"] = row["time_cycle_manual"].as<double>(60.0);
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const CallKwArgs& call) {
        MrpRoutingWorkcenter proto(db_); proto.setUserContext(extractContext_(call));
        return proto.read(call.ids());
    }
    nlohmann::json handleCreate(const CallKwArgs& call) {
        MrpRoutingWorkcenter proto(db_); proto.setUserContext(extractContext_(call));
        return proto.create(call.arg(0));
    }
    nlohmann::json handleWrite(const CallKwArgs& call) {
        MrpRoutingWorkcenter proto(db_); proto.setUserContext(extractContext_(call));
        return proto.write(call.ids(), call.arg(1));
    }
    nlohmann::json handleUnlink(const CallKwArgs& call) {
        MrpRoutingWorkcenter proto(db_); proto.setUserContext(extractContext_(call));
        return proto.unlink(call.ids());
    }
    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        MrpRoutingWorkcenter proto(db_); return proto.fieldsGet(call.fields());
    }
};

// ----------------------------------------------------------------
// MrpProductionViewModel — the Manufacturing Order engine.
// action_confirm explodes the BOM into raw + finished stock moves;
// button_mark_done consumes and produces through the quant engine.
// ----------------------------------------------------------------
class MrpProductionViewModel : public BaseViewModel {
public:
    explicit MrpProductionViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_METHOD("web_read",        handleRead)
        REGISTER_MUTATOR("create",          handleCreate)
        REGISTER_MUTATOR("write",           handleWrite)
        REGISTER_MUTATOR("unlink",          handleUnlink)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
        REGISTER_METHOD("search",          handleSearch)
        REGISTER_METHOD("default_get",     handleDefaultGet)
        REGISTER_METHOD("action_confirm",  handleActionConfirm)
        REGISTER_METHOD("button_mark_done",handleMarkDone)
        REGISTER_METHOD("action_cancel",   handleActionCancel)
    }
    std::string modelName() const override { return "mrp.production"; }
private:
    std::shared_ptr<DbConnection> db_;

    static nlohmann::json m2o(const pqxx::row& row, const char* idc, const char* namec) {
        if (row[idc].is_null()) return false;
        return nlohmann::json::array(
            {row[idc].as<int>(), row[namec].is_null() ? "" : std::string(row[namec].c_str())});
    }

    // The Production virtual location (usage='production').
    static int productionLocId(pqxx::work& txn) {
        auto r = txn.exec("SELECT id FROM stock_location WHERE usage='production' ORDER BY id LIMIT 1");
        return r.empty() ? 0 : r[0][0].as<int>();
    }

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        const int lim = call.limit() > 0 ? call.limit() : 80;
        const int off = call.offset();
        // S-49: restrict domain columns to a fixed allowlist of MO fields.
        static const std::set<std::string> kCols = {
            "id","name","state","product_id","bom_id","company_id",
            "date_planned_start","origin","user_id"};
        auto [where, paramVec] = domainFromJson(call.domain()).toSql(&kCols);

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT mp.id, mp.name, mp.state, mp.product_qty, mp.qty_producing,
                   mp.date_planned_start, mp.origin,
                   mp.product_id, pp.name AS product_name, mp.bom_id, mp.company_id
            FROM mrp_production mp
            LEFT JOIN product_product pp ON pp.id = mp.product_id
            WHERE )";
        sql += where;
        sql += " ORDER BY mp.id DESC LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);
        pqxx::result res;
        if (paramVec.empty()) res = txn.exec(sql);
        else { pqxx::params p; for (auto& s : paramVec) p.append(s); res = txn.exec(sql, p); }

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]                 = row["id"].as<int>();
            j["name"]               = row["name"].is_null()  ? nlohmann::json(false) : nlohmann::json(row["name"].c_str());
            j["state"]              = row["state"].is_null() ? nlohmann::json(false) : nlohmann::json(row["state"].c_str());
            j["product_qty"]        = core::Money::fromMicros(row["product_qty"].as<long long>(0)).toJson();
            j["qty_producing"]      = core::Money::fromMicros(row["qty_producing"].as<long long>(0)).toJson();
            j["date_planned_start"] = row["date_planned_start"].is_null() ? nlohmann::json(false) : nlohmann::json(row["date_planned_start"].c_str());
            j["origin"]             = row["origin"].is_null() ? nlohmann::json(false) : nlohmann::json(row["origin"].c_str());
            j["product_id"]         = m2o(row, "product_id", "product_name");
            j["bom_id"]             = row["bom_id"].is_null()     ? nlohmann::json(false) : nlohmann::json(row["bom_id"].as<int>());
            j["company_id"]         = row["company_id"].is_null() ? nlohmann::json(false) : nlohmann::json(row["company_id"].as<int>());
            arr.push_back(std::move(j));
        }
        return arr;
    }

    nlohmann::json handleRead(const CallKwArgs& call) {
        MrpProduction proto(db_); proto.setUserContext(extractContext_(call));
        return proto.read(call.ids(), call.fields());
    }
    nlohmann::json handleCreate(const CallKwArgs& call) {
        MrpProduction proto(db_); proto.setUserContext(extractContext_(call));
        return proto.create(call.arg(0));
    }
    nlohmann::json handleWrite(const CallKwArgs& call) {
        MrpProduction proto(db_); proto.setUserContext(extractContext_(call));
        return proto.write(call.ids(), call.arg(1));
    }
    nlohmann::json handleUnlink(const CallKwArgs& call) {
        MrpProduction proto(db_); proto.setUserContext(extractContext_(call));
        return proto.unlink(call.ids());
    }
    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        MrpProduction proto(db_); return proto.fieldsGet(call.fields());
    }
    nlohmann::json handleSearchCount(const CallKwArgs& call) {
        MrpProduction proto(db_); proto.setUserContext(extractContext_(call));
        return proto.searchCount(call.domain());
    }
    nlohmann::json handleSearch(const CallKwArgs& call) {
        MrpProduction proto(db_); proto.setUserContext(extractContext_(call));
        auto ids = proto.search(call.domain(), call.limit() > 0 ? call.limit() : 80, call.offset());
        nlohmann::json arr = nlohmann::json::array();
        for (int id : ids) arr.push_back(id);
        return arr;
    }
    nlohmann::json handleDefaultGet(const CallKwArgs& /*call*/) {
        return {{"state","draft"}, {"product_qty",1.0},
                {"location_src_id",4}, {"location_dest_id",4}, {"company_id",1}};
    }

    // Explode the BOM: raw-material moves (src → Production) + a finished
    // move (Production → dest). Quantities scale with the MO quantity.
    nlohmann::json handleActionConfirm(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto pr = txn.exec(
                "SELECT product_id, product_qty, bom_id, location_src_id, "
                "location_dest_id, company_id, state, name FROM mrp_production WHERE id=$1",
                pqxx::params{id});
            if (pr.empty()) continue;
            if (std::string(pr[0]["state"].c_str()) != "draft") continue;

            const int       prod    = pr[0]["product_id"].as<int>();
            const long long moQty   = pr[0]["product_qty"].as<long long>(0);
            const int       srcLoc  = pr[0]["location_src_id"].is_null()  ? 4 : pr[0]["location_src_id"].as<int>();
            const int       destLoc = pr[0]["location_dest_id"].is_null() ? 4 : pr[0]["location_dest_id"].as<int>();
            const int       comp    = pr[0]["company_id"].is_null() ? 0 : pr[0]["company_id"].as<int>();
            int             bomId   = pr[0]["bom_id"].is_null() ? 0 : pr[0]["bom_id"].as<int>();

            long long bomQty = 1000000;
            if (bomId <= 0) {
                auto b = txn.exec("SELECT id, product_qty FROM mrp_bom WHERE product_id=$1 AND active=TRUE ORDER BY id LIMIT 1",
                                  pqxx::params{prod});
                if (!b.empty()) { bomId = b[0]["id"].as<int>(); bomQty = b[0]["product_qty"].as<long long>(1000000); }
            } else {
                auto b = txn.exec("SELECT product_qty FROM mrp_bom WHERE id=$1", pqxx::params{bomId});
                if (!b.empty()) bomQty = b[0]["product_qty"].as<long long>(1000000);
            }
            if (bomQty <= 0) bomQty = 1000000;
            const int prodLoc = productionLocId(txn);

            if (bomId > 0 && prodLoc > 0) {
                txn.exec(
                    "INSERT INTO stock_move (product_id, name, product_uom_qty, quantity, state, "
                    "  location_id, location_dest_id, company_id, production_id, is_production_raw, product_uom_id) "
                    "SELECT bl.product_id, COALESCE(pp.name,'component'), "
                    "  (bl.product_qty::numeric * $2 / $3)::bigint, 0, 'confirmed', "
                    "  $4, $5, NULLIF($6,0), $1, TRUE, bl.product_uom_id "
                    "FROM mrp_bom_line bl LEFT JOIN product_product pp ON pp.id=bl.product_id "
                    "WHERE bl.bom_id=$7",
                    pqxx::params{id, moQty, bomQty, srcLoc, prodLoc, comp, bomId});
            }
            if (prodLoc > 0) {
                txn.exec(
                    "INSERT INTO stock_move (product_id, name, product_uom_qty, quantity, state, "
                    "  location_id, location_dest_id, company_id, production_id, is_production_raw, product_uom_id) "
                    "SELECT $2, COALESCE((SELECT name FROM product_product WHERE id=$2),'finished'), $3, 0, 'confirmed', "
                    "  $4, $5, NULLIF($6,0), $1, FALSE, (SELECT uom_id FROM product_product WHERE id=$2)",
                    pqxx::params{id, prod, moQty, prodLoc, destLoc, comp});
            }

            // One work order per BOM operation. Expected duration = the
            // operation's per-unit cycle time scaled by the order quantity.
            if (bomId > 0) {
                txn.exec(
                    "INSERT INTO mrp_workorder (production_id, workcenter_id, operation_id, "
                    "  name, sequence, state, duration_expected, company_id) "
                    "SELECT $1, r.workcenter_id, r.id, r.name, r.sequence, 'ready', "
                    "  (r.time_cycle_manual * $2 / 1000000.0), NULLIF($3,0) "
                    "FROM mrp_routing_workcenter r WHERE r.bom_id = $4",
                    pqxx::params{id, moQty, comp, bomId});
            }

            std::string curName = pr[0]["name"].is_null() ? "New" : pr[0]["name"].c_str();
            std::string moName  = curName;
            if (curName == "New" || curName.empty())
                moName = core::IrSequence::instance().nextByCode(txn, "mrp.production");
            if (bomId > 0)
                txn.exec("UPDATE mrp_production SET name=$1, bom_id=$2, state='confirmed', write_date=now() WHERE id=$3",
                         pqxx::params{moName, bomId, id});
            else
                txn.exec("UPDATE mrp_production SET name=$1, state='confirmed', write_date=now() WHERE id=$2",
                         pqxx::params{moName, id});
            txn.commit();
        }
        if (AuditService::ready() && !call.ids().empty())
            AuditService::instance().log("mrp.production", "action_confirm", call.ids(), extractContext_(call).uid);
        return true;
    }

    // Consume raw materials and produce the finished good through the quant
    // engine. Gated on all work orders being finished (Phase 3).
    nlohmann::json handleMarkDone(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto pr = txn.exec("SELECT state FROM mrp_production WHERE id=$1", pqxx::params{id});
            if (pr.empty()) continue;
            const std::string st = pr[0]["state"].c_str();
            if (st == "done" || st == "cancel" || st == "draft") continue;

            auto woOpen = txn.exec(
                "SELECT count(*) FROM mrp_workorder WHERE production_id=$1 AND state NOT IN ('done','cancel')",
                pqxx::params{id});
            if (!woOpen.empty() && woOpen[0][0].as<long long>(0) > 0)
                throw std::runtime_error("All work orders must be finished before closing the manufacturing order");

            // Consume raw materials first, summing the value that flows into the
            // build; then produce the finished good valued at that build cost
            // (total component value ÷ produced quantity), so manufacturing
            // conserves inventory value.
            long long componentValue = 0;   // micro-units
            long long producedQty    = 0;
            auto raws = txn.exec(
                "SELECT id, product_id, location_id, location_dest_id, product_uom_qty, quantity, company_id "
                "FROM stock_move WHERE production_id=$1 AND is_production_raw=TRUE AND state NOT IN ('done','cancel')",
                pqxx::params{id});
            for (const auto& m : raws) {
                const int       mid = m["id"].as<int>();
                const int       p   = m["product_id"].as<int>();
                const int       s   = m["location_id"].as<int>();
                const int       d   = m["location_dest_id"].as<int>();
                const int       c   = m["company_id"].is_null() ? 0 : m["company_id"].as<int>();
                long long       q   = m["quantity"].as<long long>(0);
                if (q <= 0) q = m["product_uom_qty"].as<long long>(0);
                auto cc = txn.exec("SELECT standard_price FROM product_product WHERE id=$1", pqxx::params{p});
                const long long cost = cc.empty() ? 0 : cc[0][0].as<long long>(0);
                componentValue += static_cast<long long>(static_cast<__int128>(q) * cost / 1000000);
                core::StockQuant::applyMove(txn, p, s, d, q, c);
                txn.exec("UPDATE stock_move SET quantity=$1, state='done', write_date=now() WHERE id=$2",
                         pqxx::params{q, mid});
            }
            auto fins = txn.exec(
                "SELECT id, product_id, location_id, location_dest_id, product_uom_qty, quantity, company_id "
                "FROM stock_move WHERE production_id=$1 AND is_production_raw=FALSE AND state NOT IN ('done','cancel')",
                pqxx::params{id});
            for (const auto& m : fins) {
                const int       mid = m["id"].as<int>();
                const int       p   = m["product_id"].as<int>();
                const int       s   = m["location_id"].as<int>();
                const int       d   = m["location_dest_id"].as<int>();
                const int       c   = m["company_id"].is_null() ? 0 : m["company_id"].as<int>();
                long long       q   = m["quantity"].as<long long>(0);
                if (q <= 0) q = m["product_uom_qty"].as<long long>(0);
                producedQty += q;
                const long long inCost = q > 0
                    ? static_cast<long long>(static_cast<__int128>(componentValue) * 1000000 / q) : -1;
                core::StockQuant::applyMove(txn, p, s, d, q, c, inCost);
                txn.exec("UPDATE stock_move SET quantity=$1, state='done', write_date=now() WHERE id=$2",
                         pqxx::params{q, mid});
            }
            (void)producedQty;
            txn.exec("UPDATE mrp_production SET state='done', qty_producing=product_qty, write_date=now() WHERE id=$1",
                     pqxx::params{id});
            txn.commit();
        }
        if (AuditService::ready() && !call.ids().empty())
            AuditService::instance().log("mrp.production", "button_mark_done", call.ids(), extractContext_(call).uid);
        return true;
    }

    nlohmann::json handleActionCancel(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            txn.exec("UPDATE stock_move SET state='cancel' WHERE production_id=$1 AND state<>'done'", pqxx::params{id});
            txn.exec("UPDATE mrp_workorder SET state='cancel' WHERE production_id=$1 AND state<>'done'", pqxx::params{id});
            txn.exec("UPDATE mrp_production SET state='cancel', write_date=now() WHERE id=$1 AND state<>'done'", pqxx::params{id});
            txn.commit();
        }
        return true;
    }
};

// ----------------------------------------------------------------
// MrpWorkorderViewModel — start/finish a work order; when the last
// one finishes the MO becomes 'to_close'.
// ----------------------------------------------------------------
class MrpWorkorderViewModel : public BaseViewModel {
public:
    explicit MrpWorkorderViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_METHOD("web_read",        handleRead)
        REGISTER_MUTATOR("create",          handleCreate)
        REGISTER_MUTATOR("write",           handleWrite)
        REGISTER_MUTATOR("unlink",          handleUnlink)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
        REGISTER_METHOD("button_start",    handleButtonStart)
        REGISTER_METHOD("button_finish",   handleButtonFinish)
    }
    std::string modelName() const override { return "mrp.workorder"; }
private:
    std::shared_ptr<DbConnection> db_;

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        const auto domain = call.domain();
        int prodId = 0;
        if (domain.is_array())
            for (const auto& c : domain)
                if (c.is_array() && c.size() == 3 && c[0].is_string() &&
                    c[0].get<std::string>() == "production_id" && c[2].is_number_integer())
                    prodId = c[2].get<int>();

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT wo.id, wo.production_id, mp.name AS production_name,
                   wo.workcenter_id, w.name AS workcenter_name,
                   wo.name, wo.sequence, wo.state,
                   wo.duration_expected, wo.duration, wo.qty_produced,
                   wo.date_start, wo.date_finished
            FROM mrp_workorder wo
            LEFT JOIN mrp_workcenter  w  ON w.id  = wo.workcenter_id
            LEFT JOIN mrp_production  mp ON mp.id = wo.production_id
        )";
        pqxx::params params;
        if (prodId > 0) { sql += " WHERE wo.production_id = $1"; params.append(prodId); }
        sql += " ORDER BY wo.sequence, wo.id";
        auto res = prodId > 0 ? txn.exec(sql, params) : txn.exec(sql);

        auto m2o = [](const pqxx::row& row, const char* idc, const char* namec) -> nlohmann::json {
            if (row[idc].is_null()) return false;
            return nlohmann::json::array(
                {row[idc].as<int>(), row[namec].is_null() ? "" : std::string(row[namec].c_str())});
        };
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]                = row["id"].as<int>();
            j["production_id"]     = m2o(row, "production_id", "production_name");
            j["workcenter_id"]     = m2o(row, "workcenter_id", "workcenter_name");
            j["name"]              = row["name"].is_null() ? "" : row["name"].c_str();
            j["sequence"]          = row["sequence"].as<int>(10);
            j["state"]             = row["state"].is_null() ? "pending" : row["state"].c_str();
            j["duration_expected"] = row["duration_expected"].as<double>(0.0);
            j["duration"]          = row["duration"].as<double>(0.0);
            j["qty_produced"]      = core::Money::fromMicros(row["qty_produced"].as<long long>(0)).toJson();
            j["date_start"]        = row["date_start"].is_null()    ? nlohmann::json(false) : nlohmann::json(row["date_start"].c_str());
            j["date_finished"]     = row["date_finished"].is_null() ? nlohmann::json(false) : nlohmann::json(row["date_finished"].c_str());
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const CallKwArgs& call) {
        MrpWorkorder p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids(), call.fields());
    }
    nlohmann::json handleCreate(const CallKwArgs& call) {
        MrpWorkorder p(db_); p.setUserContext(extractContext_(call)); return p.create(call.arg(0));
    }
    nlohmann::json handleWrite(const CallKwArgs& call) {
        MrpWorkorder p(db_); p.setUserContext(extractContext_(call)); return p.write(call.ids(), call.arg(1));
    }
    nlohmann::json handleUnlink(const CallKwArgs& call) {
        MrpWorkorder p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids());
    }
    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        MrpWorkorder p(db_); return p.fieldsGet(call.fields());
    }
    nlohmann::json handleSearchCount(const CallKwArgs& call) {
        MrpWorkorder p(db_); p.setUserContext(extractContext_(call)); return p.searchCount(call.domain());
    }

    nlohmann::json handleButtonStart(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            txn.exec("UPDATE mrp_workorder SET state='progress', "
                     "date_start=COALESCE(date_start, now()), write_date=now() "
                     "WHERE id=$1 AND state IN ('ready','pending','progress')",
                     pqxx::params{id});
            txn.exec("UPDATE mrp_production SET state='progress', write_date=now() "
                     "WHERE id=(SELECT production_id FROM mrp_workorder WHERE id=$1) AND state='confirmed'",
                     pqxx::params{id});
            txn.commit();
        }
        return true;
    }

    nlohmann::json handleButtonFinish(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            txn.exec(
                "UPDATE mrp_workorder SET state='done', date_finished=now(), "
                "duration = CASE WHEN duration > 0 THEN duration ELSE duration_expected END, "
                "qty_produced = COALESCE((SELECT product_qty FROM mrp_production WHERE id=mrp_workorder.production_id),0), "
                "write_date=now() WHERE id=$1 AND state IN ('progress','ready','pending')",
                pqxx::params{id});
            // Last open work order finished → the MO is ready to close.
            txn.exec(
                "UPDATE mrp_production SET state='to_close', write_date=now() "
                "WHERE id=(SELECT production_id FROM mrp_workorder WHERE id=$1) "
                "AND state IN ('confirmed','progress') "
                "AND NOT EXISTS (SELECT 1 FROM mrp_workorder wo2 "
                "  WHERE wo2.production_id=(SELECT production_id FROM mrp_workorder WHERE id=$1) "
                "  AND wo2.state NOT IN ('done','cancel'))",
                pqxx::params{id});
            txn.commit();
        }
        if (AuditService::ready() && !call.ids().empty())
            AuditService::instance().log("mrp.workorder", "button_finish", call.ids(), extractContext_(call).uid);
        return true;
    }
};

// ----------------------------------------------------------------
// MrpProductionScheduleViewModel — the Master Production Schedule.
// get_mps_grid runs the time-phased projection; set_forecast enters demand
// per period; action_replenish turns a suggested quantity into a draft MO.
// ----------------------------------------------------------------
class MrpProductionScheduleViewModel : public BaseViewModel {
public:
    explicit MrpProductionScheduleViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",      handleSearchRead)
        REGISTER_METHOD("web_search_read",  handleSearchRead)
        REGISTER_METHOD("read",             handleRead)
        REGISTER_METHOD("web_read",         handleRead)
        REGISTER_MUTATOR("create",           handleCreate)
        REGISTER_MUTATOR("write",            handleWrite)
        REGISTER_MUTATOR("unlink",           handleUnlink)
        REGISTER_METHOD("fields_get",       handleFieldsGet)
        REGISTER_METHOD("search_count",     handleSearchCount)
        REGISTER_METHOD("set_forecast",     handleSetForecast)
        REGISTER_METHOD("get_mps_grid",     handleGetGrid)
        REGISTER_METHOD("action_replenish", handleReplenish)
    }
    std::string modelName() const override { return "mrp.production.schedule"; }
private:
    std::shared_ptr<DbConnection> db_;

    static int    argInt(const nlohmann::json& v, const char* k)  {
        return (v.is_object() && v.contains(k)) ? mrpM2oId(v[k]) : 0;
    }
    static double argNum(const nlohmann::json& v, const char* k, const char* alt = nullptr) {
        if (v.is_object() && v.contains(k) && v[k].is_number())   return v[k].get<double>();
        if (alt && v.is_object() && v.contains(alt) && v[alt].is_number()) return v[alt].get<double>();
        return 0.0;
    }
    static std::string argStr(const nlohmann::json& v, const char* k) {
        return (v.is_object() && v.contains(k) && v[k].is_string()) ? v[k].get<std::string>() : std::string();
    }
    static std::string firstOfMonth(const std::string& ymd) {
        int y = 0, m = 1, d = 1; std::sscanf(ymd.c_str(), "%d-%d-%d", &y, &m, &d);
        if (m < 1) m = 1; if (m > 12) m = 12;
        char b[16]; std::snprintf(b, sizeof(b), "%04d-%02d-01", y, m); return b;
    }
    static std::string nextMonth(const std::string& ymd) {
        int y = 0, m = 1, d = 1; std::sscanf(ymd.c_str(), "%d-%d-%d", &y, &m, &d);
        if (m >= 12) { y++; m = 1; } else { m++; }
        char b[16]; std::snprintf(b, sizeof(b), "%04d-%02d-01", y, m); return b;
    }
    static long long toMicros(double q) { return static_cast<long long>(std::llround(q * 1000000.0)); }

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        const int lim = call.limit() > 0 ? call.limit() : 80;
        const int off = call.offset();
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT s.id, s.product_id, pp.name AS product_name,
                   s.min_to_replenish, COALESCE(pp.qty_available,0) AS on_hand, s.company_id
            FROM mrp_production_schedule s
            LEFT JOIN product_product pp ON pp.id = s.product_id
            ORDER BY pp.name)";
        sql += " LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);
        auto res = txn.exec(sql);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"] = row["id"].as<int>();
            j["product_id"] = row["product_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["product_id"].as<int>(),
                                         row["product_name"].is_null() ? "" : std::string(row["product_name"].c_str())});
            j["min_to_replenish"] = core::Money::fromMicros(row["min_to_replenish"].as<long long>(0)).toJson();
            j["on_hand"]          = core::Money::fromMicros(row["on_hand"].as<long long>(0)).toJson();
            j["company_id"]       = row["company_id"].is_null() ? nlohmann::json(false) : nlohmann::json(row["company_id"].as<int>());
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const CallKwArgs& call)        { MrpProductionSchedule p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids(), call.fields()); }
    nlohmann::json handleCreate(const CallKwArgs& call)      { MrpProductionSchedule p(db_); p.setUserContext(extractContext_(call)); return p.create(call.arg(0)); }
    nlohmann::json handleWrite(const CallKwArgs& call)       { MrpProductionSchedule p(db_); p.setUserContext(extractContext_(call)); return p.write(call.ids(), call.arg(1)); }
    nlohmann::json handleUnlink(const CallKwArgs& call)      { MrpProductionSchedule p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids()); }
    nlohmann::json handleFieldsGet(const CallKwArgs& call)   { MrpProductionSchedule p(db_); return p.fieldsGet(call.fields()); }
    nlohmann::json handleSearchCount(const CallKwArgs& call) { MrpProductionSchedule p(db_); p.setUserContext(extractContext_(call)); return p.searchCount(call.domain()); }

    // Enter forecasted demand for a product in a given month.
    nlohmann::json handleSetForecast(const CallKwArgs& call) {
        const auto v = call.arg(0);
        const int prod = argInt(v, "product_id");
        std::string date = argStr(v, "date");
        if (prod <= 0 || date.empty())
            throw std::runtime_error("set_forecast: product_id and date are required");
        date = firstOfMonth(date);
        const long long q = toMicros(argNum(v, "quantity", "forecast_qty"));
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        txn.exec("INSERT INTO mrp_forecast (product_id, date, forecast_qty) VALUES ($1,$2,$3) "
                 "ON CONFLICT (product_id, date) DO UPDATE SET forecast_qty=EXCLUDED.forecast_qty, write_date=now()",
                 pqxx::params{prod, date, q});
        txn.commit();
        return true;
    }

    // The time-phased projection: for each month, starting on-hand carries
    // forward, demand is subtracted, and a replenishment is suggested to keep
    // the projected stock at or above the safety level.
    nlohmann::json handleGetGrid(const CallKwArgs& call) {
        const auto v = call.arg(0);
        const int prod = argInt(v, "product_id");
        if (prod <= 0) throw std::runtime_error("get_mps_grid: product_id is required");
        int nPer = (v.is_object() && v.contains("periods") && v["periods"].is_number_integer()) ? v["periods"].get<int>() : 6;
        if (nPer < 1) nPer = 1; if (nPer > 24) nPer = 24;

        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string start = argStr(v, "start");
        if (start.empty())
            start = txn.exec("SELECT to_char(date_trunc('month', CURRENT_DATE), 'YYYY-MM-DD')")[0][0].c_str();
        start = firstOfMonth(start);

        long long onHand = 0;
        { auto r = txn.exec("SELECT COALESCE(qty_available,0) FROM product_product WHERE id=$1", pqxx::params{prod});
          if (!r.empty()) onHand = r[0][0].as<long long>(0); }
        long long minRep = 0;
        { auto r = txn.exec("SELECT min_to_replenish FROM mrp_production_schedule WHERE product_id=$1", pqxx::params{prod});
          if (!r.empty()) minRep = r[0][0].as<long long>(0); }

        nlohmann::json periods = nlohmann::json::array();
        long long incoming = onHand;
        std::string period = start;
        for (int i = 0; i < nPer; ++i) {
            long long demand = 0;
            { auto r = txn.exec("SELECT forecast_qty FROM mrp_forecast WHERE product_id=$1 AND date=$2",
                                pqxx::params{prod, period});
              if (!r.empty()) demand = r[0][0].as<long long>(0); }
            const long long projected = incoming - demand;
            const long long replenish = projected < minRep ? (minRep - projected) : 0;
            const long long ending    = projected + replenish;
            nlohmann::json p;
            p["date"]           = period;
            p["forecast"]       = core::Money::fromMicros(demand).toJson();
            p["to_replenish"]   = core::Money::fromMicros(replenish).toJson();
            p["forecast_stock"] = core::Money::fromMicros(ending).toJson();
            periods.push_back(std::move(p));
            incoming = ending;
            period   = nextMonth(period);
        }
        nlohmann::json out;
        out["product_id"]       = prod;
        out["on_hand"]          = core::Money::fromMicros(onHand).toJson();
        out["min_to_replenish"] = core::Money::fromMicros(minRep).toJson();
        out["periods"]          = std::move(periods);
        return out;
    }

    // Turn a suggested replenishment into a draft manufacturing order.
    nlohmann::json handleReplenish(const CallKwArgs& call) {
        const auto v = call.arg(0);
        const int prod = argInt(v, "product_id");
        const double qtyF = argNum(v, "quantity");
        std::string date = argStr(v, "date");
        if (prod <= 0 || qtyF <= 0)
            throw std::runtime_error("action_replenish: product_id and a positive quantity are required");
        const long long q = toMicros(qtyF);
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        int moId;
        if (date.empty())
            moId = txn.exec(
                "INSERT INTO mrp_production (name, product_id, product_qty, state, "
                "location_src_id, location_dest_id, company_id, origin) "
                "VALUES ('New',$1,$2,'draft',4,4,1,'MPS') RETURNING id",
                pqxx::params{prod, q})[0][0].as<int>();
        else
            moId = txn.exec(
                "INSERT INTO mrp_production (name, product_id, product_qty, state, "
                "location_src_id, location_dest_id, date_planned_start, company_id, origin) "
                "VALUES ('New',$1,$2,'draft',4,4,$3,1,'MPS') RETURNING id",
                pqxx::params{prod, q, firstOfMonth(date)})[0][0].as<int>();
        txn.commit();
        nlohmann::json out; out["production_id"] = moId; return out;
    }
};

// ================================================================
// 4. MODULE — method implementations
// ================================================================

MrpModule::MrpModule(core::ModelFactory&     models,
                     core::ServiceFactory&   services,
                     core::ViewModelFactory& viewModels,
                     core::ViewFactory&      views)
    : models_(models), services_(services),
      viewModels_(viewModels), views_(views)
{}

std::string              MrpModule::moduleName()   const { return "mrp"; }
std::vector<std::string> MrpModule::dependencies() const { return {"product", "stock"}; }

void MrpModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("mrp.bom",                [db]{ return std::make_shared<MrpBom>(db); });
    models_.registerCreator("mrp.bom.line",           [db]{ return std::make_shared<MrpBomLine>(db); });
    models_.registerCreator("mrp.workcenter",         [db]{ return std::make_shared<MrpWorkcenter>(db); });
    models_.registerCreator("mrp.routing.workcenter", [db]{ return std::make_shared<MrpRoutingWorkcenter>(db); });
    models_.registerCreator("mrp.production",         [db]{ return std::make_shared<MrpProduction>(db); });
    models_.registerCreator("mrp.workorder",          [db]{ return std::make_shared<MrpWorkorder>(db); });
    models_.registerCreator("mrp.production.schedule",[db]{ return std::make_shared<MrpProductionSchedule>(db); });
    models_.registerCreator("mrp.forecast",           [db]{ return std::make_shared<MrpForecast>(db); });
}

void MrpModule::registerServices() {}

void MrpModule::registerViewModels() {
    auto db = services_.db();
    viewModels_.registerCreator("mrp.bom",                [db]{ return std::make_shared<MrpBomViewModel>(db); });
    viewModels_.registerCreator("mrp.bom.line",           [db]{ return std::make_shared<MrpBomLineViewModel>(db); });
    viewModels_.registerCreator("mrp.workcenter",         [db]{ return std::make_shared<GenericViewModel<MrpWorkcenter>>(db); });
    viewModels_.registerCreator("mrp.routing.workcenter", [db]{ return std::make_shared<MrpRoutingWorkcenterViewModel>(db); });
    viewModels_.registerCreator("mrp.production",         [db]{ return std::make_shared<MrpProductionViewModel>(db); });
    viewModels_.registerCreator("mrp.workorder",         [db]{ return std::make_shared<MrpWorkorderViewModel>(db); });
    viewModels_.registerCreator("mrp.production.schedule",[db]{ return std::make_shared<MrpProductionScheduleViewModel>(db); });
    viewModels_.registerCreator("mrp.forecast",           [db]{ return std::make_shared<GenericViewModel<MrpForecast>>(db); });
}

void MrpModule::registerViews() {
    views_.registerCreator("mrp.bom.list",         []{ return std::make_shared<MrpBomListView>(); });
    views_.registerCreator("mrp.bom.form",         []{ return std::make_shared<MrpBomFormView>(); });
    views_.registerCreator("mrp.production.list",  []{ return std::make_shared<MrpProductionListView>(); });
    views_.registerCreator("mrp.production.form",  []{ return std::make_shared<MrpProductionFormView>(); });
    views_.registerCreator("mrp.workcenter.list",  []{ return std::make_shared<MrpWorkcenterListView>(); });
    views_.registerCreator("mrp.workcenter.form",  []{ return std::make_shared<MrpWorkcenterFormView>(); });
    views_.registerCreator("mrp.workorder.list",   []{ return std::make_shared<MrpWorkorderListView>(); });
    views_.registerCreator("mrp.production.schedule.list", []{ return std::make_shared<MrpProductionScheduleListView>(); });
}

void MrpModule::registerRoutes() {}

void MrpModule::initialize() {
    ensureSchema_();
    seedProductionData_();
    seedMenus_();
}

// ----------------------------------------------------------
// Schema
// ----------------------------------------------------------
void MrpModule::ensureSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS mrp_bom (
            id             SERIAL PRIMARY KEY,
            product_id     INTEGER REFERENCES product_product(id),
            code           VARCHAR,
            bom_type       VARCHAR NOT NULL DEFAULT 'normal',
            product_qty    NUMERIC(12,6) NOT NULL DEFAULT 1.0,
            product_uom_id INTEGER REFERENCES uom_uom(id),
            company_id     INTEGER REFERENCES res_company(id),
            active         BOOLEAN NOT NULL DEFAULT TRUE,
            create_date    TIMESTAMP DEFAULT now(),
            write_date     TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS mrp_bom_line (
            id             SERIAL PRIMARY KEY,
            bom_id         INTEGER NOT NULL REFERENCES mrp_bom(id) ON DELETE CASCADE,
            product_id     INTEGER REFERENCES product_product(id),
            product_qty    NUMERIC(12,6) NOT NULL DEFAULT 1.0,
            product_uom_id INTEGER REFERENCES uom_uom(id),
            sequence       INTEGER NOT NULL DEFAULT 10,
            create_date    TIMESTAMP DEFAULT now(),
            write_date     TIMESTAMP DEFAULT now()
        )
    )");

    // ---- Manufacturing: work centers, BOM operations, orders (P2) ----
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS mrp_workcenter (
            id              SERIAL PRIMARY KEY,
            name            VARCHAR NOT NULL,
            code            VARCHAR,
            costs_hour      BIGINT NOT NULL DEFAULT 0,
            time_efficiency NUMERIC(8,2)  NOT NULL DEFAULT 100,
            capacity        NUMERIC(12,4) NOT NULL DEFAULT 1,
            company_id      INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            active          BOOLEAN NOT NULL DEFAULT TRUE,
            create_date     TIMESTAMP DEFAULT now(),
            write_date      TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS mrp_routing_workcenter (
            id                SERIAL PRIMARY KEY,
            bom_id            INTEGER NOT NULL REFERENCES mrp_bom(id) ON DELETE CASCADE,
            workcenter_id     INTEGER REFERENCES mrp_workcenter(id) ON DELETE SET NULL,
            name              VARCHAR NOT NULL,
            sequence          INTEGER NOT NULL DEFAULT 10,
            time_cycle_manual NUMERIC(12,4) NOT NULL DEFAULT 60,
            company_id        INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date       TIMESTAMP DEFAULT now(),
            write_date        TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS mrp_production (
            id                 SERIAL PRIMARY KEY,
            name               VARCHAR NOT NULL DEFAULT 'New',
            product_id         INTEGER NOT NULL REFERENCES product_product(id),
            product_qty        BIGINT  NOT NULL DEFAULT 1000000,
            product_uom_id     INTEGER REFERENCES uom_uom(id) ON DELETE SET NULL,
            bom_id             INTEGER REFERENCES mrp_bom(id) ON DELETE SET NULL,
            state              VARCHAR NOT NULL DEFAULT 'draft',
            location_src_id    INTEGER REFERENCES stock_location(id) ON DELETE SET NULL,
            location_dest_id   INTEGER REFERENCES stock_location(id) ON DELETE SET NULL,
            date_planned_start TIMESTAMP,
            qty_producing      BIGINT  NOT NULL DEFAULT 0,
            origin             VARCHAR,
            user_id            INTEGER REFERENCES res_users(id)   ON DELETE SET NULL,
            company_id         INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date        TIMESTAMP DEFAULT now(),
            write_date         TIMESTAMP DEFAULT now()
        )
    )");

    // Work orders (behaviour wired in P3, table created now so the
    // MO close-gate can count them from the start).
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS mrp_workorder (
            id                SERIAL PRIMARY KEY,
            production_id     INTEGER NOT NULL REFERENCES mrp_production(id) ON DELETE CASCADE,
            workcenter_id     INTEGER REFERENCES mrp_workcenter(id) ON DELETE SET NULL,
            operation_id      INTEGER REFERENCES mrp_routing_workcenter(id) ON DELETE SET NULL,
            name              VARCHAR NOT NULL,
            sequence          INTEGER NOT NULL DEFAULT 10,
            state             VARCHAR NOT NULL DEFAULT 'pending',
            duration_expected NUMERIC(12,4) NOT NULL DEFAULT 0,
            duration          NUMERIC(12,4) NOT NULL DEFAULT 0,
            qty_produced      BIGINT  NOT NULL DEFAULT 0,
            date_start        TIMESTAMP,
            date_finished     TIMESTAMP,
            company_id        INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date       TIMESTAMP DEFAULT now(),
            write_date        TIMESTAMP DEFAULT now()
        )
    )");

    // stock_move gains manufacturing linkage. A move may belong to an MO
    // instead of a picking, so picking_id becomes optional.
    txn.exec("ALTER TABLE stock_move ALTER COLUMN picking_id DROP NOT NULL");
    txn.exec("ALTER TABLE stock_move ADD COLUMN IF NOT EXISTS production_id INTEGER "
             "REFERENCES mrp_production(id) ON DELETE CASCADE");
    txn.exec("ALTER TABLE stock_move ADD COLUMN IF NOT EXISTS is_production_raw BOOLEAN NOT NULL DEFAULT FALSE");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_stock_move_production ON stock_move(production_id)");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_mrp_workorder_production ON mrp_workorder(production_id)");

    // Subcontracting: a BOM may be fulfilled by an external vendor.
    txn.exec("ALTER TABLE mrp_bom ADD COLUMN IF NOT EXISTS subcontractor_id INTEGER "
             "REFERENCES res_partner(id) ON DELETE SET NULL");

    // ---- Master Production Schedule (P5) ----
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS mrp_production_schedule (
            id               SERIAL PRIMARY KEY,
            product_id       INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
            min_to_replenish BIGINT  NOT NULL DEFAULT 0,
            company_id       INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date      TIMESTAMP DEFAULT now(),
            write_date       TIMESTAMP DEFAULT now(),
            UNIQUE (product_id)
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS mrp_forecast (
            id           SERIAL PRIMARY KEY,
            product_id   INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
            date         DATE   NOT NULL,
            forecast_qty BIGINT NOT NULL DEFAULT 0,
            company_id   INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date  TIMESTAMP DEFAULT now(),
            write_date   TIMESTAMP DEFAULT now(),
            UNIQUE (product_id, date)
        )
    )");

    txn.commit();
}

void MrpModule::seedProductionData_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    // Production virtual location (usage='production') — the transient place
    // components are consumed into and finished goods produced from.
    txn.exec(R"(
        INSERT INTO stock_location (id, name, complete_name, location_id, usage, company_id)
        VALUES (8, 'Production', 'Virtual Locations/Production', 1, 'production', 1)
        ON CONFLICT (id) DO NOTHING
    )");
    // Subcontracting virtual location — where a subcontractor's components are
    // consumed. usage != 'internal', so stock parked there is not counted as
    // company on-hand (it is "out at the vendor").
    txn.exec(R"(
        INSERT INTO stock_location (id, name, complete_name, location_id, usage, company_id)
        VALUES (9, 'Subcontracting', 'Virtual Locations/Subcontracting', 1, 'subcontract', 1)
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec("SELECT setval('stock_location_id_seq', (SELECT MAX(id) FROM stock_location), true)");

    // Manufacturing Order numbering: MO/00001, gapless via ir.sequence.
    txn.exec(R"(
        INSERT INTO ir_sequence (code, name, prefix, padding, reset_policy, number_next)
        VALUES ('mrp.production', 'Manufacturing Order', 'MO/', 5, 'never', 1)
        ON CONFLICT (code) WHERE company_id IS NULL DO NOTHING
    )");

    txn.commit();
}

// ----------------------------------------------------------
// Menus & actions
// ----------------------------------------------------------
void MrpModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    // Window action
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, path, context) VALUES
            (34, 'Bills of Materials', 'mrp.bom', 'list,form', 'bom', '{}')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
                view_mode=EXCLUDED.view_mode, path=EXCLUDED.path, domain=NULL
    )");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");

    // Level 0: Manufacturing app tile (id=110, clear of ReportModule which deletes id=100)
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id, web_icon) VALUES
            (110, 'Manufacturing', NULL, 60, NULL, 'manufacturing')
        ON CONFLICT (id) DO NOTHING
    )");

    // Level 1: Products section under Manufacturing
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (111, 'Products', 110, 10, NULL)
        ON CONFLICT (id) DO NOTHING
    )");

    // Level 2: Bills of Materials leaf under Manufacturing → Products
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (112, 'Bills of Materials', 111, 10, 34)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    // Bills of Materials under Products app (id=50)
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (113, 'Bills of Materials', 50, 20, 34)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    // Bills of Materials under Inventory → Products section (id=96)
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (104, 'Bills of Materials', 96, 20, 34)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    // ---- Manufacturing Orders + Work Centers (P2) ----
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, path, context) VALUES
            (35, 'Manufacturing Orders', 'mrp.production',          'list,form', 'mo',          '{}'),
            (36, 'Work Centers',         'mrp.workcenter',          'list,form', 'workcenters', '{}'),
            (37, 'Work Orders',          'mrp.workorder',           'list',      'workorders',  '{}'),
            (38, 'Master Production Schedule', 'mrp.production.schedule', 'list', 'mps',   '{}')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
                view_mode=EXCLUDED.view_mode, path=EXCLUDED.path
    )");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");

    // Planning → MPS; Operations → MO / Work Orders; Configuration → Work Centers.
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (114, 'Operations',                 110,  5, NULL),
            (115, 'Manufacturing Orders',       114, 10, 35),
            (118, 'Work Orders',                114, 20, 37),
            (119, 'Planning',                   110,  7, NULL),
            (120, 'Master Production Schedule', 119, 10, 38),
            (116, 'Configuration',              110, 90, NULL),
            (117, 'Work Centers',               116, 10, 36)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");
    txn.commit();
}

} // namespace odoo::modules::mrp

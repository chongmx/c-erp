#include "StockModule.hpp"
// =============================================================
// modules/stock/StockModule.hpp
//
// Phase 15 — Stock MVP
//
// 4 models / 4 tables:
//   stock.location      — warehouse/location tree
//   stock.picking.type  — receipt / delivery / internal operation types
//   stock.picking       — transfer header (receipt, delivery, internal)
//   stock.move          — individual product movement line
//
// ViewModels:
//   StockPickingViewModel  — CRUD + action_confirm, action_assign,
//                            button_validate (sets done + updates
//                            sale_order_line.qty_delivered /
//                            purchase_order_line.qty_received)
//   StockMoveViewModel     — GenericViewModel<StockMove>
//   StockLocationViewModel — GenericViewModel<StockLocation>
//   StockPickingTypeViewModel — GenericViewModel<StockPickingType>
//
// Sequences:
//   stock_in_seq  → WH/IN/YYYY/NNNN
//   stock_out_seq → WH/OUT/YYYY/NNNN
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include "BaseModel.hpp"
#include "RecordRuleSql.hpp"
#include "IrSequence.hpp"
#include "IrCron.hpp"
#include "StockQuant.hpp"
#include "DecimalPrecision.hpp"
#include "BaseView.hpp"
#include "BaseViewModel.hpp"
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include "MailHelpers.hpp"
#include "AuditService.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <cmath>

namespace odoo::modules::stock {

// Parses a Many2one field that may arrive as int, string "1", or [1,"Name"] array.
static int parseM2o(const nlohmann::json& j, const std::string& key) {
    if (!j.contains(key)) return 0;
    const auto& v = j[key];
    if (v.is_number_integer())                                  return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer()) return v[0].get<int>();
    if (v.is_string()) { try { return std::stoi(v.get<std::string>()); } catch (...) {} }
    return 0;
}

using namespace odoo::core;
using namespace odoo::infrastructure;

// ================================================================
// 1. MODELS
// ================================================================

// ----------------------------------------------------------------
// StockLocation — stock.location
// ----------------------------------------------------------------
class StockLocation : public BaseModel<StockLocation> {
public:
    ODOO_MODEL("stock.location", "stock_location")

    std::string name;
    std::string completeName;
    int         locationId = 0;   // parent location
    std::string usage      = "internal"; // view|internal|supplier|customer|inventory|transit
    int         companyId  = 0;
    bool        active     = true;
    std::string barcode;

    explicit StockLocation(std::shared_ptr<DbConnection> db)
        : BaseModel<StockLocation>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",          FieldType::Char,    "Location Name", true});
        fieldRegistry_.add({"complete_name",  FieldType::Char,    "Complete Name"});
        fieldRegistry_.add({"location_id",   FieldType::Many2one,"Parent",        false, false, true, false, "stock.location"});
        fieldRegistry_.add({"usage",         FieldType::Char,    "Usage"});
        fieldRegistry_.add({"company_id",    FieldType::Many2one,"Company",       false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",        FieldType::Boolean, "Active"});
        fieldRegistry_.add({"barcode",       FieldType::Char,    "Barcode"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]          = name;
        j["complete_name"] = completeName.empty() ? name : completeName;
        j["location_id"]   = locationId > 0 ? nlohmann::json(locationId) : nlohmann::json(false);
        j["usage"]         = usage;
        j["company_id"]    = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["active"]        = active;
        j["barcode"]       = barcode.empty() ? nlohmann::json(false) : nlohmann::json(barcode);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")          && j["name"].is_string())          name         = j["name"].get<std::string>();
        if (j.contains("complete_name") && j["complete_name"].is_string()) completeName = j["complete_name"].get<std::string>();
        if (j.contains("usage")         && j["usage"].is_string())         usage        = j["usage"].get<std::string>();
        if (j.contains("active")        && j["active"].is_boolean())       active       = j["active"].get<bool>();
        if (j.contains("barcode")       && j["barcode"].is_string())       barcode      = j["barcode"].get<std::string>();
        if (const int v = parseM2o(j, "location_id"))  locationId = v;
        if (const int v = parseM2o(j, "company_id"))   companyId  = v;
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Location name is required");
        return e;
    }
};

// ----------------------------------------------------------------
// StockPickingType — stock.picking.type
// ----------------------------------------------------------------
class StockPickingType : public BaseModel<StockPickingType> {
public:
    ODOO_MODEL("stock.picking.type", "stock_picking_type")

    std::string name;
    std::string code;             // incoming | outgoing | internal
    std::string sequencePrefix = "WH/";
    int         defaultLocationSrcId  = 0;
    int         defaultLocationDestId = 0;
    int         companyId  = 0;
    bool        active     = true;

    explicit StockPickingType(std::shared_ptr<DbConnection> db)
        : BaseModel<StockPickingType>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",                     FieldType::Char,    "Operation Type", true});
        fieldRegistry_.add({"code",                     FieldType::Char,    "Type Code",      true});
        fieldRegistry_.add({"sequence_prefix",          FieldType::Char,    "Sequence Prefix"});
        fieldRegistry_.add({"default_location_src_id",  FieldType::Many2one,"Source Location",      false, false, true, false, "stock.location"});
        fieldRegistry_.add({"default_location_dest_id", FieldType::Many2one,"Destination Location", false, false, true, false, "stock.location"});
        fieldRegistry_.add({"company_id",               FieldType::Many2one,"Company",              false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",                   FieldType::Boolean, "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]                     = name;
        j["code"]                     = code;
        j["sequence_prefix"]          = sequencePrefix;
        j["default_location_src_id"]  = defaultLocationSrcId  > 0 ? nlohmann::json(defaultLocationSrcId)  : nlohmann::json(false);
        j["default_location_dest_id"] = defaultLocationDestId > 0 ? nlohmann::json(defaultLocationDestId) : nlohmann::json(false);
        j["company_id"]               = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["active"]                   = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")            && j["name"].is_string())            name           = j["name"].get<std::string>();
        if (j.contains("code")            && j["code"].is_string())            code           = j["code"].get<std::string>();
        if (j.contains("sequence_prefix") && j["sequence_prefix"].is_string()) sequencePrefix = j["sequence_prefix"].get<std::string>();
        if (j.contains("active")          && j["active"].is_boolean())         active         = j["active"].get<bool>();
        if (const int v = parseM2o(j, "default_location_src_id"))  defaultLocationSrcId  = v;
        if (const int v = parseM2o(j, "default_location_dest_id")) defaultLocationDestId = v;
        if (const int v = parseM2o(j, "company_id"))               companyId             = v;
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Operation type name is required");
        if (code.empty()) e.push_back("Code is required");
        return e;
    }
};

// ----------------------------------------------------------------
// StockPicking — stock.picking
// ----------------------------------------------------------------
class StockPicking : public BaseModel<StockPicking> {
public:
    ODOO_MODEL("stock.picking", "stock_picking")

    std::string name           = "New";
    int         pickingTypeId  = 0;
    std::string state          = "draft"; // draft|confirmed|assigned|done|cancel
    int         partnerId      = 0;
    int         locationId     = 0;
    int         locationDestId = 0;
    std::string scheduledDate;
    std::string origin;
    int         companyId  = 0;
    int         saleId     = 0;
    int         purchaseId = 0;
    int         userId     = 0;

    explicit StockPicking(std::shared_ptr<DbConnection> db)
        : BaseModel<StockPicking>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",             FieldType::Char,    "Reference"});
        fieldRegistry_.add({"picking_type_id",  FieldType::Many2one,"Operation Type", true, false, true, false, "stock.picking.type"});
        fieldRegistry_.add({"state",            FieldType::Char,    "Status"});
        fieldRegistry_.add({"partner_id",       FieldType::Many2one,"Contact",        false, false, true, false, "res.partner"});
        fieldRegistry_.add({"location_id",      FieldType::Many2one,"From",           true, false, true, false, "stock.location"});
        fieldRegistry_.add({"location_dest_id", FieldType::Many2one,"To",             true, false, true, false, "stock.location"});
        fieldRegistry_.add({"scheduled_date",   FieldType::Char,    "Scheduled Date"});
        fieldRegistry_.add({"origin",           FieldType::Char,    "Source Document"});
        fieldRegistry_.add({"company_id",       FieldType::Many2one,"Company",        false, false, true, false, "res.company"});
        fieldRegistry_.add({"sale_id",          FieldType::Many2one,"Sale Order",     false, false, true, false, "sale.order"});
        fieldRegistry_.add({"purchase_id",      FieldType::Many2one,"Purchase Order", false, false, true, false, "purchase.order"});
        fieldRegistry_.add({"user_id",          FieldType::Many2one,"Responsible",    false, false, true, false, "res.users"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]             = name;
        j["picking_type_id"]  = pickingTypeId  > 0 ? nlohmann::json(pickingTypeId)  : nlohmann::json(false);
        j["state"]            = state;
        j["partner_id"]       = partnerId      > 0 ? nlohmann::json(partnerId)      : nlohmann::json(false);
        j["location_id"]      = locationId     > 0 ? nlohmann::json(locationId)     : nlohmann::json(false);
        j["location_dest_id"] = locationDestId > 0 ? nlohmann::json(locationDestId) : nlohmann::json(false);
        j["scheduled_date"]   = scheduledDate.empty() ? nlohmann::json(false) : nlohmann::json(scheduledDate);
        j["origin"]           = origin.empty()        ? nlohmann::json(false) : nlohmann::json(origin);
        j["company_id"]       = companyId  > 0 ? nlohmann::json(companyId)  : nlohmann::json(false);
        j["sale_id"]          = saleId     > 0 ? nlohmann::json(saleId)     : nlohmann::json(false);
        j["purchase_id"]      = purchaseId > 0 ? nlohmann::json(purchaseId) : nlohmann::json(false);
        j["user_id"]          = userId     > 0 ? nlohmann::json(userId)     : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")           && j["name"].is_string())           name          = j["name"].get<std::string>();
        if (j.contains("state")          && j["state"].is_string())          state         = j["state"].get<std::string>();
        if (j.contains("scheduled_date") && j["scheduled_date"].is_string()) scheduledDate = j["scheduled_date"].get<std::string>();
        if (j.contains("origin")         && j["origin"].is_string())         origin        = j["origin"].get<std::string>();
        if (const int v = parseM2o(j, "picking_type_id"))  pickingTypeId  = v;
        if (const int v = parseM2o(j, "partner_id"))       partnerId      = v;
        if (const int v = parseM2o(j, "location_id"))      locationId     = v;
        if (const int v = parseM2o(j, "location_dest_id")) locationDestId = v;
        if (const int v = parseM2o(j, "company_id"))       companyId      = v;
        if (const int v = parseM2o(j, "sale_id"))          saleId         = v;
        if (const int v = parseM2o(j, "purchase_id"))      purchaseId     = v;
        if (const int v = parseM2o(j, "user_id"))          userId         = v;
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (pickingTypeId  <= 0) e.push_back("Operation type is required");
        if (locationId     <= 0) e.push_back("Source location is required");
        if (locationDestId <= 0) e.push_back("Destination location is required");
        return e;
    }
};

// ----------------------------------------------------------------
// StockMove — stock.move
// ----------------------------------------------------------------
class StockMove : public BaseModel<StockMove> {
public:
    ODOO_MODEL("stock.move", "stock_move")

    int         pickingId      = 0;
    int         productId      = 0;
    int         productUomId   = 0;
    std::string name;
    double      productUomQty  = 0.0; // demand
    double      quantity       = 0.0; // done
    std::string state          = "draft";
    int         locationId     = 0;
    int         locationDestId = 0;
    int         companyId      = 0;
    std::string origin;

    explicit StockMove(std::shared_ptr<DbConnection> db)
        : BaseModel<StockMove>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"picking_id",       FieldType::Many2one,"Transfer",          true,  false, true,  false, "stock.picking"});
        fieldRegistry_.add({"product_id",       FieldType::Many2one,"Product",           true,  false, true,  false, "product.product"});
        fieldRegistry_.add({"product_uom_id",   FieldType::Many2one,"Unit of Measure",   false, false, true,  false, "uom.uom"});
        fieldRegistry_.add({"name",             FieldType::Char,    "Description",       true});
        fieldRegistry_.add({"product_uom_qty",  FieldType::Float,   "Demand"});
        fieldRegistry_.add({"quantity",         FieldType::Float,   "Done"});
        fieldRegistry_.add({"state",            FieldType::Char,    "Status"});
        fieldRegistry_.add({"location_id",      FieldType::Many2one,"From",              true,  false, true,  false, "stock.location"});
        fieldRegistry_.add({"location_dest_id", FieldType::Many2one,"To",               true,  false, true,  false, "stock.location"});
        fieldRegistry_.add({"company_id",       FieldType::Many2one,"Company",           false, false, true,  false, "res.company"});
        fieldRegistry_.add({"origin",           FieldType::Char,    "Source Document"});
        fieldRegistry_.setPrecision(core::DecimalPrecision::kStock,
                                    {"product_uom_qty", "quantity"});
        fieldRegistry_.markScaled({"product_uom_qty", "quantity"});   // P2: migration 940
    }

    void serializeFields(nlohmann::json& j) const override {
        j["picking_id"]      = pickingId     > 0 ? nlohmann::json(pickingId)     : nlohmann::json(false);
        j["product_id"]      = productId     > 0 ? nlohmann::json(productId)     : nlohmann::json(false);
        j["product_uom_id"]  = productUomId  > 0 ? nlohmann::json(productUomId)  : nlohmann::json(false);
        j["name"]            = name;
        j["product_uom_qty"] = productUomQty;
        j["quantity"]        = quantity;
        j["state"]           = state;
        j["location_id"]     = locationId     > 0 ? nlohmann::json(locationId)     : nlohmann::json(false);
        j["location_dest_id"]= locationDestId > 0 ? nlohmann::json(locationDestId) : nlohmann::json(false);
        j["company_id"]      = companyId      > 0 ? nlohmann::json(companyId)      : nlohmann::json(false);
        j["origin"]          = origin.empty() ? nlohmann::json(false) : nlohmann::json(origin);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")   && j["name"].is_string())   name   = j["name"].get<std::string>();
        if (j.contains("state")  && j["state"].is_string())  state  = j["state"].get<std::string>();
        if (j.contains("origin") && j["origin"].is_string()) origin = j["origin"].get<std::string>();
        if (j.contains("product_uom_qty") && j["product_uom_qty"].is_number()) productUomQty = j["product_uom_qty"].get<double>();
        if (j.contains("quantity")        && j["quantity"].is_number())        quantity      = j["quantity"].get<double>();
        if (const int v = parseM2o(j, "picking_id"))       pickingId      = v;
        if (const int v = parseM2o(j, "product_id"))       productId      = v;
        if (const int v = parseM2o(j, "product_uom_id"))   productUomId   = v;
        if (const int v = parseM2o(j, "location_id"))      locationId     = v;
        if (const int v = parseM2o(j, "location_dest_id")) locationDestId = v;
        if (const int v = parseM2o(j, "company_id"))       companyId      = v;
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (pickingId  <= 0) e.push_back("Transfer is required");
        if (productId  <= 0) e.push_back("Product is required");
        if (name.empty())    e.push_back("Description is required");
        return e;
    }
};


// ----------------------------------------------------------------
// StockWarehouse — stock.warehouse
// ----------------------------------------------------------------
class StockWarehouse : public BaseModel<StockWarehouse> {
public:
    ODOO_MODEL("stock.warehouse", "stock_warehouse")

    std::string name, code;
    int         companyId       = 0;
    int         lotStockId      = 0;   // main stock location (WH/Stock)
    int         viewLocationId  = 0;   // parent view location (WH)
    int         inTypeId        = 0;   // receipt picking type
    int         outTypeId       = 0;   // delivery picking type
    int         intTypeId       = 0;   // internal picking type
    bool        active          = true;

    explicit StockWarehouse(std::shared_ptr<DbConnection> db)
        : BaseModel<StockWarehouse>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",             FieldType::Char,    "Warehouse",          true});
        fieldRegistry_.add({"code",             FieldType::Char,    "Short Name",         true});
        fieldRegistry_.add({"company_id",       FieldType::Many2one,"Company",            false, false, true, false, "res.company"});
        fieldRegistry_.add({"lot_stock_id",     FieldType::Many2one,"Location Stock",     false, false, true, false, "stock.location"});
        fieldRegistry_.add({"view_location_id", FieldType::Many2one,"View Location",      false, false, true, false, "stock.location"});
        fieldRegistry_.add({"in_type_id",       FieldType::Many2one,"Receipts",           false, false, true, false, "stock.picking.type"});
        fieldRegistry_.add({"out_type_id",      FieldType::Many2one,"Deliveries",         false, false, true, false, "stock.picking.type"});
        fieldRegistry_.add({"int_type_id",      FieldType::Many2one,"Internal Transfers", false, false, true, false, "stock.picking.type"});
        fieldRegistry_.add({"active",           FieldType::Boolean, "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]             = name;
        j["code"]             = code;
        j["company_id"]       = companyId      > 0 ? nlohmann::json(companyId)      : nlohmann::json(false);
        j["lot_stock_id"]     = lotStockId     > 0 ? nlohmann::json(lotStockId)     : nlohmann::json(false);
        j["view_location_id"] = viewLocationId > 0 ? nlohmann::json(viewLocationId) : nlohmann::json(false);
        j["in_type_id"]       = inTypeId       > 0 ? nlohmann::json(inTypeId)       : nlohmann::json(false);
        j["out_type_id"]      = outTypeId      > 0 ? nlohmann::json(outTypeId)      : nlohmann::json(false);
        j["int_type_id"]      = intTypeId      > 0 ? nlohmann::json(intTypeId)      : nlohmann::json(false);
        j["active"]           = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("code") && j["code"].is_string()) code = j["code"].get<std::string>();
        if (j.contains("active") && j["active"].is_boolean()) active = j["active"].get<bool>();
        if (const int v = parseM2o(j, "company_id"))       companyId      = v;
        if (const int v = parseM2o(j, "lot_stock_id"))     lotStockId     = v;
        if (const int v = parseM2o(j, "view_location_id")) viewLocationId = v;
        if (const int v = parseM2o(j, "in_type_id"))       inTypeId       = v;
        if (const int v = parseM2o(j, "out_type_id"))      outTypeId      = v;
        if (const int v = parseM2o(j, "int_type_id"))      intTypeId      = v;
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Warehouse name is required");
        if (code.empty()) e.push_back("Short name is required");
        return e;
    }
};


// ================================================================
// 2. VIEWMODELS
// ================================================================

// ----------------------------------------------------------------
// StockPickingViewModel — CRUD + workflow actions
// ----------------------------------------------------------------
class StockPickingViewModel : public BaseViewModel {
public:
    explicit StockPickingViewModel(std::shared_ptr<DbConnection> db)
        : db_(std::move(db))
    {
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
        REGISTER_METHOD("action_confirm",       handleActionConfirm)
        REGISTER_METHOD("action_assign",        handleActionAssign)
        REGISTER_METHOD("button_validate",      handleButtonValidate)
        REGISTER_METHOD("action_cancel",        handleActionCancel)
        REGISTER_METHOD("button_unreserve",     handleButtonUnreserve)
        REGISTER_METHOD("button_reset_to_draft",handleButtonResetToDraft)
        REGISTER_METHOD("default_get",          handleDefaultGet)
    }

    std::string modelName() const override { return "stock.picking"; }

private:
    std::shared_ptr<DbConnection> db_;

    // Custom search_read: JOINs location and partner names so the list view
    // can display them as [id, "Name"] arrays (formatCell handles those).
    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        int lim = call.limit() > 0 ? call.limit() : 80;
        int off = call.offset();

        // S-49: a custom search_read bypasses BaseModel's allowlist, so restrict
        // the domain columns to this model's stored fields explicitly.
        static const std::set<std::string> kCols = {
            "id","name","picking_type_id","state","partner_id","location_id",
            "location_dest_id","scheduled_date","origin","company_id",
            "sale_id","purchase_id","user_id"};
        auto [where, paramVec] = domainFromJson(call.domain()).toSql(&kCols);

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        std::string sql = R"(
            SELECT sp.id,
                   sp.name,
                   sp.state,
                   sp.origin,
                   sp.scheduled_date,
                   sp.location_id,
                   COALESCE(sl_src.complete_name, sl_src.name) AS location_name,
                   sp.location_dest_id,
                   COALESCE(sl_dst.complete_name, sl_dst.name) AS location_dest_name,
                   sp.partner_id,
                   rp.name AS partner_name,
                   sp.purchase_id,
                   sp.sale_id
            FROM stock_picking sp
            LEFT JOIN stock_location sl_src ON sl_src.id = sp.location_id
            LEFT JOIN stock_location sl_dst ON sl_dst.id = sp.location_dest_id
            LEFT JOIN res_partner    rp     ON rp.id     = sp.partner_id
            WHERE )";
        sql += where;
        // S-30: this custom read bypasses BaseModel, so enforce ir.rule here
        // (record-rule bypass fix, 071 §1.2) — as an id-subquery, alias-safe.
        pqxx::params p; for (auto& s : paramVec) p.append(s);
        core::appendRecordRuleSubquery(sql, p, "stock.picking", core::RuleOp::Read,
                                       extractContext_(call), "stock_picking", "sp.id",
                                       static_cast<int>(paramVec.size()));
        sql += " ORDER BY sp.id DESC";
        sql += " LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);

        pqxx::result res = txn.exec(sql, p);

        auto m2o = [](const pqxx::row& row,
                      const char* idCol, const char* nameCol) -> nlohmann::json {
            if (row[idCol].is_null()) return false;
            nlohmann::json pair = nlohmann::json::array();
            pair.push_back(row[idCol].as<int>());
            pair.push_back(row[nameCol].is_null() ? "" : std::string(row[nameCol].c_str()));
            return pair;
        };

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json obj;
            obj["id"]             = row["id"].as<int>();
            obj["name"]           = row["name"].is_null()           ? nlohmann::json(false) : nlohmann::json(row["name"].c_str());
            obj["state"]          = row["state"].is_null()          ? nlohmann::json(false) : nlohmann::json(row["state"].c_str());
            obj["origin"]         = row["origin"].is_null()         ? nlohmann::json(false) : nlohmann::json(row["origin"].c_str());
            obj["scheduled_date"] = row["scheduled_date"].is_null() ? nlohmann::json(false) : nlohmann::json(row["scheduled_date"].c_str());
            obj["location_id"]      = m2o(row, "location_id",      "location_name");
            obj["location_dest_id"] = m2o(row, "location_dest_id", "location_dest_name");
            obj["partner_id"]       = m2o(row, "partner_id",       "partner_name");
            arr.push_back(std::move(obj));
        }
        return arr;
    }
    nlohmann::json handleRead(const CallKwArgs& call) {
        StockPicking proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.read(call.ids(), call.fields());
    }
    nlohmann::json handleCreate(const CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("create: args[0] must be a dict");
        StockPicking proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const int newId = proto.create(v);
        return newId;
    }
    nlohmann::json handleWrite(const CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        StockPicking proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const auto result = proto.write(call.ids(), v);
        return result;
    }
    nlohmann::json handleUnlink(const CallKwArgs& call) {
        StockPicking proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const auto ids = call.ids();
        const auto result = proto.unlink(ids);
        return result;
    }
    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        StockPicking proto(db_);
        return proto.fieldsGet(call.fields());  // schema metadata — no rules needed
    }
    nlohmann::json handleSearchCount(const CallKwArgs& call) {
        StockPicking proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.searchCount(call.domain());
    }
    nlohmann::json handleSearch(const CallKwArgs& call) {
        StockPicking proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.search(call.domain(),
                            call.limit() > 0 ? call.limit() : 80,
                            call.offset());
    }

    // ----------------------------------------------------------
    // default_get — sensible defaults for new stock.picking records
    // ----------------------------------------------------------
    nlohmann::json handleDefaultGet(const CallKwArgs& /*call*/) {
        // Read the first available picking type to pre-fill locations
        try {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto r = txn.exec(
                "SELECT id, default_location_src_id, default_location_dest_id "
                "FROM stock_picking_type WHERE active=TRUE ORDER BY id LIMIT 1");
            if (!r.empty()) {
                return {
                    {"picking_type_id",  r[0]["id"].as<int>()},
                    {"location_id",      r[0]["default_location_src_id"].is_null()  ? 5 : r[0]["default_location_src_id"].as<int>()},
                    {"location_dest_id", r[0]["default_location_dest_id"].is_null() ? 4 : r[0]["default_location_dest_id"].as<int>()},
                    {"company_id",       1},
                    {"state",            "draft"},
                };
            }
        } catch (...) {}
        return {{"state", "draft"}, {"company_id", 1}};
    }

    // ----------------------------------------------------------
    // action_confirm — draft → confirmed, assign sequence name
    // ----------------------------------------------------------
    nlohmann::json handleActionConfirm(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};

            // Only draft pickings can be confirmed
            auto r = txn.exec(
                "SELECT state, picking_type_id FROM stock_picking WHERE id=$1",
                pqxx::params{id});
            if (r.empty()) continue;
            const std::string state = r[0]["state"].c_str();
            if (state != "draft") continue;

            const int ptId = r[0]["picking_type_id"].as<int>();

            // Determine sequence to use based on picking type code
            auto pt = txn.exec(
                "SELECT code, sequence_prefix FROM stock_picking_type WHERE id=$1",
                pqxx::params{ptId});
            // P4: one ir.sequence per picking direction. The prefix and
            // padding now live in the sequence record rather than here, so an
            // operator can change "WH/OUT/" without a code change.
            std::string seqCode = "stock.picking.in";
            if (!pt.empty()) {
                const std::string code = pt[0]["code"].c_str();
                if      (code == "outgoing") seqCode = "stock.picking.out";
                else if (code == "internal") seqCode = "stock.picking.int";
            }
            const std::string ref =
                core::IrSequence::instance().nextByCode(txn, seqCode);

            txn.exec(
                "UPDATE stock_picking SET state='confirmed', name=$1, write_date=now() WHERE id=$2",
                pqxx::params{ref, id});

            // Also confirm all child moves
            txn.exec(
                "UPDATE stock_move SET state='confirmed' WHERE picking_id=$1 AND state='draft'",
                pqxx::params{id});

            odoo::modules::mail::postLog(txn, "stock.picking", id, 0,
                "Transfer confirmed.", "log_note");
            txn.commit();
        }
        if (AuditService::ready() && !call.ids().empty())
            AuditService::instance().log("stock.picking", "action_confirm",
                                         call.ids(), extractContext_(call).uid);
        return true;
    }

    // ----------------------------------------------------------
    // action_assign — confirmed → assigned, reserving real on-hand.
    // Each move reserves up to its demand at the source location through
    // the quant engine; a move that cannot be fully reserved is left
    // 'partially_available' (and so is the picking).
    // ----------------------------------------------------------
    nlohmann::json handleActionAssign(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto moves = txn.exec(
                "SELECT id, product_id, location_id, product_uom_qty, company_id, "
                "       COALESCE(lot_id,0) AS lot_id "
                "FROM stock_move "
                "WHERE picking_id=$1 AND state IN ('confirmed','partially_available')",
                pqxx::params{id});
            bool allFull = !moves.empty();
            for (const auto& m : moves) {
                const int       moveId  = m["id"].as<int>();
                const int       prod    = m["product_id"].as<int>();
                const int       src     = m["location_id"].as<int>();
                const int       lot     = m["lot_id"].as<int>(0);
                const long long demand  = m["product_uom_qty"].as<long long>(0);
                const int       comp    = m["company_id"].is_null() ? 0 : m["company_id"].as<int>();
                const long long already = txn.exec(
                    "SELECT reserved_qty FROM stock_move WHERE id=$1",
                    pqxx::params{moveId})[0][0].as<long long>(0);
                const long long need    = demand - already;
                const long long got     = need > 0
                    ? core::StockQuant::reserve(txn, prod, src, need, comp, lot) : 0;
                const long long total   = already + got;
                const bool      full    = total >= demand;
                if (!full) allFull = false;
                txn.exec(
                    "UPDATE stock_move SET reserved_qty=$1, state=$2, write_date=now() "
                    "WHERE id=$3",
                    pqxx::params{total, full ? "assigned" : "partially_available", moveId});
            }
            txn.exec(
                "UPDATE stock_picking SET state=$1, write_date=now() "
                "WHERE id=$2 AND state IN ('confirmed','partially_available')",
                pqxx::params{allFull ? "assigned" : "partially_available", id});
            txn.commit();
        }
        if (AuditService::ready() && !call.ids().empty())
            AuditService::instance().log("stock.picking", "action_assign",
                                         call.ids(), extractContext_(call).uid);
        return true;
    }

    // ----------------------------------------------------------
    // button_validate — mark done + update SO/PO line quantities
    // ----------------------------------------------------------
    nlohmann::json handleButtonValidate(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};

            // Load picking
            auto pr = txn.exec(
                "SELECT state, picking_type_id, sale_id, purchase_id "
                "FROM stock_picking WHERE id=$1",
                pqxx::params{id});
            if (pr.empty()) continue;
            const std::string curState = pr[0]["state"].c_str();
            if (curState == "done" || curState == "cancel") continue;

            const int saleId     = pr[0]["sale_id"].is_null()     ? 0 : pr[0]["sale_id"].as<int>();
            const int purchaseId = pr[0]["purchase_id"].is_null() ? 0 : pr[0]["purchase_id"].as<int>();

            // Get picking type code
            const int ptId = pr[0]["picking_type_id"].as<int>();
            auto ptRow = txn.exec(
                "SELECT code FROM stock_picking_type WHERE id=$1", pqxx::params{ptId});
            const std::string code = ptRow.empty() ? "" : ptRow[0]["code"].c_str();

            // Apply each move to the quant ledger. Done qty defaults to the
            // demand when the operator left it at 0. Any reservation this move
            // held is released first, so it is not double-counted, then the
            // real quantity flows source → destination through the engine.
            auto moves = txn.exec(
                "SELECT sm.id, sm.product_id, sm.location_id, sm.location_dest_id, "
                "       sm.product_uom_qty, sm.quantity, sm.reserved_qty, sm.company_id, "
                "       COALESCE(sm.lot_id,0) AS lot_id, COALESCE(pp.tracking,'none') AS tracking "
                "FROM stock_move sm JOIN product_product pp ON pp.id = sm.product_id "
                "WHERE sm.picking_id=$1 AND sm.state NOT IN ('done','cancel')",
                pqxx::params{id});
            for (const auto& m : moves) {
                const int         moveId   = m["id"].as<int>();
                const int         prod     = m["product_id"].as<int>();
                const int         src      = m["location_id"].as<int>();
                int               dest     = m["location_dest_id"].as<int>();
                const int         lot      = m["lot_id"].as<int>(0);
                const std::string track    = m["tracking"].is_null() ? "none" : m["tracking"].c_str();
                const long long   demand   = m["product_uom_qty"].as<long long>(0);
                // Putaway: redirect a move arriving at an internal location to
                // its designated sub-location. Product-specific rule wins over a
                // category rule; both must store to an internal location.
                {
                    auto pr = txn.exec(
                        "SELECT r.location_out_id FROM stock_putaway_rule r "
                        "JOIN stock_location il ON il.id=r.location_in_id AND il.usage='internal' "
                        "JOIN stock_location ol ON ol.id=r.location_out_id AND ol.usage='internal' "
                        "WHERE r.location_in_id=$1 AND "
                        "  (r.product_id=$2 OR (r.product_id IS NULL AND r.category_id="
                        "     (SELECT categ_id FROM product_product WHERE id=$2))) "
                        "ORDER BY (r.product_id IS NOT NULL) DESC, r.sequence, r.id LIMIT 1",
                        pqxx::params{dest, prod});
                    if (!pr.empty() && !pr[0][0].is_null()) {
                        const int newDest = pr[0][0].as<int>();
                        if (newDest != dest) {
                            dest = newDest;
                            txn.exec("UPDATE stock_move SET location_dest_id=$1 WHERE id=$2",
                                     pqxx::params{dest, moveId});
                        }
                    }
                }
                const long long   doneRaw  = m["quantity"].as<long long>(0);
                const long long   reserved = m["reserved_qty"].as<long long>(0);
                const int         comp     = m["company_id"].is_null() ? 0 : m["company_id"].as<int>();
                const long long   done     = doneRaw > 0 ? doneRaw : demand;
                // Lots/serial enforcement: a tracked product needs a lot, and a
                // serial move is exactly one unit.
                if ((track == "lot" || track == "serial") && lot <= 0)
                    throw std::runtime_error("A lot/serial number is required for this product");
                if (track == "serial" && done != 1000000)
                    throw std::runtime_error("A serial-tracked move must be exactly one unit");
                if (reserved > 0)
                    core::StockQuant::release(txn, prod, src, reserved, lot);
                // Value a purchase receipt at its PO price, so average/FIFO
                // costing blends the real purchase cost (not standard_price).
                long long inCost = -1;
                if (purchaseId > 0) {
                    auto pl = txn.exec(
                        "SELECT price_unit FROM purchase_order_line "
                        "WHERE order_id=$1 AND product_id=$2 ORDER BY id LIMIT 1",
                        pqxx::params{purchaseId, prod});
                    if (!pl.empty() && !pl[0][0].is_null()) inCost = pl[0][0].as<long long>(0);
                }
                core::StockQuant::applyMove(txn, prod, src, dest, done, comp, inCost, lot);
                txn.exec(
                    "UPDATE stock_move SET quantity=$1, reserved_qty=0, "
                    "state='done', write_date=now() WHERE id=$2",
                    pqxx::params{done, moveId});
            }

            // Mark picking done
            txn.exec(
                "UPDATE stock_picking SET state='done', write_date=now() WHERE id=$1",
                pqxx::params{id});

            // Update linked sale order lines qty_delivered
            if (saleId > 0 && code == "outgoing") {
                auto moves = txn.exec(
                    "SELECT product_id, SUM(quantity) AS qty "
                    "FROM stock_move WHERE picking_id=$1 AND state='done' "
                    "GROUP BY product_id",
                    pqxx::params{id});
                for (const auto& m : moves) {
                    const int    productId = m["product_id"].as<int>();
                    const double qty       = m["qty"].as<double>();
                    txn.exec(
                        "UPDATE sale_order_line "
                        "SET qty_delivered = qty_delivered + $1, write_date=now() "
                        "WHERE order_id=$2 AND product_id=$3",
                        pqxx::params{qty, saleId, productId});
                }
                // Re-evaluate sale order invoice_status
                txn.exec(R"(
                    UPDATE sale_order so
                    SET invoice_status = CASE
                        WHEN (SELECT COUNT(*) FROM sale_order_line
                              WHERE order_id=so.id AND qty_delivered < product_uom_qty) = 0
                        THEN 'to_invoice'
                        ELSE invoice_status
                    END,
                    write_date=now()
                    WHERE id=$1 AND state='sale'
                )", pqxx::params{saleId});
            }

            // Update linked purchase order lines qty_received
            if (purchaseId > 0 && code == "incoming") {
                auto moves = txn.exec(
                    "SELECT product_id, SUM(quantity) AS qty "
                    "FROM stock_move WHERE picking_id=$1 AND state='done' "
                    "GROUP BY product_id",
                    pqxx::params{id});
                for (const auto& m : moves) {
                    const int    productId = m["product_id"].as<int>();
                    const double qty       = m["qty"].as<double>();
                    txn.exec(
                        "UPDATE purchase_order_line "
                        "SET qty_received = qty_received + $1, write_date=now() "
                        "WHERE order_id=$2 AND product_id=$3",
                        pqxx::params{qty, purchaseId, productId});
                }
                // Re-evaluate purchase order invoice_status
                txn.exec(R"(
                    UPDATE purchase_order po
                    SET invoice_status = CASE
                        WHEN (SELECT COUNT(*) FROM purchase_order_line
                              WHERE order_id=po.id AND qty_received < product_qty) = 0
                        THEN 'to_bill'
                        ELSE invoice_status
                    END,
                    write_date=now()
                    WHERE id=$1 AND state='purchase'
                )", pqxx::params{purchaseId});
            }

            // --- Subcontracting backflush ---
            // On an incoming receipt, any received product that has a
            // subcontract BOM is treated as manufactured by the vendor: the
            // finished good's own receipt move (applied above) put it on hand,
            // and here we consume its components from WH/Stock into the
            // Subcontracting location — the receipt IS the manufacturing event.
            // (SQL-only touch of the mrp_bom tables; no module dependency.)
            if (code == "incoming") {
                auto subLocR = txn.exec(
                    "SELECT id FROM stock_location WHERE usage='subcontract' ORDER BY id LIMIT 1");
                const int subLoc = subLocR.empty() ? 0 : subLocR[0][0].as<int>();
                if (subLoc > 0) {
                    auto received = txn.exec(
                        "SELECT product_id, SUM(quantity) AS qty, MAX(company_id) AS company_id "
                        "FROM stock_move WHERE picking_id=$1 AND state='done' GROUP BY product_id",
                        pqxx::params{id});
                    for (const auto& rm : received) {
                        const int       prodF   = rm["product_id"].as<int>();
                        const long long recvQty = rm["qty"].as<long long>(0);
                        const int       comp    = rm["company_id"].is_null() ? 0 : rm["company_id"].as<int>();
                        auto b = txn.exec(
                            "SELECT id, product_qty FROM mrp_bom "
                            "WHERE product_id=$1 AND bom_type='subcontract' AND active=TRUE ORDER BY id LIMIT 1",
                            pqxx::params{prodF});
                        if (b.empty()) continue;
                        const int       bomId  = b[0]["id"].as<int>();
                        long long       bomQty = b[0]["product_qty"].as<long long>(1000000);
                        if (bomQty <= 0) bomQty = 1000000;
                        auto lines = txn.exec(
                            "SELECT product_id, (product_qty::numeric * $2 / $3)::bigint AS q "
                            "FROM mrp_bom_line WHERE bom_id=$1",
                            pqxx::params{bomId, recvQty, bomQty});
                        for (const auto& ln : lines) {
                            const int       compProd = ln["product_id"].as<int>();
                            const long long q        = ln["q"].as<long long>(0);
                            if (q <= 0) continue;
                            txn.exec(
                                "INSERT INTO stock_move (product_id, name, product_uom_qty, quantity, "
                                "  state, location_id, location_dest_id, company_id, origin, picking_id) "
                                "VALUES ($1, 'Subcontract component', $2, $2, 'done', 4, $3, NULLIF($4,0), 'Subcontract', $5)",
                                pqxx::params{compProd, q, subLoc, comp, id});
                            core::StockQuant::applyMove(txn, compProd, 4, subLoc, q, comp);
                        }
                    }
                }
            }

            odoo::modules::mail::postLog(txn, "stock.picking", id, 0,
                "Transfer validated.", "log_note");
            txn.commit();
        }
        if (AuditService::ready() && !call.ids().empty())
            AuditService::instance().log("stock.picking", "button_validate",
                                         call.ids(), extractContext_(call).uid);
        return true;
    }

    // ----------------------------------------------------------
    // action_cancel
    // ----------------------------------------------------------
    nlohmann::json handleActionCancel(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            // Free any stock this picking had reserved before cancelling.
            auto held = txn.exec(
                "SELECT product_id, location_id, reserved_qty, COALESCE(lot_id,0) AS lot_id FROM stock_move "
                "WHERE picking_id=$1 AND state NOT IN ('done','cancel') AND reserved_qty > 0",
                pqxx::params{id});
            for (const auto& m : held) {
                core::StockQuant::release(txn, m["product_id"].as<int>(),
                                          m["location_id"].as<int>(),
                                          m["reserved_qty"].as<long long>(0),
                                          m["lot_id"].as<int>(0));
            }
            txn.exec(
                "UPDATE stock_picking SET state='cancel', write_date=now() "
                "WHERE id=$1 AND state != 'done'",
                pqxx::params{id});
            txn.exec(
                "UPDATE stock_move SET state='cancel', reserved_qty=0 "
                "WHERE picking_id=$1 AND state != 'done'",
                pqxx::params{id});
            txn.commit();
        }
        if (AuditService::ready() && !call.ids().empty())
            AuditService::instance().log("stock.picking", "action_cancel",
                                         call.ids(), extractContext_(call).uid);
        return true;
    }

    // ----------------------------------------------------------
    // button_unreserve — assigned → confirmed (release reservation)
    // ----------------------------------------------------------
    nlohmann::json handleButtonUnreserve(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            // Release each move's held reservation back to the quant.
            auto moves = txn.exec(
                "SELECT id, product_id, location_id, reserved_qty, COALESCE(lot_id,0) AS lot_id "
                "FROM stock_move "
                "WHERE picking_id=$1 AND state IN ('assigned','partially_available') "
                "AND reserved_qty > 0",
                pqxx::params{id});
            for (const auto& m : moves) {
                core::StockQuant::release(txn, m["product_id"].as<int>(),
                                          m["location_id"].as<int>(),
                                          m["reserved_qty"].as<long long>(0),
                                          m["lot_id"].as<int>(0));
            }
            txn.exec(
                "UPDATE stock_move SET state='confirmed', reserved_qty=0, write_date=now() "
                "WHERE picking_id=$1 AND state IN ('assigned','partially_available')",
                pqxx::params{id});
            txn.exec(
                "UPDATE stock_picking SET state='confirmed', write_date=now() "
                "WHERE id=$1 AND state IN ('assigned','partially_available')",
                pqxx::params{id});
            txn.commit();
        }
        return true;
    }

    // ----------------------------------------------------------
    // button_reset_to_draft — cancel → draft
    // ----------------------------------------------------------
    nlohmann::json handleButtonResetToDraft(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            txn.exec(
                "UPDATE stock_picking SET state='draft', name='New', write_date=now() "
                "WHERE id=$1 AND state='cancel'",
                pqxx::params{id});
            txn.exec(
                "UPDATE stock_move SET state='draft' "
                "WHERE picking_id=$1 AND state='cancel'",
                pqxx::params{id});
            txn.commit();
        }
        return true;
    }
};


// ----------------------------------------------------------------
// StockMoveViewModel — stock.move with enriched search_read
// Returns M2one fields as [id, "Name"] so ListView.formatCell()
// can display names instead of raw integer IDs.
// ----------------------------------------------------------------
class StockMoveViewModel : public BaseViewModel {
public:
    explicit StockMoveViewModel(std::shared_ptr<DbConnection> db)
        : db_(std::move(db))
    {
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
    }

    std::string modelName() const override { return "stock.move"; }

private:
    std::shared_ptr<DbConnection> db_;

    // Helper: build [id, name] JSON array, or false if id is null
    static nlohmann::json m2o(const pqxx::row& row,
                              const char* idCol, const char* nameCol) {
        if (row[idCol].is_null()) return false;
        nlohmann::json pair = nlohmann::json::array();
        pair.push_back(row[idCol].as<int>());
        pair.push_back(row[nameCol].is_null() ? "" : std::string(row[nameCol].c_str()));
        return pair;
    }

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        int lim = call.limit() > 0 ? call.limit() : 80;
        int off = call.offset();

        // S-49: restrict domain columns to this model's stored fields.
        static const std::set<std::string> kCols = {
            "id","picking_id","product_id","product_uom_id","name","product_uom_qty",
            "quantity","state","location_id","location_dest_id","company_id",
            "origin","reserved_qty","lot_id","production_id"};
        auto [where, paramVec] = domainFromJson(call.domain()).toSql(&kCols);

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        std::string sql = R"(
            SELECT sm.id,
                   sm.name,
                   sm.state,
                   sm.origin,
                   sm.product_uom_qty,
                   sm.quantity,
                   sm.picking_id,
                   sp.name           AS picking_name,
                   sm.product_id,
                   pp.name           AS product_name,
                   sm.product_uom_id,
                   uu.name           AS product_uom_name,
                   sm.location_id,
                   COALESCE(sl_src.complete_name, sl_src.name) AS location_name,
                   sm.location_dest_id,
                   COALESCE(sl_dst.complete_name, sl_dst.name) AS location_dest_name,
                   sm.company_id,
                   rc.name           AS company_name
            FROM stock_move sm
            LEFT JOIN stock_picking   sp     ON sp.id  = sm.picking_id
            LEFT JOIN product_product pp     ON pp.id  = sm.product_id
            LEFT JOIN uom_uom          uu    ON uu.id  = sm.product_uom_id
            LEFT JOIN stock_location   sl_src ON sl_src.id = sm.location_id
            LEFT JOIN stock_location   sl_dst ON sl_dst.id = sm.location_dest_id
            LEFT JOIN res_company      rc    ON rc.id  = sm.company_id
            WHERE )";
        sql += where;
        // S-30: enforce ir.rule on this custom read (record-rule bypass fix, 071 §1.2).
        pqxx::params p; for (auto& s : paramVec) p.append(s);
        core::appendRecordRuleSubquery(sql, p, "stock.move", core::RuleOp::Read,
                                       extractContext_(call), "stock_move", "sm.id",
                                       static_cast<int>(paramVec.size()));
        sql += " ORDER BY sm.id DESC";
        sql += " LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);

        pqxx::result res = txn.exec(sql, p);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json obj;
            obj["id"]              = row["id"].as<int>();
            obj["name"]            = row["name"].is_null()    ? nlohmann::json(false) : nlohmann::json(row["name"].c_str());
            obj["state"]           = row["state"].is_null()   ? nlohmann::json(false) : nlohmann::json(row["state"].c_str());
            obj["origin"]          = row["origin"].is_null()  ? nlohmann::json(false) : nlohmann::json(row["origin"].c_str());
            // P2: both are BIGINT micro-units (migration 940)
            obj["product_uom_qty"] = row["product_uom_qty"].is_null() ? 0.0
                : core::Money::fromMicros(row["product_uom_qty"].as<long long>(0)).toJson();
            obj["quantity"]        = row["quantity"].is_null() ? 0.0
                : core::Money::fromMicros(row["quantity"].as<long long>(0)).toJson();
            obj["picking_id"]       = m2o(row, "picking_id",       "picking_name");
            obj["product_id"]       = m2o(row, "product_id",       "product_name");
            obj["product_uom_id"]   = m2o(row, "product_uom_id",   "product_uom_name");
            obj["location_id"]      = m2o(row, "location_id",      "location_name");
            obj["location_dest_id"] = m2o(row, "location_dest_id", "location_dest_name");
            obj["company_id"]       = m2o(row, "company_id",       "company_name");
            arr.push_back(std::move(obj));
        }
        return arr;
    }

    nlohmann::json handleRead(const CallKwArgs& call) {
        StockMove proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.read(call.ids(), call.fields());
    }
    nlohmann::json handleCreate(const CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("create: args[0] must be a dict");
        StockMove proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const int newId = proto.create(v);
        return newId;
    }
    nlohmann::json handleWrite(const CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        StockMove proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const auto result = proto.write(call.ids(), v);
        return result;
    }
    nlohmann::json handleUnlink(const CallKwArgs& call) {
        StockMove proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const auto ids = call.ids();
        const auto result = proto.unlink(ids);
        return result;
    }
    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        StockMove proto(db_);
        return proto.fieldsGet(call.fields());  // schema metadata — no rules needed
    }
    nlohmann::json handleSearchCount(const CallKwArgs& call) {
        StockMove proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.searchCount(call.domain());
    }
    nlohmann::json handleSearch(const CallKwArgs& call) {
        StockMove proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.search(call.domain(),
                            call.limit() > 0 ? call.limit() : 80,
                            call.offset());
    }
};

// ================================================================
// 3. MODULE
// ================================================================
// VIEWS
// ================================================================

// ----------------------------------------------------------------
// stock.picking list — only char/date/selection columns so the
// generic ListView never tries to display raw integer FK values.
// Columns: Reference, Source Document, Status, Scheduled Date.
// ----------------------------------------------------------------
class StockPickingListView : public core::BaseView {
public:
    std::string viewName()  const override { return "stock.picking.list"; }
    std::string modelName() const override { return "stock.picking"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Transfers\">"
               "<field name=\"name\"/>"
               "<field name=\"location_id\"/>"
               "<field name=\"location_dest_id\"/>"
               "<field name=\"partner_id\"/>"
               "<field name=\"scheduled_date\"/>"
               "<field name=\"origin\"/>"
               "<field name=\"state\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",             {{"type","char"},      {"string","Reference"}}},
            {"location_id",      {{"type","many2one"},  {"string","From"},           {"relation","stock.location"}}},
            {"location_dest_id", {{"type","many2one"},  {"string","To"},             {"relation","stock.location"}}},
            {"partner_id",       {{"type","many2one"},  {"string","Contact"},        {"relation","res.partner"}}},
            {"scheduled_date",   {{"type","datetime"},  {"string","Scheduled Date"}}},
            {"origin",           {{"type","char"},      {"string","Source Document"}}},
            {"state",            {{"type","selection"}, {"string","Status"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class StockPickingFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "stock.picking.form"; }
    std::string modelName() const override { return "stock.picking"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Transfer\"/>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",             {{"type","char"},      {"string","Reference"}}},
            {"state",            {{"type","selection"}, {"string","Status"}}},
            {"origin",           {{"type","char"},      {"string","Source Document"}}},
            {"partner_id",       {{"type","many2one"},  {"string","Contact"},        {"relation","res.partner"}}},
            {"location_id",      {{"type","many2one"},  {"string","From"},           {"relation","stock.location"}}},
            {"location_dest_id", {{"type","many2one"},  {"string","To"},             {"relation","stock.location"}}},
            {"scheduled_date",   {{"type","datetime"},  {"string","Scheduled Date"}}},
            {"picking_type_id",  {{"type","many2one"},  {"string","Operation Type"}, {"relation","stock.picking.type"}}},
            {"user_id",          {{"type","many2one"},  {"string","Responsible"},    {"relation","res.users"}}},
            {"company_id",       {{"type","many2one"},  {"string","Company"},        {"relation","res.company"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class StockMoveListView : public core::BaseView {
public:
    std::string viewName()  const override { return "stock.move.list"; }
    std::string modelName() const override { return "stock.move"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Move History\">"
               "<field name=\"picking_id\"/>"
               "<field name=\"product_id\"/>"
               "<field name=\"location_id\"/>"
               "<field name=\"location_dest_id\"/>"
               "<field name=\"product_uom_qty\"/>"
               "<field name=\"quantity\"/>"
               "<field name=\"state\"/>"
               "<field name=\"origin\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"picking_id",       {{"type","many2one"},  {"string","Transfer"},       {"relation","stock.picking"}}},
            {"product_id",       {{"type","many2one"},  {"string","Product"},        {"relation","product.product"}}},
            {"location_id",      {{"type","many2one"},  {"string","From"},           {"relation","stock.location"}}},
            {"location_dest_id", {{"type","many2one"},  {"string","To"},             {"relation","stock.location"}}},
            {"product_uom_qty",  {{"type","float"},     {"string","Demand"}}},
            {"quantity",         {{"type","float"},     {"string","Done"}}},
            {"state",            {{"type","selection"}, {"string","Status"}}},
            {"origin",           {{"type","char"},      {"string","Source Document"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ----------------------------------------------------------------
// StockQuantModel — stock.quant (on-hand ledger; read-only in the UI)
// ----------------------------------------------------------------
class StockQuantModel : public BaseModel<StockQuantModel> {
public:
    ODOO_MODEL("stock.quant", "stock_quant")

    int    productId        = 0;
    int    locationId       = 0;
    int    lotId            = 0;
    double quantity         = 0.0;
    double reservedQuantity = 0.0;
    int    companyId        = 0;

    explicit StockQuantModel(std::shared_ptr<DbConnection> db)
        : BaseModel<StockQuantModel>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"product_id",        FieldType::Many2one,"Product",  false, true, true, true, "product.product"});
        fieldRegistry_.add({"location_id",       FieldType::Many2one,"Location", false, true, true, true, "stock.location"});
        fieldRegistry_.add({"lot_id",            FieldType::Many2one,"Lot/Serial", false, true, true, true, "stock.production.lot"});
        fieldRegistry_.add({"quantity",          FieldType::Float,   "On Hand",  false, true, true, true});
        fieldRegistry_.add({"reserved_quantity", FieldType::Float,   "Reserved", false, true, true, true});
        fieldRegistry_.add({"company_id",        FieldType::Many2one,"Company",  false, true, true, true, "res.company"});
        fieldRegistry_.markScaled({"quantity", "reserved_quantity"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["product_id"]        = productId  > 0 ? nlohmann::json(productId)  : nlohmann::json(false);
        j["location_id"]       = locationId > 0 ? nlohmann::json(locationId) : nlohmann::json(false);
        j["lot_id"]            = lotId      > 0 ? nlohmann::json(lotId)      : nlohmann::json(false);
        j["quantity"]          = quantity;
        j["reserved_quantity"] = reservedQuantity;
        j["company_id"]        = companyId  > 0 ? nlohmann::json(companyId)  : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("quantity")          && j["quantity"].is_number())          quantity         = j["quantity"].get<double>();
        if (j.contains("reserved_quantity") && j["reserved_quantity"].is_number()) reservedQuantity = j["reserved_quantity"].get<double>();
        if (const int v = parseM2o(j, "product_id"))  productId  = v;
        if (const int v = parseM2o(j, "location_id")) locationId = v;
        if (const int v = parseM2o(j, "company_id"))  companyId  = v;
    }
};

// ----------------------------------------------------------------
// StockQuantViewModel — read-only on-hand report + inventory adjustment.
// Quants are engine-managed, so no create/write/unlink is exposed; the only
// mutating entry point is set_on_hand (a counted-quantity correction).
// ----------------------------------------------------------------
class StockQuantViewModel : public BaseViewModel {
public:
    explicit StockQuantViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_METHOD("web_read",        handleRead)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
        REGISTER_METHOD("set_on_hand",     handleSetOnHand)
        REGISTER_METHOD("resolve_barcode", handleResolveBarcode)
    }

    std::string modelName() const override { return "stock.quant"; }

private:
    std::shared_ptr<DbConnection> db_;

    static int domainInt(const nlohmann::json& domain, const std::string& key) {
        if (!domain.is_array()) return 0;
        for (const auto& c : domain)
            if (c.is_array() && c.size() == 3 && c[0].is_string() &&
                c[0].get<std::string>() == key && c[2].is_number_integer())
                return c[2].get<int>();
        return 0;
    }

    // On-hand report: internal locations only, names joined, available derived.
    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        const int prodFilter = domainInt(call.domain(), "product_id");
        const int locFilter  = domainInt(call.domain(), "location_id");
        const int lim = call.limit() > 0 ? call.limit() : 80;
        const int off = call.offset();

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT q.id, q.product_id, pp.name AS product_name,
                   q.location_id, COALESCE(sl.complete_name, sl.name) AS location_name,
                   q.lot_id, lot.name AS lot_name,
                   q.quantity, q.reserved_quantity,
                   q.company_id, rc.name AS company_name
            FROM stock_quant q
            JOIN stock_location sl ON sl.id = q.location_id
            LEFT JOIN product_product pp ON pp.id = q.product_id
            LEFT JOIN stock_production_lot lot ON lot.id = q.lot_id
            LEFT JOIN res_company     rc ON rc.id = q.company_id
            WHERE sl.usage = 'internal'
        )";
        pqxx::params p;
        int n = 0;
        if (prodFilter > 0) { sql += " AND q.product_id = $" + std::to_string(++n); p.append(prodFilter); }
        if (locFilter  > 0) { sql += " AND q.location_id = $" + std::to_string(++n); p.append(locFilter); }
        sql += " ORDER BY pp.name, sl.complete_name";
        sql += " LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);
        auto res = n ? txn.exec(sql, p) : txn.exec(sql);

        auto m2o = [](const pqxx::row& row, const char* idc, const char* namec) -> nlohmann::json {
            if (row[idc].is_null()) return false;
            return nlohmann::json::array(
                {row[idc].as<int>(), row[namec].is_null() ? "" : std::string(row[namec].c_str())});
        };
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            const long long qty = row["quantity"].as<long long>(0);
            const long long rsv = row["reserved_quantity"].as<long long>(0);
            nlohmann::json j;
            j["id"]                 = row["id"].as<int>();
            j["product_id"]         = m2o(row, "product_id",  "product_name");
            j["location_id"]        = m2o(row, "location_id", "location_name");
            j["lot_id"]             = row["lot_id"].is_null() || row["lot_id"].as<int>(0) == 0
                                        ? nlohmann::json(false) : m2o(row, "lot_id", "lot_name");
            j["quantity"]           = core::Money::fromMicros(qty).toJson();
            j["reserved_quantity"]  = core::Money::fromMicros(rsv).toJson();
            j["available_quantity"] = core::Money::fromMicros(qty - rsv).toJson();
            j["company_id"]         = m2o(row, "company_id",  "company_name");
            arr.push_back(std::move(j));
        }
        return arr;
    }

    nlohmann::json handleRead(const CallKwArgs& call) {
        StockQuantModel proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.read(call.ids(), call.fields());
    }
    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        StockQuantModel proto(db_);
        return proto.fieldsGet(call.fields());
    }
    nlohmann::json handleSearchCount(const CallKwArgs& /*call*/) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto r = txn.exec(
            "SELECT count(*) FROM stock_quant q "
            "JOIN stock_location sl ON sl.id=q.location_id WHERE sl.usage='internal'");
        return r[0][0].as<int>(0);
    }

    // Inventory adjustment — set the on-hand at a location to a counted value,
    // booking the difference as a move to/from Inventory Adjustments (loc 7)
    // so every correction leaves an auditable stock ledger entry.
    nlohmann::json handleSetOnHand(const CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("set_on_hand: args[0] must be a dict");
        const int    prod = parseM2o(v, "product_id");
        const int    loc  = parseM2o(v, "location_id");
        const double newF = (v.contains("quantity") && v["quantity"].is_number())
                            ? v["quantity"].get<double>() : 0.0;
        if (prod <= 0 || loc <= 0)
            throw std::runtime_error("set_on_hand: product_id and location_id are required");
        const long long newQ = static_cast<long long>(std::llround(newF * 1000000.0));

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto cur = txn.exec(
            "SELECT quantity, company_id FROM stock_quant WHERE product_id=$1 AND location_id=$2",
            pqxx::params{prod, loc});
        const long long curQ = cur.empty() ? 0 : cur[0]["quantity"].as<long long>(0);
        int comp = 0;
        if (!cur.empty() && !cur[0]["company_id"].is_null()) comp = cur[0]["company_id"].as<int>();
        const long long delta = newQ - curQ;
        constexpr int kInventoryLoc = 7;   // "Inventory Adjustments" (usage='inventory')
        if (delta > 0)      core::StockQuant::applyMove(txn, prod, kInventoryLoc, loc,  delta, comp);
        else if (delta < 0) core::StockQuant::applyMove(txn, prod, loc, kInventoryLoc, -delta, comp);
        txn.commit();

        if (AuditService::ready())
            AuditService::instance().log("stock.quant", "set_on_hand",
                                         {prod}, extractContext_(call).uid);
        nlohmann::json out;
        out["product_id"]  = prod;
        out["location_id"] = loc;
        out["quantity"]    = core::Money::fromMicros(newQ).toJson();
        return out;
    }

    // Barcode resolver: a scanned code → what it is (product / location / lot).
    nlohmann::json handleResolveBarcode(const CallKwArgs& call) {
        const auto v = call.arg(0);
        std::string code;
        if (v.is_object() && v.contains("barcode") && v["barcode"].is_string()) code = v["barcode"].get<std::string>();
        else if (v.is_string()) code = v.get<std::string>();
        if (code.empty()) throw std::runtime_error("resolve_barcode: a barcode is required");

        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        auto hit = [](const std::string& type, int id, const char* name) {
            nlohmann::json j; j["type"] = type; j["id"] = id;
            j["name"] = name ? nlohmann::json(name) : nlohmann::json(false); return j;
        };
        { auto r = txn.exec("SELECT id, name FROM product_product WHERE barcode=$1 AND active=TRUE LIMIT 1", pqxx::params{code});
          if (!r.empty()) return hit("product", r[0]["id"].as<int>(), r[0]["name"].is_null() ? nullptr : r[0]["name"].c_str()); }
        { auto r = txn.exec("SELECT id, COALESCE(complete_name,name) AS name FROM stock_location WHERE barcode=$1 LIMIT 1", pqxx::params{code});
          if (!r.empty()) return hit("location", r[0]["id"].as<int>(), r[0]["name"].is_null() ? nullptr : r[0]["name"].c_str()); }
        { auto r = txn.exec("SELECT id, name FROM stock_production_lot WHERE barcode=$1 LIMIT 1", pqxx::params{code});
          if (!r.empty()) return hit("lot", r[0]["id"].as<int>(), r[0]["name"].is_null() ? nullptr : r[0]["name"].c_str()); }
        nlohmann::json miss; miss["type"] = "unknown"; miss["id"] = false; miss["name"] = false;
        return miss;
    }
};

// ----------------------------------------------------------------
// stock.quant on-hand list
// ----------------------------------------------------------------
class StockQuantListView : public core::BaseView {
public:
    std::string viewName()  const override { return "stock.quant.list"; }
    std::string modelName() const override { return "stock.quant"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"On Hand\">"
               "<field name=\"product_id\"/>"
               "<field name=\"location_id\"/>"
               "<field name=\"lot_id\"/>"
               "<field name=\"quantity\"/>"
               "<field name=\"reserved_quantity\"/>"
               "<field name=\"available_quantity\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"product_id",         {{"type","many2one"}, {"string","Product"},  {"relation","product.product"}}},
            {"location_id",        {{"type","many2one"}, {"string","Location"}, {"relation","stock.location"}}},
            {"lot_id",             {{"type","many2one"}, {"string","Lot/Serial"},{"relation","stock.production.lot"}}},
            {"quantity",           {{"type","float"},    {"string","On Hand"}}},
            {"reserved_quantity",  {{"type","float"},    {"string","Reserved"}}},
            {"available_quantity", {{"type","float"},    {"string","Available"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ----------------------------------------------------------------
// StockValuationLayerModel — stock.valuation.layer (read-only ledger)
// ----------------------------------------------------------------
class StockValuationLayerModel : public BaseModel<StockValuationLayerModel> {
public:
    ODOO_MODEL("stock.valuation.layer", "stock_valuation_layer")

    int         productId = 0;
    double      quantity  = 0.0;
    double      unitCost  = 0.0;
    double      value     = 0.0;
    std::string description;
    int         companyId = 0;

    explicit StockValuationLayerModel(std::shared_ptr<DbConnection> db)
        : BaseModel<StockValuationLayerModel>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"product_id",  FieldType::Many2one,"Product",     false, true, true, true, "product.product"});
        fieldRegistry_.add({"quantity",    FieldType::Float,   "Quantity",    false, true, true, true});
        fieldRegistry_.add({"unit_cost",   FieldType::Monetary,"Unit Value",  false, true, true, true});
        fieldRegistry_.add({"value",       FieldType::Monetary,"Value",       false, true, true, true});
        fieldRegistry_.add({"description", FieldType::Char,    "Description",  false, true, true, true});
        fieldRegistry_.add({"company_id",  FieldType::Many2one,"Company",      false, true, true, true, "res.company"});
        fieldRegistry_.markScaled({"quantity", "unit_cost", "value"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["product_id"]  = productId > 0 ? nlohmann::json(productId) : nlohmann::json(false);
        j["quantity"]    = quantity;
        j["unit_cost"]   = unitCost;
        j["value"]       = value;
        j["description"] = description.empty() ? nlohmann::json(false) : nlohmann::json(description);
        j["company_id"]  = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("quantity")    && j["quantity"].is_number())    quantity = j["quantity"].get<double>();
        if (j.contains("unit_cost")   && j["unit_cost"].is_number())   unitCost = j["unit_cost"].get<double>();
        if (j.contains("value")       && j["value"].is_number())       value    = j["value"].get<double>();
        if (j.contains("description") && j["description"].is_string()) description = j["description"].get<std::string>();
        if (const int v = parseM2o(j, "product_id")) productId = v;
        if (const int v = parseM2o(j, "company_id")) companyId = v;
    }
};

// ----------------------------------------------------------------
// StockValuationLayerViewModel — read-only inventory-value ledger
// ----------------------------------------------------------------
class StockValuationLayerViewModel : public BaseViewModel {
public:
    explicit StockValuationLayerViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_METHOD("web_read",        handleRead)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
    }
    std::string modelName() const override { return "stock.valuation.layer"; }
private:
    std::shared_ptr<DbConnection> db_;

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        const int lim = call.limit() > 0 ? call.limit() : 80;
        const int off = call.offset();
        int prodFilter = 0;
        { auto d = call.domain(); if (d.is_array()) for (const auto& c : d)
            if (c.is_array() && c.size() == 3 && c[0].is_string() &&
                c[0].get<std::string>() == "product_id" && c[2].is_number_integer())
                prodFilter = c[2].get<int>(); }
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT v.id, v.product_id, pp.name AS product_name,
                   v.quantity, v.unit_cost, v.value, v.description, v.create_date, v.company_id
            FROM stock_valuation_layer v
            LEFT JOIN product_product pp ON pp.id = v.product_id
        )";
        pqxx::params p; int n = 0;
        if (prodFilter > 0) { sql += " WHERE v.product_id = $" + std::to_string(++n); p.append(prodFilter); }
        sql += " ORDER BY v.id DESC LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);
        auto res = n ? txn.exec(sql, p) : txn.exec(sql);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]          = row["id"].as<int>();
            j["product_id"]  = row["product_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["product_id"].as<int>(),
                                         row["product_name"].is_null() ? "" : std::string(row["product_name"].c_str())});
            j["quantity"]    = core::Money::fromMicros(row["quantity"].as<long long>(0)).toJson();
            j["unit_cost"]   = core::Money::fromMicros(row["unit_cost"].as<long long>(0)).toJson();
            j["value"]       = core::Money::fromMicros(row["value"].as<long long>(0)).toJson();
            j["description"] = row["description"].is_null() ? nlohmann::json(false) : nlohmann::json(row["description"].c_str());
            j["create_date"] = row["create_date"].is_null() ? nlohmann::json(false) : nlohmann::json(row["create_date"].c_str());
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const CallKwArgs& call)        { StockValuationLayerModel p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids(), call.fields()); }
    nlohmann::json handleFieldsGet(const CallKwArgs& call)   { StockValuationLayerModel p(db_); return p.fieldsGet(call.fields()); }
    nlohmann::json handleSearchCount(const CallKwArgs& call) { StockValuationLayerModel p(db_); p.setUserContext(extractContext_(call)); return p.searchCount(call.domain()); }
};

class StockValuationLayerListView : public core::BaseView {
public:
    std::string viewName()  const override { return "stock.valuation.layer.list"; }
    std::string modelName() const override { return "stock.valuation.layer"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Inventory Valuation\">"
               "<field name=\"product_id\"/>"
               "<field name=\"description\"/>"
               "<field name=\"quantity\"/>"
               "<field name=\"unit_cost\"/>"
               "<field name=\"value\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"product_id",  {{"type","many2one"}, {"string","Product"}, {"relation","product.product"}}},
            {"description", {{"type","char"},     {"string","Description"}}},
            {"quantity",    {{"type","float"},    {"string","Quantity"}}},
            {"unit_cost",   {{"type","monetary"}, {"string","Unit Value"}}},
            {"value",       {{"type","monetary"}, {"string","Value"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ----------------------------------------------------------------
// StockProductionLotModel — stock.production.lot (a lot/serial number)
// ----------------------------------------------------------------
class StockProductionLotModel : public BaseModel<StockProductionLotModel> {
public:
    ODOO_MODEL("stock.production.lot", "stock_production_lot")

    std::string name;
    int         productId = 0;
    std::string ref;
    std::string barcode;
    int         companyId = 0;

    explicit StockProductionLotModel(std::shared_ptr<DbConnection> db)
        : BaseModel<StockProductionLotModel>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",       FieldType::Char,    "Lot/Serial Number", true});
        fieldRegistry_.add({"product_id", FieldType::Many2one,"Product", true, false, true, true, "product.product"});
        fieldRegistry_.add({"ref",        FieldType::Char,    "Internal Reference"});
        fieldRegistry_.add({"barcode",    FieldType::Char,    "Barcode"});
        fieldRegistry_.add({"company_id", FieldType::Many2one,"Company", false, false, true, false, "res.company"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"]       = name;
        j["product_id"] = productId > 0 ? nlohmann::json(productId) : nlohmann::json(false);
        j["ref"]        = ref.empty() ? nlohmann::json(false) : nlohmann::json(ref);
        j["barcode"]    = barcode.empty() ? nlohmann::json(false) : nlohmann::json(barcode);
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")    && j["name"].is_string())    name    = j["name"].get<std::string>();
        if (j.contains("ref")     && j["ref"].is_string())     ref     = j["ref"].get<std::string>();
        if (j.contains("barcode") && j["barcode"].is_string()) barcode = j["barcode"].get<std::string>();
        if (const int v = parseM2o(j, "product_id")) productId = v;
        if (const int v = parseM2o(j, "company_id")) companyId = v;
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty())    e.push_back("Lot/serial number is required");
        if (productId <= 0)  e.push_back("Product is required");
        return e;
    }
};

// ----------------------------------------------------------------
// StockProductionLotViewModel — lots + a traceability action
// ----------------------------------------------------------------
class StockProductionLotViewModel : public BaseViewModel {
public:
    explicit StockProductionLotViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_METHOD("web_read",        handleRead)
        REGISTER_MUTATOR("create",          handleCreate)
        REGISTER_MUTATOR("write",           handleWrite)
        REGISTER_MUTATOR("unlink",          handleUnlink)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
        REGISTER_METHOD("traceability",    handleTraceability)
    }
    std::string modelName() const override { return "stock.production.lot"; }
private:
    std::shared_ptr<DbConnection> db_;

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        const int lim = call.limit() > 0 ? call.limit() : 80;
        const int off = call.offset();
        int prodFilter = 0;
        { auto d = call.domain(); if (d.is_array()) for (const auto& c : d)
            if (c.is_array() && c.size() == 3 && c[0].is_string() &&
                c[0].get<std::string>() == "product_id" && c[2].is_number_integer())
                prodFilter = c[2].get<int>(); }
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT l.id, l.name, l.product_id, pp.name AS product_name, l.ref,
                   COALESCE((SELECT SUM(q.quantity) FROM stock_quant q
                             JOIN stock_location sl ON sl.id=q.location_id
                             WHERE q.lot_id=l.id AND sl.usage='internal'),0) AS on_hand
            FROM stock_production_lot l
            LEFT JOIN product_product pp ON pp.id = l.product_id
        )";
        pqxx::params p; int n = 0;
        if (prodFilter > 0) { sql += " WHERE l.product_id = $" + std::to_string(++n); p.append(prodFilter); }
        sql += " ORDER BY l.id DESC LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);
        auto res = n ? txn.exec(sql, p) : txn.exec(sql);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]         = row["id"].as<int>();
            j["name"]       = row["name"].is_null() ? "" : row["name"].c_str();
            j["product_id"] = row["product_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["product_id"].as<int>(),
                                         row["product_name"].is_null() ? "" : std::string(row["product_name"].c_str())});
            j["ref"]        = row["ref"].is_null() ? nlohmann::json(false) : nlohmann::json(row["ref"].c_str());
            j["product_qty"]= core::Money::fromMicros(row["on_hand"].as<long long>(0)).toJson();
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const CallKwArgs& call)        { StockProductionLotModel p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids(), call.fields()); }
    nlohmann::json handleCreate(const CallKwArgs& call)      { StockProductionLotModel p(db_); p.setUserContext(extractContext_(call)); return p.create(call.arg(0)); }
    nlohmann::json handleWrite(const CallKwArgs& call)       { StockProductionLotModel p(db_); p.setUserContext(extractContext_(call)); return p.write(call.ids(), call.arg(1)); }
    nlohmann::json handleUnlink(const CallKwArgs& call)      { StockProductionLotModel p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids()); }
    nlohmann::json handleFieldsGet(const CallKwArgs& call)   { StockProductionLotModel p(db_); return p.fieldsGet(call.fields()); }
    nlohmann::json handleSearchCount(const CallKwArgs& call) { StockProductionLotModel p(db_); p.setUserContext(extractContext_(call)); return p.searchCount(call.domain()); }

    // traceability(lot_id): current on-hand by location + full move history.
    nlohmann::json handleTraceability(const CallKwArgs& call) {
        const auto v = call.arg(0);
        int lotId = 0;
        if (v.is_object() && v.contains("lot_id")) lotId = parseM2o(v, "lot_id");
        else if (!call.ids().empty()) lotId = call.ids().front();
        if (lotId <= 0) throw std::runtime_error("traceability: lot_id is required");

        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        nlohmann::json out;
        out["on_hand"] = core::Money::fromMicros(txn.exec(
            "SELECT COALESCE(SUM(q.quantity),0) FROM stock_quant q "
            "JOIN stock_location sl ON sl.id=q.location_id "
            "WHERE q.lot_id=$1 AND sl.usage='internal'", pqxx::params{lotId})[0][0].as<long long>(0)).toJson();
        auto byLoc = txn.exec(
            "SELECT COALESCE(sl.complete_name, sl.name) AS loc, q.quantity "
            "FROM stock_quant q JOIN stock_location sl ON sl.id=q.location_id "
            "WHERE q.lot_id=$1 AND q.quantity <> 0 ORDER BY sl.complete_name", pqxx::params{lotId});
        nlohmann::json locs = nlohmann::json::array();
        for (const auto& r : byLoc)
            locs.push_back({{"location", r["loc"].is_null() ? "" : r["loc"].c_str()},
                            {"quantity", core::Money::fromMicros(r["quantity"].as<long long>(0)).toJson()}});
        out["by_location"] = std::move(locs);
        auto mv = txn.exec(
            "SELECT sm.id, sm.quantity, sm.state, sm.origin, "
            "  COALESCE(ss.complete_name, ss.name) AS src, COALESCE(sd.complete_name, sd.name) AS dst "
            "FROM stock_move sm "
            "LEFT JOIN stock_location ss ON ss.id=sm.location_id "
            "LEFT JOIN stock_location sd ON sd.id=sm.location_dest_id "
            "WHERE sm.lot_id=$1 ORDER BY sm.id", pqxx::params{lotId});
        nlohmann::json moves = nlohmann::json::array();
        for (const auto& r : mv)
            moves.push_back({{"id", r["id"].as<int>()},
                             {"quantity", core::Money::fromMicros(r["quantity"].as<long long>(0)).toJson()},
                             {"state", r["state"].is_null() ? "" : r["state"].c_str()},
                             {"from", r["src"].is_null() ? "" : r["src"].c_str()},
                             {"to", r["dst"].is_null() ? "" : r["dst"].c_str()},
                             {"origin", r["origin"].is_null() ? "" : r["origin"].c_str()}});
        out["moves"] = std::move(moves);
        return out;
    }
};

class StockProductionLotListView : public core::BaseView {
public:
    std::string viewName()  const override { return "stock.production.lot.list"; }
    std::string modelName() const override { return "stock.production.lot"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Lots/Serial Numbers\">"
               "<field name=\"name\"/>"
               "<field name=\"product_id\"/>"
               "<field name=\"product_qty\"/>"
               "<field name=\"ref\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",        {{"type","char"},     {"string","Lot/Serial Number"}}},
            {"product_id",  {{"type","many2one"}, {"string","Product"}, {"relation","product.product"}}},
            {"product_qty", {{"type","float"},    {"string","On Hand"}}},
            {"ref",         {{"type","char"},     {"string","Internal Reference"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ----------------------------------------------------------------
// StockLandedCostModel — stock.landed.cost (header)
// ----------------------------------------------------------------
class StockLandedCostModel : public BaseModel<StockLandedCostModel> {
public:
    ODOO_MODEL("stock.landed.cost", "stock_landed_cost")

    std::string name = "New";
    std::string date;
    int         pickingId = 0;
    std::string state = "draft";
    int         companyId = 0;

    explicit StockLandedCostModel(std::shared_ptr<DbConnection> db)
        : BaseModel<StockLandedCostModel>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",       FieldType::Char,    "Reference"});
        fieldRegistry_.add({"date",       FieldType::Date,    "Date"});
        fieldRegistry_.add({"picking_id", FieldType::Many2one,"Receipt", true, false, true, false, "stock.picking"});
        fieldRegistry_.add({"state",      FieldType::Char,    "Status"});
        fieldRegistry_.add({"company_id", FieldType::Many2one,"Company", false, false, true, false, "res.company"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"]       = name;
        j["date"]       = date.empty() ? nlohmann::json(false) : nlohmann::json(date);
        j["picking_id"] = pickingId > 0 ? nlohmann::json(pickingId) : nlohmann::json(false);
        j["state"]      = state;
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")  && j["name"].is_string())  name  = j["name"].get<std::string>();
        if (j.contains("date")  && j["date"].is_string())  date  = j["date"].get<std::string>();
        if (j.contains("state") && j["state"].is_string()) state = j["state"].get<std::string>();
        if (const int v = parseM2o(j, "picking_id")) pickingId = v;
        if (const int v = parseM2o(j, "company_id")) companyId = v;
    }
};

// ----------------------------------------------------------------
// StockLandedCostLineModel — stock.landed.cost.line (a cost item)
// ----------------------------------------------------------------
class StockLandedCostLineModel : public BaseModel<StockLandedCostLineModel> {
public:
    ODOO_MODEL("stock.landed.cost.line", "stock_landed_cost_line")

    int         landedCostId = 0;
    std::string name;
    int         productId = 0;
    double      price = 0.0;
    std::string splitMethod = "by_quantity";
    int         accountId = 0;

    explicit StockLandedCostLineModel(std::shared_ptr<DbConnection> db)
        : BaseModel<StockLandedCostLineModel>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"landed_cost_id", FieldType::Many2one,"Landed Cost", true, false, true, false, "stock.landed.cost"});
        fieldRegistry_.add({"name",           FieldType::Char,    "Description", true});
        fieldRegistry_.add({"product_id",     FieldType::Many2one,"Cost Product", false, false, true, false, "product.product"});
        fieldRegistry_.add({"price",          FieldType::Monetary,"Amount"});
        fieldRegistry_.add({"split_method",   FieldType::Char,    "Split Method"});
        fieldRegistry_.add({"account_id",     FieldType::Many2one,"Account", false, false, true, false, "account.account"});
        fieldRegistry_.markScaled({"price"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["landed_cost_id"] = landedCostId > 0 ? nlohmann::json(landedCostId) : nlohmann::json(false);
        j["name"]           = name;
        j["product_id"]     = productId > 0 ? nlohmann::json(productId) : nlohmann::json(false);
        j["price"]          = price;
        j["split_method"]   = splitMethod.empty() ? "by_quantity" : splitMethod;
        j["account_id"]     = accountId > 0 ? nlohmann::json(accountId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")         && j["name"].is_string())         name        = j["name"].get<std::string>();
        if (j.contains("split_method") && j["split_method"].is_string()) splitMethod = j["split_method"].get<std::string>();
        if (j.contains("price")        && j["price"].is_number())        price       = j["price"].get<double>();
        if (const int v = parseM2o(j, "landed_cost_id")) landedCostId = v;
        if (const int v = parseM2o(j, "product_id"))     productId    = v;
        if (const int v = parseM2o(j, "account_id"))     accountId    = v;
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (landedCostId <= 0) e.push_back("landed_cost_id is required");
        if (name.empty())      e.push_back("Description is required");
        return e;
    }
};

// ----------------------------------------------------------------
// StockLandedCostViewModel — CRUD + the distribution/validation engine
// ----------------------------------------------------------------
class StockLandedCostViewModel : public BaseViewModel {
public:
    explicit StockLandedCostViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_METHOD("web_read",        handleRead)
        REGISTER_MUTATOR("create",          handleCreate)
        REGISTER_MUTATOR("write",           handleWrite)
        REGISTER_MUTATOR("unlink",          handleUnlink)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
        REGISTER_METHOD("default_get",     handleDefaultGet)
        REGISTER_METHOD("button_validate", handleValidate)
    }
    std::string modelName() const override { return "stock.landed.cost"; }
private:
    std::shared_ptr<DbConnection> db_;

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        const int lim = call.limit() > 0 ? call.limit() : 80;
        const int off = call.offset();
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT lc.id, lc.name, lc.state, lc.date, lc.picking_id, sp.name AS picking_name,
                   COALESCE((SELECT SUM(price) FROM stock_landed_cost_line WHERE landed_cost_id=lc.id),0) AS total
            FROM stock_landed_cost lc
            LEFT JOIN stock_picking sp ON sp.id = lc.picking_id
            ORDER BY lc.id DESC)";
        sql += " LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);
        auto res = txn.exec(sql);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]         = row["id"].as<int>();
            j["name"]       = row["name"].is_null() ? nlohmann::json(false) : nlohmann::json(row["name"].c_str());
            j["state"]      = row["state"].is_null() ? nlohmann::json(false) : nlohmann::json(row["state"].c_str());
            j["date"]       = row["date"].is_null() ? nlohmann::json(false) : nlohmann::json(row["date"].c_str());
            j["picking_id"] = row["picking_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["picking_id"].as<int>(),
                                         row["picking_name"].is_null() ? "" : std::string(row["picking_name"].c_str())});
            j["amount_total"] = core::Money::fromMicros(row["total"].as<long long>(0)).toJson();
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const CallKwArgs& call)        { StockLandedCostModel p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids(), call.fields()); }
    nlohmann::json handleCreate(const CallKwArgs& call)      { StockLandedCostModel p(db_); p.setUserContext(extractContext_(call)); return p.create(call.arg(0)); }
    nlohmann::json handleWrite(const CallKwArgs& call)       { StockLandedCostModel p(db_); p.setUserContext(extractContext_(call)); return p.write(call.ids(), call.arg(1)); }
    nlohmann::json handleUnlink(const CallKwArgs& call)      { StockLandedCostModel p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids()); }
    nlohmann::json handleFieldsGet(const CallKwArgs& call)   { StockLandedCostModel p(db_); return p.fieldsGet(call.fields()); }
    nlohmann::json handleSearchCount(const CallKwArgs& call) { StockLandedCostModel p(db_); p.setUserContext(extractContext_(call)); return p.searchCount(call.domain()); }
    nlohmann::json handleDefaultGet(const CallKwArgs& /*call*/) { return {{"state","draft"}, {"company_id",1}}; }

    // Distribute each cost line across the products received on the linked
    // picking, per its split method — raising each product's inventory value
    // (a revaluation layer) and posting Dr Stock Valuation / Cr the cost account.
    nlohmann::json handleValidate(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire(); pqxx::work txn{conn.get()};
            auto lc = txn.exec("SELECT picking_id, state, company_id FROM stock_landed_cost WHERE id=$1",
                               pqxx::params{id});
            if (lc.empty()) continue;
            if (std::string(lc[0]["state"].c_str()) == "done") continue;
            if (lc[0]["picking_id"].is_null())
                throw std::runtime_error("Set the receipt this landed cost applies to");
            const int pickingId = lc[0]["picking_id"].as<int>();
            const int comp      = lc[0]["company_id"].is_null() ? 1 : lc[0]["company_id"].as<int>();

            // Received products on that picking (done incoming moves).
            struct P { int id; long long qty; double weight, volume; long long cost; };
            std::vector<P> prods;
            auto recv = txn.exec(
                "SELECT sm.product_id, SUM(sm.quantity) AS qty, "
                "       MAX(pp.weight) AS weight, MAX(pp.volume) AS volume, MAX(pp.standard_price) AS cost "
                "FROM stock_move sm JOIN product_product pp ON pp.id=sm.product_id "
                "WHERE sm.picking_id=$1 AND sm.state='done' AND sm.location_dest_id IN "
                "  (SELECT id FROM stock_location WHERE usage='internal') "
                "GROUP BY sm.product_id", pqxx::params{pickingId});
            for (const auto& r : recv)
                prods.push_back({r["product_id"].as<int>(), r["qty"].as<long long>(0),
                                 r["weight"].as<double>(0.0), r["volume"].as<double>(0.0),
                                 r["cost"].as<long long>(0)});

            // Resolve the Stock Valuation account + stock journal once.
            auto acctByCode = [&](const char* code) -> int {
                auto r = txn.exec("SELECT id FROM account_account WHERE code=$1 AND company_id=$2 LIMIT 1",
                                  pqxx::params{std::string(code), comp});
                return r.empty() ? 0 : r[0][0].as<int>();
            };
            const int valAcct  = acctByCode("1400");
            const int dfltCost = acctByCode("5200");
            int journalId = 0;
            { auto r = txn.exec("SELECT id FROM account_journal WHERE code='STJ' AND company_id=$1 LIMIT 1",
                                pqxx::params{comp}); journalId = r.empty() ? 0 : r[0][0].as<int>(); }

            auto postGl = [&](int layerId, long long amount, int costAcct) {
                if (amount <= 0 || journalId <= 0 || valAcct <= 0 || costAcct <= 0) return;
                const int moveId = txn.exec(
                    "INSERT INTO account_move (name, move_type, state, date, journal_id, company_id) "
                    "VALUES ('/','entry','posted',CURRENT_DATE,$1,$2) RETURNING id",
                    pqxx::params{journalId, comp})[0][0].as<int>();
                txn.exec("UPDATE account_move SET name=$2 WHERE id=$1",
                         pqxx::params{moveId, std::string("STJ/") + std::to_string(moveId)});
                auto line = [&](int acct, long long dr, long long cr) {
                    txn.exec("INSERT INTO account_move_line "
                             "(move_id, account_id, journal_id, company_id, date, name, debit, credit) "
                             "VALUES ($1,$2,$3,$4,CURRENT_DATE,'Landed cost',$5,$6)",
                             pqxx::params{moveId, acct, journalId, comp, dr, cr});
                };
                line(valAcct, amount, 0);      // Dr Stock Valuation
                line(costAcct, 0, amount);     // Cr the cost account (capitalises the cost)
                if (layerId > 0)
                    txn.exec("UPDATE stock_valuation_layer SET account_move_id=$2 WHERE id=$1",
                             pqxx::params{layerId, moveId});
            };

            auto lines = txn.exec(
                "SELECT id, name, price, split_method, account_id FROM stock_landed_cost_line "
                "WHERE landed_cost_id=$1", pqxx::params{id});
            for (const auto& ln : lines) {
                const long long   priceMicros = ln["price"].as<long long>(0);
                const std::string method      = ln["split_method"].is_null() ? "by_quantity" : ln["split_method"].c_str();
                const int         costAcct     = ln["account_id"].is_null() ? dfltCost : ln["account_id"].as<int>();
                const std::string desc         = ln["name"].is_null() ? "Landed cost" : ln["name"].c_str();
                if (priceMicros <= 0 || prods.empty()) continue;

                std::vector<double> basis(prods.size(), 0.0);
                double totalB = 0.0;
                for (std::size_t i = 0; i < prods.size(); ++i) {
                    const double qtyU = static_cast<double>(prods[i].qty) / 1e6;
                    double b = 0.0;
                    if      (method == "by_quantity") b = qtyU;
                    else if (method == "by_price")    b = qtyU * (static_cast<double>(prods[i].cost) / 1e6);
                    else if (method == "by_weight")   b = qtyU * prods[i].weight;
                    else if (method == "by_volume")   b = qtyU * prods[i].volume;
                    else                              b = 1.0;   // equal
                    basis[i] = b; totalB += b;
                }
                std::vector<long long> alloc(prods.size(), 0);
                long long distributed = 0;
                for (std::size_t i = 0; i < prods.size(); ++i) {
                    if (totalB > 0.0)
                        alloc[i] = static_cast<long long>(std::llround(static_cast<double>(priceMicros) * basis[i] / totalB));
                    distributed += alloc[i];
                }
                alloc.back() += (priceMicros - distributed);   // rounding remainder onto the last product
                for (std::size_t i = 0; i < prods.size(); ++i) {
                    if (alloc[i] == 0) continue;
                    const int layerId = core::StockQuant::revalue(txn, prods[i].id, alloc[i],
                                                                  "Landed: " + desc, comp);
                    postGl(layerId, alloc[i], costAcct);
                }
            }
            txn.exec("UPDATE stock_landed_cost SET state='done', write_date=now() WHERE id=$1", pqxx::params{id});
            txn.commit();
        }
        if (AuditService::ready() && !call.ids().empty())
            AuditService::instance().log("stock.landed.cost", "button_validate", call.ids(), extractContext_(call).uid);
        return true;
    }
};

// ----------------------------------------------------------------
// StockLandedCostLineViewModel — cost lines, filtered by landed_cost_id
// ----------------------------------------------------------------
class StockLandedCostLineViewModel : public BaseViewModel {
public:
    explicit StockLandedCostLineViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read", handleSearchRead)
        REGISTER_METHOD("read",        handleRead)
        REGISTER_MUTATOR("create",      handleCreate)
        REGISTER_MUTATOR("write",       handleWrite)
        REGISTER_MUTATOR("unlink",      handleUnlink)
        REGISTER_METHOD("fields_get",  handleFieldsGet)
    }
    std::string modelName() const override { return "stock.landed.cost.line"; }
private:
    std::shared_ptr<DbConnection> db_;
    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        int lcId = 0;
        { auto d = call.domain(); if (d.is_array()) for (const auto& c : d)
            if (c.is_array() && c.size() == 3 && c[0].is_string() &&
                c[0].get<std::string>() == "landed_cost_id" && c[2].is_number_integer())
                lcId = c[2].get<int>(); }
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT l.id, l.landed_cost_id, l.name, l.product_id, pp.name AS product_name,
                   l.price, l.split_method, l.account_id
            FROM stock_landed_cost_line l
            LEFT JOIN product_product pp ON pp.id=l.product_id )";
        pqxx::params p;
        if (lcId > 0) { sql += " WHERE l.landed_cost_id = $1"; p.append(lcId); }
        sql += " ORDER BY l.id";
        auto res = lcId > 0 ? txn.exec(sql, p) : txn.exec(sql);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]             = row["id"].as<int>();
            j["landed_cost_id"] = row["landed_cost_id"].as<int>();
            j["name"]           = row["name"].is_null() ? "" : row["name"].c_str();
            j["product_id"]     = row["product_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["product_id"].as<int>(),
                                         row["product_name"].is_null() ? "" : std::string(row["product_name"].c_str())});
            j["price"]          = core::Money::fromMicros(row["price"].as<long long>(0)).toJson();
            j["split_method"]   = row["split_method"].is_null() ? "by_quantity" : row["split_method"].c_str();
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const CallKwArgs& call)      { StockLandedCostLineModel p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids()); }
    nlohmann::json handleCreate(const CallKwArgs& call)    { StockLandedCostLineModel p(db_); p.setUserContext(extractContext_(call)); return p.create(call.arg(0)); }
    nlohmann::json handleWrite(const CallKwArgs& call)     { StockLandedCostLineModel p(db_); p.setUserContext(extractContext_(call)); return p.write(call.ids(), call.arg(1)); }
    nlohmann::json handleUnlink(const CallKwArgs& call)    { StockLandedCostLineModel p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids()); }
    nlohmann::json handleFieldsGet(const CallKwArgs& call) { StockLandedCostLineModel p(db_); return p.fieldsGet(call.fields()); }
};

class StockLandedCostListView : public core::BaseView {
public:
    std::string viewName()  const override { return "stock.landed.cost.list"; }
    std::string modelName() const override { return "stock.landed.cost"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Landed Costs\">"
               "<field name=\"name\"/>"
               "<field name=\"date\"/>"
               "<field name=\"picking_id\"/>"
               "<field name=\"amount_total\"/>"
               "<field name=\"state\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",         {{"type","char"},      {"string","Reference"}}},
            {"date",         {{"type","date"},      {"string","Date"}}},
            {"picking_id",   {{"type","many2one"},  {"string","Receipt"}, {"relation","stock.picking"}}},
            {"amount_total", {{"type","monetary"},  {"string","Total"}}},
            {"state",        {{"type","selection"}, {"string","Status"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ----------------------------------------------------------------
// StockWarehouseOrderpoint — stock.warehouse.orderpoint (a reorder rule)
// ----------------------------------------------------------------
class StockWarehouseOrderpoint : public BaseModel<StockWarehouseOrderpoint> {
public:
    ODOO_MODEL("stock.warehouse.orderpoint", "stock_warehouse_orderpoint")

    int         productId    = 0;
    int         locationId   = 4;
    double      productMinQty = 0.0;
    double      productMaxQty = 0.0;
    double      qtyMultiple   = 1.0;
    std::string route         = "buy";   // buy | manufacture
    int         supplierId    = 0;
    int         companyId     = 0;
    bool        active        = true;

    explicit StockWarehouseOrderpoint(std::shared_ptr<DbConnection> db)
        : BaseModel<StockWarehouseOrderpoint>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"product_id",      FieldType::Many2one,"Product", true, false, true, true, "product.product"});
        fieldRegistry_.add({"location_id",     FieldType::Many2one,"Location", false, false, true, false, "stock.location"});
        fieldRegistry_.add({"product_min_qty", FieldType::Float,   "Min Quantity"});
        fieldRegistry_.add({"product_max_qty", FieldType::Float,   "Max Quantity"});
        fieldRegistry_.add({"qty_multiple",    FieldType::Float,   "Multiple Quantity"});
        fieldRegistry_.add({"route",           FieldType::Char,    "Route"});
        fieldRegistry_.add({"supplier_id",     FieldType::Many2one,"Vendor", false, false, true, false, "res.partner"});
        fieldRegistry_.add({"company_id",      FieldType::Many2one,"Company", false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",          FieldType::Boolean, "Active"});
        fieldRegistry_.markScaled({"product_min_qty", "product_max_qty", "qty_multiple"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["product_id"]      = productId  > 0 ? nlohmann::json(productId)  : nlohmann::json(false);
        j["location_id"]     = locationId > 0 ? nlohmann::json(locationId) : nlohmann::json(false);
        j["product_min_qty"] = productMinQty;
        j["product_max_qty"] = productMaxQty;
        j["qty_multiple"]    = qtyMultiple;
        j["route"]           = route.empty() ? "buy" : route;
        j["supplier_id"]     = supplierId > 0 ? nlohmann::json(supplierId) : nlohmann::json(false);
        j["company_id"]      = companyId  > 0 ? nlohmann::json(companyId)  : nlohmann::json(false);
        j["active"]          = active;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("product_min_qty") && j["product_min_qty"].is_number()) productMinQty = j["product_min_qty"].get<double>();
        if (j.contains("product_max_qty") && j["product_max_qty"].is_number()) productMaxQty = j["product_max_qty"].get<double>();
        if (j.contains("qty_multiple")    && j["qty_multiple"].is_number())    qtyMultiple   = j["qty_multiple"].get<double>();
        if (j.contains("route")           && j["route"].is_string())           route         = j["route"].get<std::string>();
        if (j.contains("active")          && j["active"].is_boolean())         active        = j["active"].get<bool>();
        if (const int v = parseM2o(j, "product_id"))  productId  = v;
        if (const int v = parseM2o(j, "location_id")) locationId = v;
        if (const int v = parseM2o(j, "supplier_id")) supplierId = v;
        if (const int v = parseM2o(j, "company_id"))  companyId  = v;
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (productId <= 0) e.push_back("Product is required");
        return e;
    }
};

// The reorder scheduler: for each active rule, if forecasted stock
// (on-hand + open incoming POs/MOs) is below the minimum, draft a
// replenishment up to the maximum, rounded to the multiple — a purchase
// order (buy) or a manufacturing order (make). Returns a summary.
static nlohmann::json runReorderScheduler(std::shared_ptr<DbConnection> db) {
    auto conn = db->acquire();
    pqxx::work txn{conn.get()};
    int checked = 0, created = 0;
    auto ops = txn.exec(
        "SELECT id, product_id, location_id, product_min_qty, product_max_qty, "
        "       qty_multiple, route, supplier_id, company_id "
        "FROM stock_warehouse_orderpoint WHERE active=TRUE");
    for (const auto& op : ops) {
        ++checked;
        const int         opId  = op["id"].as<int>();
        const int         prod  = op["product_id"].as<int>();
        const int         loc   = op["location_id"].is_null() ? 4 : op["location_id"].as<int>();
        const long long   minQ  = op["product_min_qty"].as<long long>(0);
        const long long   maxQ  = op["product_max_qty"].as<long long>(0);
        long long         mult  = op["qty_multiple"].as<long long>(0);
        const std::string route = op["route"].is_null() ? "buy" : op["route"].c_str();
        const int         supp  = op["supplier_id"].is_null() ? 0 : op["supplier_id"].as<int>();
        const int         comp  = op["company_id"].is_null() ? 1 : op["company_id"].as<int>();

        const long long onHand = txn.exec(
            "SELECT COALESCE(SUM(quantity),0) FROM stock_quant WHERE product_id=$1 AND location_id=$2",
            pqxx::params{prod, loc})[0][0].as<long long>(0);
        const long long inPo = txn.exec(
            "SELECT COALESCE(SUM(pol.product_qty - pol.qty_received),0) "
            "FROM purchase_order_line pol JOIN purchase_order po ON po.id=pol.order_id "
            "WHERE pol.product_id=$1 AND po.state IN ('draft','sent','purchase') "
            "AND pol.product_qty > pol.qty_received", pqxx::params{prod})[0][0].as<long long>(0);
        const long long inMo = txn.exec(
            "SELECT COALESCE(SUM(product_qty),0) FROM mrp_production "
            "WHERE product_id=$1 AND state NOT IN ('done','cancel')",
            pqxx::params{prod})[0][0].as<long long>(0);

        const long long virt = onHand + inPo + inMo;
        if (virt >= minQ) continue;
        long long toOrder = maxQ - virt;
        if (toOrder <= 0) continue;
        if (mult > 0) toOrder = ((toOrder + mult - 1) / mult) * mult;   // round up to the multiple
        if (toOrder <= 0) continue;

        const std::string origin = "Reordering: OP/" + std::to_string(opId);
        if (route == "manufacture") {
            txn.exec(
                "INSERT INTO mrp_production (name, product_id, product_qty, state, "
                "location_src_id, location_dest_id, company_id, origin) "
                "VALUES ('New',$1,$2,'draft',4,4,$3,$4)",
                pqxx::params{prod, toOrder, comp, origin});
            ++created;
        } else {  // buy
            auto pr = txn.exec("SELECT name, standard_price FROM product_product WHERE id=$1",
                               pqxx::params{prod});
            if (pr.empty()) continue;
            const std::string pname = pr[0]["name"].is_null() ? "Product" : pr[0]["name"].c_str();
            long long price = pr[0]["standard_price"].as<long long>(0);
            int       vendor = supp;
            // Prefer product.supplierinfo for the vendor and price: use the rule's
            // vendor if set (with that vendor's listed price), otherwise the
            // product's first vendor line.
            if (vendor > 0) {
                auto si = txn.exec(
                    "SELECT price FROM product_supplierinfo "
                    "WHERE product_id=$1 AND partner_id=$2 ORDER BY sequence,id LIMIT 1",
                    pqxx::params{prod, vendor});
                if (!si.empty() && si[0][0].as<long long>(0) > 0) price = si[0][0].as<long long>(price);
            } else {
                auto si = txn.exec(
                    "SELECT partner_id, price FROM product_supplierinfo "
                    "WHERE product_id=$1 ORDER BY sequence,id LIMIT 1", pqxx::params{prod});
                if (!si.empty()) {
                    vendor = si[0]["partner_id"].as<int>();
                    if (si[0]["price"].as<long long>(0) > 0) price = si[0]["price"].as<long long>(price);
                }
            }
            if (vendor <= 0) continue;   // no vendor on the rule or in the vendor pricelist
            const int poId = txn.exec(
                "INSERT INTO purchase_order (name, state, partner_id, date_order, company_id, origin) "
                "VALUES ('New','draft',$1,now(),$2,$3) RETURNING id",
                pqxx::params{vendor, comp, origin})[0][0].as<int>();
            txn.exec("UPDATE purchase_order SET name=$2 WHERE id=$1",
                     pqxx::params{poId, core::IrSequence::instance().nextByCode(txn, "purchase.order")});
            txn.exec(
                "INSERT INTO purchase_order_line (order_id, product_id, name, product_qty, product_uom_id, price_unit) "
                "VALUES ($1,$2,$3,$4,(SELECT uom_po_id FROM product_product WHERE id=$2),$5)",
                pqxx::params{poId, prod, pname, toOrder, price});
            ++created;
        }
    }
    txn.commit();
    nlohmann::json out;
    out["orderpoints_checked"]      = checked;
    out["replenishments_created"]   = created;
    return out;
}

// ----------------------------------------------------------------
// StockWarehouseOrderpointViewModel — CRUD + run_scheduler
// ----------------------------------------------------------------
class StockWarehouseOrderpointViewModel : public BaseViewModel {
public:
    explicit StockWarehouseOrderpointViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_METHOD("web_read",        handleRead)
        REGISTER_MUTATOR("create",          handleCreate)
        REGISTER_MUTATOR("write",           handleWrite)
        REGISTER_MUTATOR("unlink",          handleUnlink)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
        REGISTER_METHOD("run_scheduler",   handleRunScheduler)
    }
    std::string modelName() const override { return "stock.warehouse.orderpoint"; }
private:
    std::shared_ptr<DbConnection> db_;

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        const int lim = call.limit() > 0 ? call.limit() : 80;
        const int off = call.offset();
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT o.id, o.product_id, pp.name AS product_name, o.location_id,
                   o.product_min_qty, o.product_max_qty, o.qty_multiple, o.route,
                   o.supplier_id, rp.name AS supplier_name,
                   COALESCE(pp.qty_available,0) AS on_hand
            FROM stock_warehouse_orderpoint o
            LEFT JOIN product_product pp ON pp.id = o.product_id
            LEFT JOIN res_partner     rp ON rp.id = o.supplier_id
            WHERE o.active = TRUE
            ORDER BY o.id DESC)";
        sql += " LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);
        auto res = txn.exec(sql);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]              = row["id"].as<int>();
            j["product_id"]      = row["product_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["product_id"].as<int>(),
                                         row["product_name"].is_null() ? "" : std::string(row["product_name"].c_str())});
            j["product_min_qty"] = core::Money::fromMicros(row["product_min_qty"].as<long long>(0)).toJson();
            j["product_max_qty"] = core::Money::fromMicros(row["product_max_qty"].as<long long>(0)).toJson();
            j["qty_multiple"]    = core::Money::fromMicros(row["qty_multiple"].as<long long>(0)).toJson();
            j["route"]           = row["route"].is_null() ? "buy" : row["route"].c_str();
            j["on_hand"]         = core::Money::fromMicros(row["on_hand"].as<long long>(0)).toJson();
            j["supplier_id"]     = row["supplier_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["supplier_id"].as<int>(),
                                         row["supplier_name"].is_null() ? "" : std::string(row["supplier_name"].c_str())});
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const CallKwArgs& call)        { StockWarehouseOrderpoint p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids(), call.fields()); }
    nlohmann::json handleCreate(const CallKwArgs& call)      { StockWarehouseOrderpoint p(db_); p.setUserContext(extractContext_(call)); return p.create(call.arg(0)); }
    nlohmann::json handleWrite(const CallKwArgs& call)       { StockWarehouseOrderpoint p(db_); p.setUserContext(extractContext_(call)); return p.write(call.ids(), call.arg(1)); }
    nlohmann::json handleUnlink(const CallKwArgs& call)      { StockWarehouseOrderpoint p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids()); }
    nlohmann::json handleFieldsGet(const CallKwArgs& call)   { StockWarehouseOrderpoint p(db_); return p.fieldsGet(call.fields()); }
    nlohmann::json handleSearchCount(const CallKwArgs& call) { StockWarehouseOrderpoint p(db_); p.setUserContext(extractContext_(call)); return p.searchCount(call.domain()); }
    nlohmann::json handleRunScheduler(const CallKwArgs& call) {
        auto res = runReorderScheduler(db_);
        if (AuditService::ready())
            AuditService::instance().log("stock.warehouse.orderpoint", "run_scheduler", {}, extractContext_(call).uid);
        return res;
    }
};

class StockWarehouseOrderpointListView : public core::BaseView {
public:
    std::string viewName()  const override { return "stock.warehouse.orderpoint.list"; }
    std::string modelName() const override { return "stock.warehouse.orderpoint"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Reordering Rules\">"
               "<field name=\"product_id\"/>"
               "<field name=\"on_hand\"/>"
               "<field name=\"product_min_qty\"/>"
               "<field name=\"product_max_qty\"/>"
               "<field name=\"route\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"product_id",      {{"type","many2one"},  {"string","Product"}, {"relation","product.product"}}},
            {"on_hand",         {{"type","float"},     {"string","On Hand"}}},
            {"product_min_qty", {{"type","float"},     {"string","Min"}}},
            {"product_max_qty", {{"type","float"},     {"string","Max"}}},
            {"route",           {{"type","selection"}, {"string","Route"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ----------------------------------------------------------------
// StockPutawayRule — stock.putaway.rule (where incoming stock goes)
// ----------------------------------------------------------------
class StockPutawayRule : public BaseModel<StockPutawayRule> {
public:
    ODOO_MODEL("stock.putaway.rule", "stock_putaway_rule")

    int productId    = 0;
    int categoryId   = 0;
    int locationInId = 0;
    int locationOutId= 0;
    int sequence     = 10;
    int companyId    = 0;

    explicit StockPutawayRule(std::shared_ptr<DbConnection> db)
        : BaseModel<StockPutawayRule>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"product_id",      FieldType::Many2one,"Product",         false, false, true, true, "product.product"});
        fieldRegistry_.add({"category_id",     FieldType::Many2one,"Product Category", false, false, true, true, "product.category"});
        fieldRegistry_.add({"location_in_id",  FieldType::Many2one,"When arriving at", true,  false, true, false, "stock.location"});
        fieldRegistry_.add({"location_out_id", FieldType::Many2one,"Store to",         true,  false, true, false, "stock.location"});
        fieldRegistry_.add({"sequence",        FieldType::Integer, "Priority"});
        fieldRegistry_.add({"company_id",      FieldType::Many2one,"Company",          false, false, true, false, "res.company"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["product_id"]      = productId     > 0 ? nlohmann::json(productId)     : nlohmann::json(false);
        j["category_id"]     = categoryId    > 0 ? nlohmann::json(categoryId)    : nlohmann::json(false);
        j["location_in_id"]  = locationInId  > 0 ? nlohmann::json(locationInId)  : nlohmann::json(false);
        j["location_out_id"] = locationOutId > 0 ? nlohmann::json(locationOutId) : nlohmann::json(false);
        j["sequence"]        = sequence;
        j["company_id"]      = companyId     > 0 ? nlohmann::json(companyId)     : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("sequence") && j["sequence"].is_number()) sequence = j["sequence"].get<int>();
        if (const int v = parseM2o(j, "product_id"))      productId     = v;
        if (const int v = parseM2o(j, "category_id"))     categoryId    = v;
        if (const int v = parseM2o(j, "location_in_id"))  locationInId  = v;
        if (const int v = parseM2o(j, "location_out_id")) locationOutId = v;
        if (const int v = parseM2o(j, "company_id"))      companyId     = v;
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (locationInId  <= 0) e.push_back("Arrival location is required");
        if (locationOutId <= 0) e.push_back("Destination location is required");
        return e;
    }
};

class StockPutawayRuleListView : public core::BaseView {
public:
    std::string viewName()  const override { return "stock.putaway.rule.list"; }
    std::string modelName() const override { return "stock.putaway.rule"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Putaway Rules\">"
               "<field name=\"product_id\"/>"
               "<field name=\"category_id\"/>"
               "<field name=\"location_in_id\"/>"
               "<field name=\"location_out_id\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"product_id",      {{"type","many2one"}, {"string","Product"},           {"relation","product.product"}}},
            {"category_id",     {{"type","many2one"}, {"string","Product Category"},   {"relation","product.category"}}},
            {"location_in_id",  {{"type","many2one"}, {"string","When arriving at"},   {"relation","stock.location"}}},
            {"location_out_id", {{"type","many2one"}, {"string","Store to"},           {"relation","stock.location"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ================================================================

// ================================================================
// MODULE IMPLEMENTATIONS
// ================================================================
StockModule::StockModule(core::ModelFactory&     modelFactory,
                         core::ServiceFactory&   serviceFactory,
                         core::ViewModelFactory& viewModelFactory,
                         core::ViewFactory&      viewFactory)
    : models_    (modelFactory)
    , services_  (serviceFactory)
    , viewModels_(viewModelFactory)
    , views_     (viewFactory)
{}

std::string              StockModule::moduleName()   const { return "stock"; }
std::string              StockModule::version()      const { return "19.0.1.0.0"; }
std::vector<std::string> StockModule::dependencies() const { return {"product", "sale", "purchase"}; }

void StockModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("stock.location",     [db]{ return std::make_shared<StockLocation>(db); });
    models_.registerCreator("stock.picking.type", [db]{ return std::make_shared<StockPickingType>(db); });
    models_.registerCreator("stock.picking",      [db]{ return std::make_shared<StockPicking>(db); });
    models_.registerCreator("stock.move",         [db]{ return std::make_shared<StockMove>(db); });
    models_.registerCreator("stock.warehouse",    [db]{ return std::make_shared<StockWarehouse>(db); });
    models_.registerCreator("stock.quant",        [db]{ return std::make_shared<StockQuantModel>(db); });
    models_.registerCreator("stock.valuation.layer", [db]{ return std::make_shared<StockValuationLayerModel>(db); });
    models_.registerCreator("stock.production.lot",  [db]{ return std::make_shared<StockProductionLotModel>(db); });
    models_.registerCreator("stock.landed.cost",      [db]{ return std::make_shared<StockLandedCostModel>(db); });
    models_.registerCreator("stock.landed.cost.line", [db]{ return std::make_shared<StockLandedCostLineModel>(db); });
    models_.registerCreator("stock.warehouse.orderpoint", [db]{ return std::make_shared<StockWarehouseOrderpoint>(db); });
    models_.registerCreator("stock.putaway.rule",         [db]{ return std::make_shared<StockPutawayRule>(db); });
}

void StockModule::registerServices() {}

void StockModule::registerViews() {
    views_.registerView<StockPickingListView>("stock.picking.list");
    views_.registerView<StockPickingFormView>("stock.picking.form");
    views_.registerView<StockMoveListView>   ("stock.move.list");
    views_.registerView<StockQuantListView>  ("stock.quant.list");
    views_.registerView<StockValuationLayerListView>("stock.valuation.layer.list");
    views_.registerView<StockProductionLotListView>("stock.production.lot.list");
    views_.registerView<StockLandedCostListView>("stock.landed.cost.list");
    views_.registerView<StockWarehouseOrderpointListView>("stock.warehouse.orderpoint.list");
    views_.registerView<StockPutawayRuleListView>("stock.putaway.rule.list");
}

void StockModule::registerViewModels() {
    auto db = services_.db();
    viewModels_.registerCreator("stock.picking", [db]{
        return std::make_shared<StockPickingViewModel>(db);
    });
    viewModels_.registerCreator("stock.move", [db]{
        return std::make_shared<StockMoveViewModel>(db);
    });
    viewModels_.registerCreator("stock.location", [db]{
        return std::make_shared<GenericViewModel<StockLocation>>(db);
    });
    viewModels_.registerCreator("stock.picking.type", [db]{
        return std::make_shared<GenericViewModel<StockPickingType>>(db);
    });
    viewModels_.registerCreator("stock.warehouse", [db]{
        return std::make_shared<GenericViewModel<StockWarehouse>>(db);
    });
    viewModels_.registerCreator("stock.quant", [db]{
        return std::make_shared<StockQuantViewModel>(db);
    });
    viewModels_.registerCreator("stock.valuation.layer", [db]{
        return std::make_shared<StockValuationLayerViewModel>(db);
    });
    viewModels_.registerCreator("stock.production.lot", [db]{
        return std::make_shared<StockProductionLotViewModel>(db);
    });
    viewModels_.registerCreator("stock.landed.cost", [db]{
        return std::make_shared<StockLandedCostViewModel>(db);
    });
    viewModels_.registerCreator("stock.landed.cost.line", [db]{
        return std::make_shared<StockLandedCostLineViewModel>(db);
    });
    viewModels_.registerCreator("stock.warehouse.orderpoint", [db]{
        return std::make_shared<StockWarehouseOrderpointViewModel>(db);
    });
    viewModels_.registerCreator("stock.putaway.rule", [db]{
        return std::make_shared<GenericViewModel<StockPutawayRule>>(db);
    });
}

void StockModule::initialize() {
    ensureSchema_();
    seedLocations_();
    seedPickingTypes_();
    seedWarehouses_();
    seedMenus_();
    // Bind the reordering scheduler to its cron code (the DB row decides when).
    if (core::IrCron::ready()) {
        auto db = services_.db();
        core::IrCron::instance().registerJob("stock.reorder", [db] { runReorderScheduler(db); });
    }
}

void StockModule::ensureSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS stock_location (
            id            SERIAL  PRIMARY KEY,
            name          VARCHAR NOT NULL,
            complete_name VARCHAR,
            location_id   INTEGER REFERENCES stock_location(id) ON DELETE SET NULL,
            usage         VARCHAR NOT NULL DEFAULT 'internal',
            company_id    INTEGER REFERENCES res_company(id)    ON DELETE SET NULL,
            active        BOOLEAN NOT NULL DEFAULT TRUE,
            create_date   TIMESTAMP DEFAULT now(),
            write_date    TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS stock_picking_type (
            id                       SERIAL  PRIMARY KEY,
            name                     VARCHAR NOT NULL,
            code                     VARCHAR NOT NULL,
            sequence_prefix          VARCHAR NOT NULL DEFAULT 'WH/',
            default_location_src_id  INTEGER REFERENCES stock_location(id) ON DELETE SET NULL,
            default_location_dest_id INTEGER REFERENCES stock_location(id) ON DELETE SET NULL,
            company_id               INTEGER REFERENCES res_company(id)    ON DELETE SET NULL,
            active                   BOOLEAN NOT NULL DEFAULT TRUE,
            create_date              TIMESTAMP DEFAULT now(),
            write_date               TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS stock_picking (
            id               SERIAL  PRIMARY KEY,
            name             VARCHAR NOT NULL DEFAULT 'New',
            picking_type_id  INTEGER NOT NULL REFERENCES stock_picking_type(id),
            state            VARCHAR NOT NULL DEFAULT 'draft',
            partner_id       INTEGER REFERENCES res_partner(id)      ON DELETE SET NULL,
            location_id      INTEGER NOT NULL REFERENCES stock_location(id),
            location_dest_id INTEGER NOT NULL REFERENCES stock_location(id),
            scheduled_date   TIMESTAMP,
            origin           VARCHAR,
            company_id       INTEGER REFERENCES res_company(id)      ON DELETE SET NULL,
            sale_id          INTEGER REFERENCES sale_order(id)        ON DELETE SET NULL,
            purchase_id      INTEGER REFERENCES purchase_order(id)    ON DELETE SET NULL,
            create_date      TIMESTAMP DEFAULT now(),
            write_date       TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS stock_move (
            id               SERIAL  PRIMARY KEY,
            picking_id       INTEGER NOT NULL REFERENCES stock_picking(id) ON DELETE CASCADE,
            product_id       INTEGER NOT NULL REFERENCES product_product(id),
            product_uom_id   INTEGER REFERENCES uom_uom(id)          ON DELETE SET NULL,
            name             TEXT    NOT NULL,
            product_uom_qty  NUMERIC(16,4) NOT NULL DEFAULT 0,
            quantity         NUMERIC(16,4) NOT NULL DEFAULT 0,
            state            VARCHAR NOT NULL DEFAULT 'draft',
            location_id      INTEGER NOT NULL REFERENCES stock_location(id),
            location_dest_id INTEGER NOT NULL REFERENCES stock_location(id),
            company_id       INTEGER REFERENCES res_company(id)      ON DELETE SET NULL,
            origin           VARCHAR,
            create_date      TIMESTAMP DEFAULT now(),
            write_date       TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS stock_warehouse (
            id               SERIAL  PRIMARY KEY,
            name             VARCHAR NOT NULL,
            code             VARCHAR(5) NOT NULL UNIQUE,
            company_id       INTEGER REFERENCES res_company(id)        ON DELETE SET NULL,
            lot_stock_id     INTEGER REFERENCES stock_location(id)     ON DELETE SET NULL,
            view_location_id INTEGER REFERENCES stock_location(id)     ON DELETE SET NULL,
            in_type_id       INTEGER REFERENCES stock_picking_type(id) ON DELETE SET NULL,
            out_type_id      INTEGER REFERENCES stock_picking_type(id) ON DELETE SET NULL,
            int_type_id      INTEGER REFERENCES stock_picking_type(id) ON DELETE SET NULL,
            active           BOOLEAN NOT NULL DEFAULT TRUE,
            create_date      TIMESTAMP DEFAULT now(),
            write_date       TIMESTAMP DEFAULT now()
        )
    )");

    // stock_quant — on-hand ledger. One row per (product, location); quantity
    // and reserved_quantity are BIGINT micro-units (scale 6), matching
    // stock_move. The quant engine (core/StockQuant) is the only writer.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS stock_quant (
            id                SERIAL  PRIMARY KEY,
            product_id        INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
            location_id       INTEGER NOT NULL REFERENCES stock_location(id)  ON DELETE CASCADE,
            lot_id            INTEGER NOT NULL DEFAULT 0,
            quantity          BIGINT  NOT NULL DEFAULT 0,
            reserved_quantity BIGINT  NOT NULL DEFAULT 0,
            company_id        INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date       TIMESTAMP DEFAULT now(),
            write_date        TIMESTAMP DEFAULT now(),
            UNIQUE (product_id, location_id, lot_id)
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_stock_quant_product ON stock_quant(product_id)");
    // Lots/serial: on-hand keyed per (product, location, lot). lot_id 0 = untracked.
    // Existing DB migration — add lot_id and widen the unique key.
    txn.exec("ALTER TABLE stock_quant ADD COLUMN IF NOT EXISTS lot_id INTEGER NOT NULL DEFAULT 0");
    txn.exec("ALTER TABLE stock_quant DROP CONSTRAINT IF EXISTS stock_quant_product_id_location_id_key");
    txn.exec(R"(
        DO $$ BEGIN
            IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname='stock_quant_pll_key') THEN
                ALTER TABLE stock_quant ADD CONSTRAINT stock_quant_pll_key
                    UNIQUE (product_id, location_id, lot_id);
            END IF;
        END $$;
    )");
    // Per-move reservation, so unreserve/validate release the exact amount held.
    txn.exec("ALTER TABLE stock_move ADD COLUMN IF NOT EXISTS reserved_qty BIGINT NOT NULL DEFAULT 0");
    // Lots/serial: the lot a move carries (NULL = untracked).
    txn.exec("ALTER TABLE stock_move ADD COLUMN IF NOT EXISTS lot_id INTEGER");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_stock_move_lot ON stock_move(lot_id)");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS stock_production_lot (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            product_id  INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
            ref         VARCHAR,
            company_id  INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now(),
            UNIQUE (name, product_id, company_id)
        )
    )");

    // stock_valuation_layer — the inventory-value ledger. One row per move that
    // crosses the owned-stock boundary. quantity/value are SIGNED micro-units;
    // remaining_* carry FIFO cost layers; counterpart_usage records the virtual
    // side (supplier/customer/production/inventory/subcontract) so the GL posting
    // (Phase B) can pick the offset account; account_move_id links the posted JE.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS stock_valuation_layer (
            id                SERIAL  PRIMARY KEY,
            product_id        INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
            quantity          BIGINT  NOT NULL DEFAULT 0,
            unit_cost         BIGINT  NOT NULL DEFAULT 0,
            value             BIGINT  NOT NULL DEFAULT 0,
            remaining_qty     BIGINT  NOT NULL DEFAULT 0,
            remaining_value   BIGINT  NOT NULL DEFAULT 0,
            counterpart_usage VARCHAR,
            description       VARCHAR,
            account_move_id   INTEGER REFERENCES account_move(id) ON DELETE SET NULL,
            company_id        INTEGER REFERENCES res_company(id)  ON DELETE SET NULL,
            create_date       TIMESTAMP DEFAULT now(),
            write_date        TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_svl_product ON stock_valuation_layer(product_id)");

    // Landed costs: extra costs (freight/duty/handling) added to a receipt and
    // distributed across the received products, raising their inventory value.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS stock_landed_cost (
            id          SERIAL  PRIMARY KEY,
            name        VARCHAR NOT NULL DEFAULT 'New',
            date        DATE    NOT NULL DEFAULT CURRENT_DATE,
            picking_id  INTEGER REFERENCES stock_picking(id) ON DELETE SET NULL,
            state       VARCHAR NOT NULL DEFAULT 'draft',
            company_id  INTEGER REFERENCES res_company(id)   ON DELETE SET NULL,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS stock_landed_cost_line (
            id              SERIAL  PRIMARY KEY,
            landed_cost_id  INTEGER NOT NULL REFERENCES stock_landed_cost(id) ON DELETE CASCADE,
            name            VARCHAR NOT NULL,
            product_id      INTEGER REFERENCES product_product(id) ON DELETE SET NULL,
            price           BIGINT  NOT NULL DEFAULT 0,
            split_method    VARCHAR NOT NULL DEFAULT 'by_quantity',
            account_id      INTEGER REFERENCES account_account(id) ON DELETE SET NULL,
            create_date     TIMESTAMP DEFAULT now(),
            write_date      TIMESTAMP DEFAULT now()
        )
    )");

    // Reordering rules (orderpoints) + the scheduler cron row (inactive until enabled).
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS stock_warehouse_orderpoint (
            id              SERIAL  PRIMARY KEY,
            product_id      INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
            location_id     INTEGER REFERENCES stock_location(id) ON DELETE SET NULL,
            product_min_qty BIGINT  NOT NULL DEFAULT 0,
            product_max_qty BIGINT  NOT NULL DEFAULT 0,
            qty_multiple    BIGINT  NOT NULL DEFAULT 1000000,
            route           VARCHAR NOT NULL DEFAULT 'buy',
            supplier_id     INTEGER REFERENCES res_partner(id) ON DELETE SET NULL,
            company_id      INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            active          BOOLEAN NOT NULL DEFAULT TRUE,
            create_date     TIMESTAMP DEFAULT now(),
            write_date      TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(
        "INSERT INTO ir_cron (code, name, interval_minutes, active) "
        "SELECT 'stock.reorder', 'Reordering scheduler', 1440, FALSE "
        "WHERE NOT EXISTS (SELECT 1 FROM ir_cron c WHERE c.code='stock.reorder')");

    // Putaway rules: route incoming products to a designated sub-location.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS stock_putaway_rule (
            id              SERIAL  PRIMARY KEY,
            product_id      INTEGER REFERENCES product_product(id)  ON DELETE CASCADE,
            category_id     INTEGER REFERENCES product_category(id) ON DELETE CASCADE,
            location_in_id  INTEGER NOT NULL REFERENCES stock_location(id) ON DELETE CASCADE,
            location_out_id INTEGER NOT NULL REFERENCES stock_location(id) ON DELETE CASCADE,
            sequence        INTEGER NOT NULL DEFAULT 10,
            company_id      INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date     TIMESTAMP DEFAULT now(),
            write_date      TIMESTAMP DEFAULT now()
        )
    )");
    // Barcode fields (product already has one).
    txn.exec("ALTER TABLE stock_location ADD COLUMN IF NOT EXISTS barcode VARCHAR");
    txn.exec("ALTER TABLE stock_production_lot ADD COLUMN IF NOT EXISTS barcode VARCHAR");

    txn.exec("ALTER TABLE stock_picking ADD COLUMN IF NOT EXISTS user_id INTEGER REFERENCES res_users(id)");
    txn.exec("CREATE SEQUENCE IF NOT EXISTS stock_in_seq  START 1 INCREMENT 1");
    txn.exec("CREATE SEQUENCE IF NOT EXISTS stock_out_seq START 1 INCREMENT 1");
    txn.exec("CREATE SEQUENCE IF NOT EXISTS stock_int_seq START 1 INCREMENT 1");
    txn.commit();
}

void StockModule::seedLocations_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(R"(
        INSERT INTO stock_location (id, name, complete_name, location_id, usage, company_id) VALUES
            (1, 'Virtual Locations',    'Virtual Locations',             NULL, 'view',     NULL),
            (2, 'Physical Locations',   'Physical Locations',            NULL, 'view',     NULL),
            (3, 'WH',                   'WH',                            2,    'view',     1),
            (4, 'Stock',                'WH/Stock',                      3,    'internal', 1),
            (5, 'Vendors',              'Partners/Vendors',               1,    'supplier', NULL),
            (6, 'Customers',            'Partners/Customers',             1,    'customer', NULL),
            (7, 'Inventory Adjustments','Virtual Locations/Inventory',   1,    'inventory',NULL)
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec("SELECT setval('stock_location_id_seq', (SELECT MAX(id) FROM stock_location), true)");
    txn.commit();
}

void StockModule::seedPickingTypes_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(R"(
        INSERT INTO stock_picking_type
            (id, name, code, sequence_prefix, default_location_src_id, default_location_dest_id, company_id) VALUES
            (1, 'Receipts',   'incoming', 'WH/IN/',  5, 4, 1),
            (2, 'Deliveries', 'outgoing', 'WH/OUT/', 4, 6, 1),
            (3, 'Internal',   'internal', 'WH/INT/', 4, 4, 1)
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec("SELECT setval('stock_picking_type_id_seq', (SELECT MAX(id) FROM stock_picking_type), true)");
    txn.commit();
}

void StockModule::seedWarehouses_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(R"(
        INSERT INTO stock_warehouse
            (id, name, code, company_id, lot_stock_id, view_location_id, in_type_id, out_type_id, int_type_id)
        VALUES (1, 'Main Warehouse', 'WH', 1, 4, 3, 1, 2, 3)
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec("SELECT setval('stock_warehouse_id_seq', (SELECT MAX(id) FROM stock_warehouse), true)");
    txn.commit();
}

void StockModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, domain, context) VALUES
            (17, 'All Transfers',       'stock.picking',      'list,form', '[]', '{}'),
            (18, 'Locations',           'stock.location',     'list,form', '[]', '{}'),
            (19, 'Operation Types',     'stock.picking.type', 'list,form', '[]', '{}'),
            (20, 'Products',            'product.product',    'list,form', '[]', '{}'),
            (21, 'Moves History',       'stock.move',         'list,form', '[]', '{}'),
            (22, 'Receipts',            'stock.picking',      'list,form', '[["picking_type_id","=",1]]', '{}'),
            (23, 'Deliveries',          'stock.picking',      'list,form', '[["picking_type_id","=",2]]', '{}'),
            (24, 'Internal Transfers',  'stock.picking',      'list,form', '[["picking_type_id","=",3]]', '{}'),
            (25, 'Warehouses',          'stock.warehouse',       'list,form', '[]', '{}'),
            (26, 'On Hand',             'stock.quant',           'list',      '[]', '{}'),
            (27, 'Inventory Valuation', 'stock.valuation.layer', 'list',      '[]', '{}'),
            (28, 'Lots/Serial Numbers', 'stock.production.lot',  'list,form', '[]', '{}'),
            (29, 'Landed Costs',        'stock.landed.cost',     'list,form', '[]', '{}'),
            -- id 30 is owned by ReportModule ('Document Templates') and is seeded
            -- there with ON CONFLICT DO UPDATE, so it won this id and the
            -- Reordering Rules menu opened the template editor. Use 94.
            (94, 'Reordering Rules',    'stock.warehouse.orderpoint', 'list,form', '[]', '{}'),
            (31, 'Putaway Rules',       'stock.putaway.rule',    'list,form', '[]', '{}'),
            (47, 'Barcode',             'barcode.scan',          'list',      '[]', '{}')
        ON CONFLICT (id) DO UPDATE
            SET name = EXCLUDED.name, domain = EXCLUDED.domain
    )");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");

    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id, web_icon) VALUES
            (90, 'Inventory', NULL, 50, NULL, 'inventory')
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (91, 'Operations',    90, 10, NULL),
            (96, 'Products',      90, 20, NULL),
            (97, 'Reporting',     90, 30, NULL),
            (92, 'Configuration', 90, 90, NULL)
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
            sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (200, 'Receipts',           91, 10, 22),
            (201, 'Deliveries',         91, 20, 23),
            (202, 'Internal Transfers', 91, 30, 24),
            (95,  'All Transfers',      91, 40, 17),
            (207, 'Landed Costs',       91, 50, 29),
            (208, 'Reordering Rules',   92, 30, 94),
            (210, 'Barcode',            91, 60, 47)
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
            sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (98,  'Products',            96, 10, 20),
            (99,  'Moves History',       97, 10, 21),
            (204, 'On Hand',             97,  5, 26),
            (205, 'Inventory Valuation', 97,  7, 27),
            (206, 'Lots/Serial Numbers', 96, 30, 28)
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
            sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (203, 'Warehouses',      92,  5, 25),
            (93,  'Locations',       92, 10, 18),
            (94,  'Operation Types', 92, 20, 19),
            (209, 'Putaway Rules',   92, 40, 31)
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
            sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");
    txn.commit();
}

void StockModule::registerRoutes()   {}
} // namespace odoo::modules::stock

#pragma once
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
#include "BaseView.hpp"
#include "BaseViewModel.hpp"
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>

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

    explicit StockLocation(std::shared_ptr<DbConnection> db)
        : BaseModel<StockLocation>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",          FieldType::Char,    "Location Name", true});
        fieldRegistry_.add({"complete_name",  FieldType::Char,    "Complete Name"});
        fieldRegistry_.add({"location_id",   FieldType::Many2one,"Parent",        false, false, true, false, "stock.location"});
        fieldRegistry_.add({"usage",         FieldType::Char,    "Usage"});
        fieldRegistry_.add({"company_id",    FieldType::Many2one,"Company",       false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",        FieldType::Boolean, "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]          = name;
        j["complete_name"] = completeName.empty() ? name : completeName;
        j["location_id"]   = locationId > 0 ? nlohmann::json(locationId) : nlohmann::json(false);
        j["usage"]         = usage;
        j["company_id"]    = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["active"]        = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")          && j["name"].is_string())          name         = j["name"].get<std::string>();
        if (j.contains("complete_name") && j["complete_name"].is_string()) completeName = j["complete_name"].get<std::string>();
        if (j.contains("usage")         && j["usage"].is_string())         usage        = j["usage"].get<std::string>();
        if (j.contains("active")        && j["active"].is_boolean())       active       = j["active"].get<bool>();
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
        REGISTER_METHOD("create",          handleCreate)
        REGISTER_METHOD("write",           handleWrite)
        REGISTER_METHOD("unlink",          handleUnlink)
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
                   rp.name AS partner_name
            FROM stock_picking sp
            LEFT JOIN stock_location sl_src ON sl_src.id = sp.location_id
            LEFT JOIN stock_location sl_dst ON sl_dst.id = sp.location_dest_id
            LEFT JOIN res_partner    rp     ON rp.id     = sp.partner_id
            ORDER BY sp.id DESC
        )";
        sql += " LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);

        auto res = txn.exec(sql);

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
        return proto.read(call.ids(), call.fields());
    }
    nlohmann::json handleCreate(const CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("create: args[0] must be a dict");
        StockPicking proto(db_);
        return proto.create(v);
    }
    nlohmann::json handleWrite(const CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        StockPicking proto(db_);
        return proto.write(call.ids(), v);
    }
    nlohmann::json handleUnlink(const CallKwArgs& call) {
        StockPicking proto(db_);
        return proto.unlink(call.ids());
    }
    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        StockPicking proto(db_);
        return proto.fieldsGet(call.fields());
    }
    nlohmann::json handleSearchCount(const CallKwArgs& call) {
        StockPicking proto(db_);
        return proto.searchCount(call.domain());
    }
    nlohmann::json handleSearch(const CallKwArgs& call) {
        StockPicking proto(db_);
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
            std::string seqName  = "stock_in_seq";
            std::string prefix   = "WH/IN/";
            if (!pt.empty()) {
                const std::string code = pt[0]["code"].c_str();
                if (code == "outgoing") { seqName = "stock_out_seq"; prefix = "WH/OUT/"; }
                else if (code == "internal") { seqName = "stock_int_seq"; prefix = "WH/INT/"; }
            }

            // Generate sequence number
            auto seqRes = txn.exec("SELECT nextval('" + seqName + "')");
            const long long seq = seqRes[0][0].as<long long>();
            const std::string year = txn.exec("SELECT to_char(now(),'YYYY')")[0][0].c_str();
            const std::string ref  = prefix + year + "/" + std::string(4 - std::min(4, (int)std::to_string(seq).size()), '0') + std::to_string(seq);

            txn.exec(
                "UPDATE stock_picking SET state='confirmed', name=$1, write_date=now() WHERE id=$2",
                pqxx::params{ref, id});

            // Also confirm all child moves
            txn.exec(
                "UPDATE stock_move SET state='confirmed' WHERE picking_id=$1 AND state='draft'",
                pqxx::params{id});

            txn.commit();
        }
        return true;
    }

    // ----------------------------------------------------------
    // action_assign — confirmed → assigned (availability check stub)
    // ----------------------------------------------------------
    nlohmann::json handleActionAssign(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            txn.exec(
                "UPDATE stock_picking SET state='assigned', write_date=now() "
                "WHERE id=$1 AND state IN ('confirmed','partially_available')",
                pqxx::params{id});
            txn.exec(
                "UPDATE stock_move SET state='assigned' "
                "WHERE picking_id=$1 AND state='confirmed'",
                pqxx::params{id});
            txn.commit();
        }
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

            // For moves where quantity (done) = 0, default to product_uom_qty (demand)
            txn.exec(
                "UPDATE stock_move "
                "SET quantity = product_uom_qty "
                "WHERE picking_id=$1 AND quantity = 0 AND product_uom_qty > 0",
                pqxx::params{id});

            // Mark all moves done
            txn.exec(
                "UPDATE stock_move SET state='done', write_date=now() WHERE picking_id=$1",
                pqxx::params{id});

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
                // Re-evaluate purchase order billing_status
                txn.exec(R"(
                    UPDATE purchase_order po
                    SET billing_status = CASE
                        WHEN (SELECT COUNT(*) FROM purchase_order_line
                              WHERE order_id=po.id AND qty_received < product_qty) = 0
                        THEN 'to_bill'
                        ELSE billing_status
                    END,
                    write_date=now()
                    WHERE id=$1 AND state='purchase'
                )", pqxx::params{purchaseId});
            }

            txn.commit();
        }
        return true;
    }

    // ----------------------------------------------------------
    // action_cancel
    // ----------------------------------------------------------
    nlohmann::json handleActionCancel(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            txn.exec(
                "UPDATE stock_picking SET state='cancel', write_date=now() "
                "WHERE id=$1 AND state != 'done'",
                pqxx::params{id});
            txn.exec(
                "UPDATE stock_move SET state='cancel' "
                "WHERE picking_id=$1 AND state != 'done'",
                pqxx::params{id});
            txn.commit();
        }
        return true;
    }

    // ----------------------------------------------------------
    // button_unreserve — assigned → confirmed (release reservation)
    // ----------------------------------------------------------
    nlohmann::json handleButtonUnreserve(const CallKwArgs& call) {
        for (int id : call.ids()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            txn.exec(
                "UPDATE stock_picking SET state='confirmed', write_date=now() "
                "WHERE id=$1 AND state='assigned'",
                pqxx::params{id});
            txn.exec(
                "UPDATE stock_move SET state='confirmed' "
                "WHERE picking_id=$1 AND state='assigned'",
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
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ================================================================

class StockModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "stock"; }

    explicit StockModule(core::ModelFactory&     modelFactory,
                         core::ServiceFactory&   serviceFactory,
                         core::ViewModelFactory& viewModelFactory,
                         core::ViewFactory&      viewFactory)
        : models_    (modelFactory)
        , services_  (serviceFactory)
        , viewModels_(viewModelFactory)
        , views_     (viewFactory)
    {}

    std::string              moduleName()   const override { return "stock"; }
    std::string              version()      const override { return "19.0.1.0.0"; }
    std::vector<std::string> dependencies() const override { return {"product", "sale", "purchase"}; }

    void registerModels() override {
        auto db = services_.db();
        models_.registerCreator("stock.location",     [db]{ return std::make_shared<StockLocation>(db); });
        models_.registerCreator("stock.picking.type", [db]{ return std::make_shared<StockPickingType>(db); });
        models_.registerCreator("stock.picking",      [db]{ return std::make_shared<StockPicking>(db); });
        models_.registerCreator("stock.move",         [db]{ return std::make_shared<StockMove>(db); });
    }

    void registerServices() override {}
    void registerViews()    override {
        views_.registerView<StockPickingListView>("stock.picking.list");
        views_.registerView<StockPickingFormView>("stock.picking.form");
    }

    void registerViewModels() override {
        auto db = services_.db();
        viewModels_.registerCreator("stock.picking", [db]{
            return std::make_shared<StockPickingViewModel>(db);
        });
        viewModels_.registerCreator("stock.move", [db]{
            return std::make_shared<GenericViewModel<StockMove>>(db);
        });
        viewModels_.registerCreator("stock.location", [db]{
            return std::make_shared<GenericViewModel<StockLocation>>(db);
        });
        viewModels_.registerCreator("stock.picking.type", [db]{
            return std::make_shared<GenericViewModel<StockPickingType>>(db);
        });
    }

    void initialize() override {
        ensureSchema_();
        seedLocations_();
        seedPickingTypes_();
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

        // Sequences
        txn.exec("CREATE SEQUENCE IF NOT EXISTS stock_in_seq  START 1 INCREMENT 1");
        txn.exec("CREATE SEQUENCE IF NOT EXISTS stock_out_seq START 1 INCREMENT 1");
        txn.exec("CREATE SEQUENCE IF NOT EXISTS stock_int_seq START 1 INCREMENT 1");

        txn.commit();
    }

    // ----------------------------------------------------------
    // Seed default locations
    // ----------------------------------------------------------
    void seedLocations_() {
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

    // ----------------------------------------------------------
    // Seed default picking types
    // ----------------------------------------------------------
    void seedPickingTypes_() {
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

    // ----------------------------------------------------------
    // Seed IR menus
    // ----------------------------------------------------------
    void seedMenus_() {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};

        // ── Window actions ──────────────────────────────────────────
        txn.exec(R"(
            INSERT INTO ir_act_window (id, name, res_model, view_mode, context) VALUES
                (17, 'Transfers',      'stock.picking',      'list,form', '{}'),
                (18, 'Locations',      'stock.location',     'list,form', '{}'),
                (19, 'Operation Types','stock.picking.type', 'list,form', '{}'),
                (20, 'Products',       'product.product',    'list,form', '{}'),
                (21, 'Moves History',  'stock.move',         'list,form', '{}')
            ON CONFLICT (id) DO NOTHING
        )");
        txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");

        // ── Inventory app tile ──────────────────────────────────────
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id, web_icon) VALUES
                (90, 'Inventory', NULL, 50, NULL, 'inventory')
            ON CONFLICT (id) DO NOTHING
        )");

        // ── Top-level sections (Operations, Products, Reporting, Configuration)
        // id=91 existed as "Transfers" — update it to become "Operations" parent
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
                (91, 'Operations',    90, 10, NULL),
                (96, 'Products',      90, 20, NULL),
                (97, 'Reporting',     90, 30, NULL),
                (92, 'Configuration', 90, 90, NULL)
            ON CONFLICT (id) DO UPDATE
                SET name      = EXCLUDED.name,
                    parent_id = EXCLUDED.parent_id,
                    sequence  = EXCLUDED.sequence,
                    action_id = EXCLUDED.action_id
        )");

        // ── Operations sub-menu ──────────────────────────────────────
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
                (95, 'Transfers', 91, 10, 17)
            ON CONFLICT (id) DO NOTHING
        )");

        // ── Products sub-menu ────────────────────────────────────────
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
                (98, 'Products', 96, 10, 20)
            ON CONFLICT (id) DO NOTHING
        )");

        // ── Reporting sub-menu ───────────────────────────────────────
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
                (99, 'Moves History', 97, 10, 21)
            ON CONFLICT (id) DO NOTHING
        )");

        // ── Configuration sub-menu (Locations, Operation Types) ─────
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
                (93, 'Locations',       92, 10, 18),
                (94, 'Operation Types', 92, 20, 19)
            ON CONFLICT (id) DO NOTHING
        )");

        txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");
        txn.commit();
    }
};

} // namespace odoo::modules::stock

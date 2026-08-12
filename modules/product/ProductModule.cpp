// =============================================================
// modules/product/ProductModule.cpp
// =============================================================
#include "ProductModule.hpp"
#include "BaseModel.hpp"
#include "RecordRuleSql.hpp"
#include "DecimalPrecision.hpp"
#include "BaseView.hpp"
#include "GenericViewModel.hpp"
#include "BaseViewModel.hpp"
#include "Money.hpp"
#include "DbConnection.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>
#include <set>

namespace odoo::modules::product {

using namespace odoo::infrastructure;
using namespace odoo::core;

// ================================================================
// 1. MODELS
// ================================================================

// ----------------------------------------------------------------
// ProductCategory — product.category
// ----------------------------------------------------------------
class ProductCategory : public BaseModel<ProductCategory> {
public:
    static constexpr const char* MODEL_NAME = "product.category";
    static constexpr const char* TABLE_NAME = "product_category";

    explicit ProductCategory(std::shared_ptr<DbConnection> db)
        : BaseModel(std::move(db))
    {}

    std::string name;
    int         parentId = 0;
    bool        active   = true;
    // Costing GL config (P/064b): valuation accounts + stock journal. NULL here
    // falls back to seeded defaults (1400 / STJ / 1410 / 5000) in the posting.
    int         stockValuationAccountId = 0;
    int         stockJournalId          = 0;
    int         stockAccountInputId     = 0;
    int         stockAccountOutputId    = 0;

    void registerFields() {
        fieldRegistry_.add({"name",      FieldType::Char,    "Name",    true});
        fieldRegistry_.add({"parent_id", FieldType::Many2one,"Parent Category",
                            false, false, true, true, "product.category"});
        fieldRegistry_.add({"active",    FieldType::Boolean, "Active"});
        fieldRegistry_.add({"property_stock_valuation_account_id", FieldType::Many2one, "Stock Valuation Account",   false, false, true, false, "account.account"});
        fieldRegistry_.add({"property_stock_journal_id",           FieldType::Many2one, "Stock Journal",             false, false, true, false, "account.journal"});
        fieldRegistry_.add({"property_stock_account_input_id",     FieldType::Many2one, "Stock Input Account",       false, false, true, false, "account.account"});
        fieldRegistry_.add({"property_stock_account_output_id",    FieldType::Many2one, "Stock Output (COGS) Account", false, false, true, false, "account.account"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]   = name;
        j["active"] = active;
        j["parent_id"] = parentId > 0
            ? nlohmann::json::array({parentId, ""})
            : nlohmann::json(false);
        j["property_stock_valuation_account_id"] = stockValuationAccountId > 0 ? nlohmann::json(stockValuationAccountId) : nlohmann::json(false);
        j["property_stock_journal_id"]           = stockJournalId          > 0 ? nlohmann::json(stockJournalId)          : nlohmann::json(false);
        j["property_stock_account_input_id"]     = stockAccountInputId     > 0 ? nlohmann::json(stockAccountInputId)     : nlohmann::json(false);
        j["property_stock_account_output_id"]    = stockAccountOutputId    > 0 ? nlohmann::json(stockAccountOutputId)    : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        auto m2o = [](const nlohmann::json& v) -> int {
            if (v.is_number_integer()) return v.get<int>();
            if (v.is_array() && v.size() >= 1 && v[0].is_number_integer()) return v[0].get<int>();
            return 0;
        };
        if (j.contains("name")   && j["name"].is_string())   name   = j["name"].get<std::string>();
        if (j.contains("active") && j["active"].is_boolean()) active = j["active"].get<bool>();
        if (j.contains("parent_id")) parentId = m2o(j["parent_id"]);
        if (j.contains("property_stock_valuation_account_id")) stockValuationAccountId = m2o(j["property_stock_valuation_account_id"]);
        if (j.contains("property_stock_journal_id"))           stockJournalId          = m2o(j["property_stock_journal_id"]);
        if (j.contains("property_stock_account_input_id"))     stockAccountInputId     = m2o(j["property_stock_account_input_id"]);
        if (j.contains("property_stock_account_output_id"))    stockAccountOutputId    = m2o(j["property_stock_account_output_id"]);
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


// ----------------------------------------------------------------
// ProductProduct — product.product (single-model, no variant split)
// ----------------------------------------------------------------
class ProductProduct : public BaseModel<ProductProduct> {
public:
    static constexpr const char* MODEL_NAME = "product.product";
    static constexpr const char* TABLE_NAME = "product_product";

    explicit ProductProduct(std::shared_ptr<DbConnection> db)
        : BaseModel(std::move(db))
    {}

    std::string name, defaultCode, barcode, description, type;
    std::string descriptionSale, descriptionPurchase;
    // Sales tab
    std::string invoicePolicy    = "order";   // 'order' | 'delivery'
    std::string saleLineWarn     = "no-message"; // 'no-message' | 'warning' | 'block'
    std::string saleLineWarnMsg;
    // Purchase tab
    std::string purchaseMethod   = "purchase"; // 'purchase' | 'receive'
    std::string purchaseLineWarn = "no-message";
    std::string purchaseLineWarnMsg;
    double      purchaseLeadTime = 0.0;
    int         categId         = 0;
    int         footprintId     = 0;
    int         uomId           = 1;
    int         uomPoId         = 1;
    int         companyId       = 0;
    int         incomeAccountId = 0;
    int         expenseAccountId= 0;
    double      listPrice     = 0.0;
    double      standardPrice = 0.0;
    double      qtyAvailable  = 0.0;   // on-hand, denormalised from stock_quant
    std::string costMethod    = "standard";  // standard | average | fifo
    double      quantitySvl   = 0.0;   // valued quantity (valuation cache)
    double      valueSvl      = 0.0;   // inventory value (valuation cache)
    std::string tracking      = "none";      // none | lot | serial
    double      volume        = 0.0;
    double      weight        = 0.0;
    bool        saleOk     = true;
    bool        purchaseOk = true;
    bool        expenseOk  = false;
    bool        active     = true;
    std::string image1920;

    void registerFields() {
        fieldRegistry_.add({"name",           FieldType::Char,    "Product Name", true});
        fieldRegistry_.add({"default_code",   FieldType::Char,    "Internal Reference"});
        fieldRegistry_.add({"barcode",        FieldType::Char,    "Barcode"});
        fieldRegistry_.add({"description",    FieldType::Text,    "Description"});
        fieldRegistry_.add({"type",           FieldType::Char,    "Product Type"});
        fieldRegistry_.add({"categ_id",       FieldType::Many2one,"Category",
                            false, false, true, true, "product.category"});
        fieldRegistry_.add({"footprint_id",   FieldType::Many2one,"Footprint",
                            false, false, true, true, "part.footprint"});
        fieldRegistry_.add({"uom_id",         FieldType::Many2one,"Unit of Measure",
                            true, false, true, true, "uom.uom"});
        fieldRegistry_.add({"uom_po_id",      FieldType::Many2one,"Purchase UoM",
                            true, false, true, true, "uom.uom"});
        fieldRegistry_.add({"list_price",     FieldType::Monetary,"Sales Price"});
        fieldRegistry_.add({"standard_price", FieldType::Monetary,"Cost"});
        fieldRegistry_.add({"volume",         FieldType::Float,   "Volume"});
        fieldRegistry_.add({"weight",         FieldType::Float,   "Weight"});
        fieldRegistry_.add({"sale_ok",        FieldType::Boolean, "Can be Sold"});
        fieldRegistry_.add({"purchase_ok",    FieldType::Boolean, "Can be Purchased"});
        fieldRegistry_.add({"company_id",     FieldType::Many2one,"Company",
                            false, false, true, true, "res.company"});
        fieldRegistry_.add({"expense_ok",          FieldType::Boolean, "Can be Expensed"});
        fieldRegistry_.add({"image_1920",          FieldType::Text,    "Image"});
        fieldRegistry_.add({"active",              FieldType::Boolean, "Active"});
        fieldRegistry_.add({"description_sale",     FieldType::Text,    "Sales Description"});
        fieldRegistry_.add({"description_purchase", FieldType::Text,    "Purchase Description"});
        fieldRegistry_.add({"income_account_id",    FieldType::Many2one,"Income Account",
                            false, false, true, true, "account.account"});
        fieldRegistry_.add({"expense_account_id",   FieldType::Many2one,"Expense Account",
                            false, false, true, true, "account.account"});
        // Sales tab
        fieldRegistry_.add({"invoice_policy",     FieldType::Char, "Invoicing Policy"});
        fieldRegistry_.add({"sale_line_warn",     FieldType::Char, "Sales Warning"});
        fieldRegistry_.add({"sale_line_warn_msg", FieldType::Text, "Sales Warning Message"});
        // Purchase tab
        fieldRegistry_.add({"purchase_method",       FieldType::Char,  "Control Policy"});
        fieldRegistry_.add({"purchase_lead_time",    FieldType::Float, "Purchase Lead Time"});
        fieldRegistry_.add({"purchase_line_warn",    FieldType::Char,  "Purchase Warning"});
        fieldRegistry_.add({"purchase_line_warn_msg",FieldType::Text,  "Purchase Warning Message"});
        // P2: BIGINT micro-units (migration 950). purchase_lead_time, weight and
        // volume stay NUMERIC — they are not money and were not migrated.
        fieldRegistry_.setPrecision(core::DecimalPrecision::kProductPrice,
                                    {"list_price", "standard_price"});
        fieldRegistry_.markScaled({"list_price", "standard_price"});
        // On-hand quantity, kept fresh by the quant engine (core/StockQuant).
        // Read-only/computed — the client never writes it.
        fieldRegistry_.add({"qty_available", FieldType::Float, "On Hand", false, true, true, true});
        fieldRegistry_.markScaled({"qty_available"});
        // Costing (P/064b): method + cached valued quantity/value, maintained by
        // the valuation engine (core/StockQuant). value_svl is read-only.
        fieldRegistry_.add({"cost_method",  FieldType::Char,     "Costing Method"});
        fieldRegistry_.add({"quantity_svl", FieldType::Float,    "Valued Quantity", false, true, true, true});
        fieldRegistry_.add({"value_svl",    FieldType::Monetary, "Inventory Value", false, true, true, true});
        fieldRegistry_.markScaled({"quantity_svl", "value_svl"});
        // Traceability: none / lot / serial.
        fieldRegistry_.add({"tracking", FieldType::Char, "Tracking"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]           = name;
        j["default_code"]   = defaultCode;
        j["barcode"]        = barcode.empty()       ? nlohmann::json(false) : nlohmann::json(barcode);
        j["description"]    = description.empty()   ? nlohmann::json(false) : nlohmann::json(description);
        j["type"]           = type.empty() ? "consu" : type;
        j["categ_id"]       = categId  > 0 ? nlohmann::json::array({categId,  ""}) : nlohmann::json(false);
        j["footprint_id"]   = footprintId > 0 ? nlohmann::json::array({footprintId, ""}) : nlohmann::json(false);
        j["uom_id"]         = uomId    > 0 ? nlohmann::json::array({uomId,    ""}) : nlohmann::json(false);
        j["uom_po_id"]      = uomPoId  > 0 ? nlohmann::json::array({uomPoId,  ""}) : nlohmann::json(false);
        j["company_id"]     = companyId> 0 ? nlohmann::json::array({companyId,""}) : nlohmann::json(false);
        j["list_price"]     = listPrice;
        j["standard_price"] = standardPrice;
        j["qty_available"]  = qtyAvailable;
        j["cost_method"]    = costMethod.empty() ? "standard" : costMethod;
        j["quantity_svl"]   = quantitySvl;
        j["value_svl"]      = valueSvl;
        j["tracking"]       = tracking.empty() ? "none" : tracking;
        j["volume"]         = volume;
        j["weight"]         = weight;
        j["sale_ok"]        = saleOk;
        j["purchase_ok"]    = purchaseOk;
        j["expense_ok"]           = expenseOk;
        j["image_1920"]           = image1920.empty() ? nlohmann::json(false) : nlohmann::json(image1920);
        j["active"]               = active;
        j["description_sale"]     = descriptionSale.empty()     ? nlohmann::json(false) : nlohmann::json(descriptionSale);
        j["description_purchase"] = descriptionPurchase.empty() ? nlohmann::json(false) : nlohmann::json(descriptionPurchase);
        j["income_account_id"]    = incomeAccountId  > 0 ? nlohmann::json::array({incomeAccountId,  ""}) : nlohmann::json(false);
        j["expense_account_id"]   = expenseAccountId > 0 ? nlohmann::json::array({expenseAccountId, ""}) : nlohmann::json(false);
        j["invoice_policy"]       = invoicePolicy.empty()    ? "order"      : invoicePolicy;
        j["sale_line_warn"]       = saleLineWarn.empty()     ? "no-message" : saleLineWarn;
        j["sale_line_warn_msg"]   = saleLineWarnMsg.empty()  ? nlohmann::json(false) : nlohmann::json(saleLineWarnMsg);
        j["purchase_method"]      = purchaseMethod.empty()   ? "purchase"   : purchaseMethod;
        j["purchase_lead_time"]   = purchaseLeadTime;
        j["purchase_line_warn"]   = purchaseLineWarn.empty() ? "no-message" : purchaseLineWarn;
        j["purchase_line_warn_msg"] = purchaseLineWarnMsg.empty() ? nlohmann::json(false) : nlohmann::json(purchaseLineWarnMsg);
    }

    void deserializeFields(const nlohmann::json& j) override {
        auto m2o = [](const nlohmann::json& v) -> int {
            if (v.is_number_integer()) return v.get<int>();
            if (v.is_array() && v.size() >= 1 && v[0].is_number_integer()) return v[0].get<int>();
            return 0;
        };
        if (j.contains("name")           && j["name"].is_string())     name           = j["name"].get<std::string>();
        if (j.contains("default_code")   && j["default_code"].is_string()) defaultCode = j["default_code"].get<std::string>();
        if (j.contains("barcode")        && j["barcode"].is_string())   barcode        = j["barcode"].get<std::string>();
        if (j.contains("description")    && j["description"].is_string()) description  = j["description"].get<std::string>();
        if (j.contains("type")           && j["type"].is_string())      type           = j["type"].get<std::string>();
        if (j.contains("categ_id"))     categId     = m2o(j["categ_id"]);
        if (j.contains("footprint_id")) footprintId = m2o(j["footprint_id"]);
        if (j.contains("uom_id"))       uomId       = m2o(j["uom_id"]);
        if (j.contains("uom_po_id"))  uomPoId   = m2o(j["uom_po_id"]);
        if (j.contains("company_id")) companyId = m2o(j["company_id"]);
        if (j.contains("list_price")     && j["list_price"].is_number())     listPrice     = j["list_price"].get<double>();
        if (j.contains("standard_price") && j["standard_price"].is_number()) standardPrice = j["standard_price"].get<double>();
        if (j.contains("qty_available")  && j["qty_available"].is_number())  qtyAvailable  = j["qty_available"].get<double>();
        if (j.contains("cost_method")    && j["cost_method"].is_string())    costMethod    = j["cost_method"].get<std::string>();
        if (j.contains("quantity_svl")   && j["quantity_svl"].is_number())   quantitySvl   = j["quantity_svl"].get<double>();
        if (j.contains("value_svl")      && j["value_svl"].is_number())      valueSvl      = j["value_svl"].get<double>();
        if (j.contains("tracking")       && j["tracking"].is_string())       tracking      = j["tracking"].get<std::string>();
        if (j.contains("volume")         && j["volume"].is_number())         volume        = j["volume"].get<double>();
        if (j.contains("weight")         && j["weight"].is_number())         weight        = j["weight"].get<double>();
        if (j.contains("sale_ok")     && j["sale_ok"].is_boolean())     saleOk     = j["sale_ok"].get<bool>();
        if (j.contains("purchase_ok") && j["purchase_ok"].is_boolean()) purchaseOk = j["purchase_ok"].get<bool>();
        if (j.contains("expense_ok")  && j["expense_ok"].is_boolean())  expenseOk  = j["expense_ok"].get<bool>();
        if (j.contains("image_1920")  && j["image_1920"].is_string())   image1920  = j["image_1920"].get<std::string>();
        if (j.contains("active")      && j["active"].is_boolean())      active     = j["active"].get<bool>();
        if (j.contains("description_sale")     && j["description_sale"].is_string())
            descriptionSale     = j["description_sale"].get<std::string>();
        if (j.contains("description_purchase") && j["description_purchase"].is_string())
            descriptionPurchase = j["description_purchase"].get<std::string>();
        if (j.contains("income_account_id"))  incomeAccountId  = m2o(j["income_account_id"]);
        if (j.contains("expense_account_id")) expenseAccountId = m2o(j["expense_account_id"]);
        if (j.contains("invoice_policy")       && j["invoice_policy"].is_string())      invoicePolicy    = j["invoice_policy"].get<std::string>();
        if (j.contains("sale_line_warn")       && j["sale_line_warn"].is_string())      saleLineWarn     = j["sale_line_warn"].get<std::string>();
        if (j.contains("sale_line_warn_msg")   && j["sale_line_warn_msg"].is_string())  saleLineWarnMsg  = j["sale_line_warn_msg"].get<std::string>();
        if (j.contains("purchase_method")      && j["purchase_method"].is_string())     purchaseMethod   = j["purchase_method"].get<std::string>();
        if (j.contains("purchase_lead_time")   && j["purchase_lead_time"].is_number())  purchaseLeadTime = j["purchase_lead_time"].get<double>();
        if (j.contains("purchase_line_warn")   && j["purchase_line_warn"].is_string())  purchaseLineWarn = j["purchase_line_warn"].get<std::string>();
        if (j.contains("purchase_line_warn_msg")&& j["purchase_line_warn_msg"].is_string()) purchaseLineWarnMsg = j["purchase_line_warn_msg"].get<std::string>();
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

class ProductCategoryListView : public core::BaseView {
public:
    std::string viewName()  const override { return "product.category.list"; }
    std::string modelName() const override { return "product.category"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Product Categories\">"
               "<field name=\"name\"/>"
               "<field name=\"parent_id\"/>"
               "<field name=\"active\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",      {{"type","char"},    {"string","Name"}}},
            {"parent_id", {{"type","many2one"},{"string","Parent Category"},{"relation","product.category"}}},
            {"active",    {{"type","boolean"}, {"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class ProductCategoryFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "product.category.form"; }
    std::string modelName() const override { return "product.category"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Product Category\">"
               "<field name=\"name\"/>"
               "<field name=\"parent_id\"/>"
               "<field name=\"active\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",      {{"type","char"},    {"string","Name"}}},
            {"parent_id", {{"type","many2one"},{"string","Parent Category"},{"relation","product.category"}}},
            {"active",    {{"type","boolean"}, {"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class ProductProductListView : public core::BaseView {
public:
    std::string viewName()  const override { return "product.product.list"; }
    std::string modelName() const override { return "product.product"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Products\">"
               "<field name=\"default_code\"/>"
               "<field name=\"name\"/>"
               "<field name=\"type\"/>"
               "<field name=\"categ_id\"/>"
               "<field name=\"list_price\"/>"
               "<field name=\"standard_price\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"default_code",   {{"type","char"},    {"string","Internal Reference"}}},
            {"name",           {{"type","char"},    {"string","Product Name"}}},
            {"type",           {{"type","char"},    {"string","Type"}}},
            {"categ_id",       {{"type","many2one"},{"string","Category"},{"relation","product.category"}}},
            {"list_price",     {{"type","monetary"},{"string","Sales Price"}}},
            {"standard_price", {{"type","monetary"},{"string","Cost"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class ProductProductFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "product.product.form"; }
    std::string modelName() const override { return "product.product"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Product\">"
               "<field name=\"name\"/>"
               "<field name=\"default_code\"/>"
               "<field name=\"barcode\"/>"
               "<field name=\"type\"/>"
               "<field name=\"categ_id\"/>"
               "<field name=\"uom_id\"/>"
               "<field name=\"uom_po_id\"/>"
               "<field name=\"list_price\"/>"
               "<field name=\"standard_price\"/>"
               "<field name=\"volume\"/>"
               "<field name=\"weight\"/>"
               "<field name=\"sale_ok\"/>"
               "<field name=\"purchase_ok\"/>"
               "<field name=\"active\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",           {{"type","char"},    {"string","Product Name"}}},
            {"default_code",   {{"type","char"},    {"string","Internal Reference"}}},
            {"barcode",        {{"type","char"},    {"string","Barcode"}}},
            {"type",           {{"type","char"},    {"string","Product Type"}}},
            {"categ_id",       {{"type","many2one"},{"string","Category"},{"relation","product.category"}}},
            {"uom_id",         {{"type","many2one"},{"string","Unit of Measure"},{"relation","uom.uom"}}},
            {"uom_po_id",      {{"type","many2one"},{"string","Purchase UoM"},{"relation","uom.uom"}}},
            {"list_price",     {{"type","monetary"},{"string","Sales Price"}}},
            {"standard_price", {{"type","monetary"},{"string","Cost"}}},
            {"volume",         {{"type","float"},   {"string","Volume"}}},
            {"weight",         {{"type","float"},   {"string","Weight"}}},
            {"sale_ok",        {{"type","boolean"}, {"string","Can be Sold"}}},
            {"purchase_ok",    {{"type","boolean"}, {"string","Can be Purchased"}}},
            {"expense_ok",     {{"type","boolean"}, {"string","Can be Expensed"}}},
            {"image_1920",     {{"type","char"},    {"string","Image"}}},
            {"active",         {{"type","boolean"}, {"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};


// ================================================================
// 3. PRODUCT CATEGORY VIEW MODEL
//    Enriches search_read / read with parent name, child_count,
//    and product_count — all computed via SQL JOINs/subqueries.
// ================================================================

class ProductCategoryViewModel : public core::BaseViewModel {
public:
    explicit ProductCategoryViewModel(std::shared_ptr<DbConnection> db)
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
        REGISTER_METHOD("name_search",     handleNameSearch)
        REGISTER_METHOD("search",          handleSearch)
    }

    std::string modelName() const override { return "product.category"; }

    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        // Domain filter — support [["active","=",true]] and empty
        std::string whereClause = "1=1";
        const auto& domain = call.domain();
        if (domain.is_array()) {
            for (const auto& leaf : domain) {
                if (!leaf.is_array() || leaf.size() < 3) continue;
                std::string field = leaf[0].get<std::string>();
                std::string op    = leaf[1].get<std::string>();
                if (field == "active" && op == "=" && leaf[2].is_boolean()) {
                    whereClause += leaf[2].get<bool>()
                        ? " AND pc.active = TRUE"
                        : " AND pc.active = FALSE";
                } else if (field == "parent_id" && op == "=") {
                    if (leaf[2].is_null() || leaf[2] == false) {
                        whereClause += " AND pc.parent_id IS NULL";
                    } else if (leaf[2].is_number_integer()) {
                        whereClause += " AND pc.parent_id = " + std::to_string(leaf[2].get<int>());
                    }
                }
            }
        }

        int limit  = call.limit()  > 0 ? call.limit()  : 500;
        int offset = call.offset();

        auto rows = txn.exec(
            "SELECT pc.id, pc.name, pc.active, pc.parent_id, "
            "COALESCE(par.name,'') AS parent_name, "
            "(SELECT COUNT(*) FROM product_category c2 WHERE c2.parent_id = pc.id) AS child_count, "
            "(SELECT COUNT(*) FROM product_product pp WHERE pp.categ_id = pc.id) AS product_count "
            "FROM product_category pc "
            "LEFT JOIN product_category par ON par.id = pc.parent_id "
            "WHERE " + whereClause + " "
            "ORDER BY pc.name "
            "LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset)
        );

        nlohmann::json result = nlohmann::json::array();
        for (const auto& r : rows) result.push_back(serializeRow_(r));
        return result;
    }

    nlohmann::json handleRead(const core::CallKwArgs& call) {
        auto ids = call.ids();
        if (ids.empty()) return nlohmann::json::array();

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        std::string inList;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i) inList += ",";
            inList += std::to_string(ids[i]);
        }

        auto rows = txn.exec(
            "SELECT pc.id, pc.name, pc.active, pc.parent_id, "
            "COALESCE(par.name,'') AS parent_name, "
            "(SELECT COUNT(*) FROM product_category c2 WHERE c2.parent_id = pc.id) AS child_count, "
            "(SELECT COUNT(*) FROM product_product pp WHERE pp.categ_id = pc.id) AS product_count "
            "FROM product_category pc "
            "LEFT JOIN product_category par ON par.id = pc.parent_id "
            "WHERE pc.id IN (" + inList + ") ORDER BY pc.name"
        );

        nlohmann::json result = nlohmann::json::array();
        for (const auto& r : rows) result.push_back(serializeRow_(r));
        return result;
    }

    nlohmann::json handleCreate(const core::CallKwArgs& call) {
        const auto vals = call.arg(0);
        if (!vals.is_object()) throw std::runtime_error("create: args[0] must be a dict");
        std::string name   = vals.value("name", std::string(""));
        bool        active = vals.value("active", true);
        int parentId = 0;
        if (vals.contains("parent_id")) {
            const auto& v = vals["parent_id"];
            if (v.is_number_integer())          parentId = v.get<int>();
            else if (v.is_array() && v.size() >= 1 && v[0].is_number_integer())
                                                parentId = v[0].get<int>();
        }
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        pqxx::result r;
        if (parentId > 0) {
            r = txn.exec(
                "INSERT INTO product_category (name, parent_id, active, write_date) "
                "VALUES ($1, $2, $3, now()) RETURNING id",
                pqxx::params{name, parentId, active});
        } else {
            r = txn.exec(
                "INSERT INTO product_category (name, parent_id, active, write_date) "
                "VALUES ($1, NULL, $2, now()) RETURNING id",
                pqxx::params{name, active});
        }
        txn.commit();
        int newId = r[0]["id"].as<int>();
        return nlohmann::json(newId);
    }

    // C-3: a category may not become its own ancestor. Without this a cycle can
    // be created from the UI, and the frontend's categoryDescendantIds() walk
    // (which has no visited set) then recurses until the tab dies — for every
    // user, repairable only by a direct DB edit.
    static bool wouldCreateCycle_(pqxx::work& txn, int id, int newParentId) {
        if (newParentId <= 0) return false;
        if (newParentId == id) return true;
        auto rows = txn.exec(
            "WITH RECURSIVE anc AS ("
            "  SELECT id, parent_id FROM product_category WHERE id = $1"
            "  UNION ALL"
            "  SELECT c.id, c.parent_id FROM product_category c"
            "    JOIN anc ON c.id = anc.parent_id"
            ") SELECT 1 FROM anc WHERE id = $2 LIMIT 1",
            pqxx::params{newParentId, id});
        return !rows.empty();
    }

    nlohmann::json handleWrite(const core::CallKwArgs& call) {
        auto ids = call.ids();
        if (ids.empty()) return nlohmann::json(true);
        const auto vals = call.arg(1);
        if (!vals.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : ids) {
            if (vals.contains("name") && vals["name"].is_string())
                txn.exec("UPDATE product_category SET name=$1, write_date=now() WHERE id=$2",
                         pqxx::params{vals["name"].get<std::string>(), id});
            if (vals.contains("active") && vals["active"].is_boolean())
                txn.exec("UPDATE product_category SET active=$1, write_date=now() WHERE id=$2",
                         pqxx::params{vals["active"].get<bool>(), id});
            if (vals.contains("parent_id")) {
                const auto& v = vals["parent_id"];
                int parentId = 0;
                if (v.is_number_integer()) parentId = v.get<int>();
                else if (v.is_array() && v.size() >= 1 && v[0].is_number_integer())
                    parentId = v[0].get<int>();
                if (parentId > 0) {
                    if (wouldCreateCycle_(txn, id, parentId))   // C-3
                        throw infrastructure::ValidationError(
                            "Cannot set parent: that category is a descendant of this one.");
                    txn.exec("UPDATE product_category SET parent_id=$1, write_date=now() WHERE id=$2",
                             pqxx::params{parentId, id});
                } else {
                    txn.exec("UPDATE product_category SET parent_id=NULL, write_date=now() WHERE id=$1",
                             pqxx::params{id});
                }
            }
        }
        txn.commit();
        return nlohmann::json(true);
    }

    nlohmann::json handleUnlink(const core::CallKwArgs& call) {
        auto ids = call.ids();
        if (ids.empty()) return nlohmann::json(true);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : ids) {
            // C-4: both FKs are ON DELETE SET NULL, so an unguarded delete
            // silently promotes every child category to root level and strips
            // the category from every product under it — damage that does not
            // look like it came from the delete. Refuse, as Odoo does.
            auto childRows = txn.exec(
                "SELECT COUNT(*) FROM product_category WHERE parent_id = $1",
                pqxx::params{id});
            const int children = childRows[0][0].as<int>();
            auto prodRows = txn.exec(
                "SELECT COUNT(*) FROM product_product WHERE categ_id = $1",
                pqxx::params{id});
            const int products = prodRows[0][0].as<int>();
            if (children > 0 || products > 0)
                throw infrastructure::ValidationError(
                    "Cannot delete this category: it has " +
                    std::to_string(children) + " subcategor" +
                    (children == 1 ? "y" : "ies") + " and " +
                    std::to_string(products) + " product" +
                    (products == 1 ? "" : "s") +
                    ". Reassign or delete them first.");
            txn.exec("DELETE FROM product_category WHERE id=$1", pqxx::params{id});
        }
        txn.commit();
        return nlohmann::json(true);
    }

    nlohmann::json handleFieldsGet(const core::CallKwArgs&) {
        return {
            {"id",            {{"type","integer"}, {"string","ID"}}},
            {"name",          {{"type","char"},    {"string","Name"}}},
            {"parent_id",     {{"type","many2one"},{"string","Parent Category"},{"relation","product.category"}}},
            {"active",        {{"type","boolean"}, {"string","Active"}}},
            {"child_count",   {{"type","integer"}, {"string","Subcategories"}}},
            {"product_count", {{"type","integer"}, {"string","Products"}}},
        };
    }

    // name_search — supports Many2one autocomplete
    nlohmann::json handleNameSearch(const core::CallKwArgs& call) {
        // name_search: args[0] = name string, kwargs["limit"]
        std::string name;
        if (call.args.is_array() && !call.args.empty() && call.args[0].is_string())
            name = call.args[0].get<std::string>();
        int lim = call.limit() > 0 ? call.limit() : 20;
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto rows = txn.exec(
            "SELECT id, name FROM product_category "
            "WHERE name ILIKE $1 AND active = TRUE ORDER BY name LIMIT $2",
            pqxx::params{"%" + name + "%", lim});
        nlohmann::json result = nlohmann::json::array();
        for (const auto& r : rows)
            result.push_back(nlohmann::json::array({r["id"].as<int>(), r["name"].as<std::string>()}));
        return result;
    }

    nlohmann::json handleSearch(const core::CallKwArgs& call) {
        // Returns array of ids matching domain
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        std::string whereClause = "active = TRUE";
        const auto& domain = call.domain();
        if (domain.is_array()) {
            for (const auto& leaf : domain) {
                if (!leaf.is_array() || leaf.size() < 3) continue;
                std::string field = leaf[0].get<std::string>();
                std::string op    = leaf[1].get<std::string>();
                if (field == "parent_id" && op == "=" && leaf[2].is_number_integer())
                    whereClause += " AND parent_id = " + std::to_string(leaf[2].get<int>());
            }
        }
        int lim = call.limit() > 0 ? call.limit() : 500;
        auto rows = txn.exec(
            "SELECT id FROM product_category WHERE " + whereClause +
            " ORDER BY name LIMIT " + std::to_string(lim));
        nlohmann::json result = nlohmann::json::array();
        for (const auto& r : rows) result.push_back(r["id"].as<int>());
        return result;
    }

private:
    std::shared_ptr<DbConnection> db_;

    static nlohmann::json serializeRow_(const pqxx::row& r) {
        int parentId = r["parent_id"].is_null() ? 0 : r["parent_id"].as<int>();
        std::string parentName = r["parent_name"].as<std::string>("");
        return {
            {"id",            r["id"].as<int>()},
            {"name",          r["name"].as<std::string>()},
            {"display_name",  r["name"].as<std::string>()},
            {"active",        r["active"].as<bool>(true)},
            {"parent_id",     parentId > 0
                                ? nlohmann::json::array({parentId, parentName})
                                : nlohmann::json(false)},
            {"child_count",   r["child_count"].as<int>(0)},
            {"product_count", r["product_count"].as<int>(0)},
        };
    }
};


// ----------------------------------------------------------------
// ProductSupplierInfo — product.supplierinfo (a vendor pricelist line)
// ----------------------------------------------------------------
class ProductSupplierInfo : public BaseModel<ProductSupplierInfo> {
public:
    static constexpr const char* MODEL_NAME = "product.supplierinfo";
    static constexpr const char* TABLE_NAME = "product_supplierinfo";

    int         productId = 0;
    int         partnerId = 0;   // the vendor
    std::string productName;     // the vendor's name for this product
    std::string productCode;     // the vendor's reference
    double      minQty  = 0.0;
    double      price   = 0.0;
    int         delay   = 1;      // delivery lead time, days
    int         sequence = 10;
    int         companyId = 0;

    explicit ProductSupplierInfo(std::shared_ptr<DbConnection> db)
        : BaseModel(std::move(db)) {}

    static int m2o(const nlohmann::json& v) {
        if (v.is_number_integer()) return v.get<int>();
        if (v.is_array() && v.size() >= 1 && v[0].is_number_integer()) return v[0].get<int>();
        return 0;
    }

    void registerFields() {
        fieldRegistry_.add({"product_id",   FieldType::Many2one,"Product",             false, false, true, true, "product.product"});
        fieldRegistry_.add({"partner_id",   FieldType::Many2one,"Vendor",              true,  false, true, true, "res.partner"});
        fieldRegistry_.add({"product_name", FieldType::Char,    "Vendor Product Name"});
        fieldRegistry_.add({"product_code", FieldType::Char,    "Vendor Product Code"});
        fieldRegistry_.add({"min_qty",      FieldType::Float,   "Min Quantity"});
        fieldRegistry_.add({"price",        FieldType::Monetary,"Price"});
        fieldRegistry_.add({"delay",        FieldType::Integer, "Delivery Lead Time"});
        fieldRegistry_.add({"sequence",     FieldType::Integer, "Sequence"});
        fieldRegistry_.add({"company_id",   FieldType::Many2one,"Company",             false, false, true, true, "res.company"});
        fieldRegistry_.markScaled({"min_qty", "price"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["product_id"]   = productId > 0 ? nlohmann::json(productId) : nlohmann::json(false);
        j["partner_id"]   = partnerId > 0 ? nlohmann::json(partnerId) : nlohmann::json(false);
        j["product_name"] = productName.empty() ? nlohmann::json(false) : nlohmann::json(productName);
        j["product_code"] = productCode.empty() ? nlohmann::json(false) : nlohmann::json(productCode);
        j["min_qty"]      = minQty;
        j["price"]        = price;
        j["delay"]        = delay;
        j["sequence"]     = sequence;
        j["company_id"]   = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("product_name") && j["product_name"].is_string()) productName = j["product_name"].get<std::string>();
        if (j.contains("product_code") && j["product_code"].is_string()) productCode = j["product_code"].get<std::string>();
        if (j.contains("min_qty")  && j["min_qty"].is_number())  minQty   = j["min_qty"].get<double>();
        if (j.contains("price")    && j["price"].is_number())    price    = j["price"].get<double>();
        if (j.contains("delay")    && j["delay"].is_number())    delay    = j["delay"].get<int>();
        if (j.contains("sequence") && j["sequence"].is_number()) sequence = j["sequence"].get<int>();
        if (j.contains("product_id")) productId = m2o(j["product_id"]);
        if (j.contains("partner_id")) partnerId = m2o(j["partner_id"]);
        if (j.contains("company_id")) companyId = m2o(j["company_id"]);
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (partnerId <= 0) e.push_back("Vendor is required");
        return e;
    }
};

// ----------------------------------------------------------------
// ProductSupplierInfoViewModel — vendor lines, filtered by product_id
// ----------------------------------------------------------------
class ProductSupplierInfoViewModel : public core::BaseViewModel {
public:
    explicit ProductSupplierInfoViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_MUTATOR("create",          handleCreate)
        REGISTER_MUTATOR("write",           handleWrite)
        REGISTER_MUTATOR("unlink",          handleUnlink)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
    }
    std::string modelName() const override { return "product.supplierinfo"; }
private:
    std::shared_ptr<DbConnection> db_;

    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        const int lim = call.limit() > 0 ? call.limit() : 80;
        const int off = call.offset();
        int prodFilter = 0, partnerFilter = 0;
        { auto d = call.domain(); if (d.is_array()) for (const auto& c : d)
            if (c.is_array() && c.size() == 3 && c[0].is_string() && c[2].is_number_integer()) {
                if (c[0].get<std::string>() == "product_id") prodFilter    = c[2].get<int>();
                if (c[0].get<std::string>() == "partner_id") partnerFilter = c[2].get<int>();
            } }
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT s.id, s.product_id, pp.name AS product_name_,
                   s.partner_id, rp.name AS partner_name,
                   s.product_name, s.product_code, s.min_qty, s.price, s.delay, s.sequence
            FROM product_supplierinfo s
            LEFT JOIN product_product pp ON pp.id = s.product_id
            LEFT JOIN res_partner     rp ON rp.id = s.partner_id
        )";
        pqxx::params p; int n = 0;
        sql += " WHERE TRUE";
        if (prodFilter > 0)    { sql += " AND s.product_id=$" + std::to_string(++n); p.append(prodFilter); }
        if (partnerFilter > 0) { sql += " AND s.partner_id=$" + std::to_string(++n); p.append(partnerFilter); }
        // S-30: enforce ir.rule on this custom read (record-rule bypass fix, 071 §1.2).
        core::appendRecordRuleSubquery(sql, p, "product.supplierinfo", core::RuleOp::Read,
                                       extractContext_(call), "product_supplierinfo", "s.id", n);
        sql += " ORDER BY s.sequence, s.id";
        sql += " LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);
        auto res = txn.exec(sql, p);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]           = row["id"].as<int>();
            j["product_id"]   = row["product_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["product_id"].as<int>(), row["product_name_"].is_null()?"":std::string(row["product_name_"].c_str())});
            j["partner_id"]   = row["partner_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["partner_id"].as<int>(), row["partner_name"].is_null()?"":std::string(row["partner_name"].c_str())});
            j["product_name"] = row["product_name"].is_null() ? nlohmann::json(false) : nlohmann::json(row["product_name"].c_str());
            j["product_code"] = row["product_code"].is_null() ? nlohmann::json(false) : nlohmann::json(row["product_code"].c_str());
            j["min_qty"]      = core::Money::fromMicros(row["min_qty"].as<long long>(0)).toJson();
            j["price"]        = core::Money::fromMicros(row["price"].as<long long>(0)).toJson();
            j["delay"]        = row["delay"].as<int>(1);
            j["sequence"]     = row["sequence"].as<int>(10);
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const core::CallKwArgs& call)        { ProductSupplierInfo p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids(), call.fields()); }
    nlohmann::json handleCreate(const core::CallKwArgs& call)      { ProductSupplierInfo p(db_); p.setUserContext(extractContext_(call)); return p.create(call.arg(0)); }
    nlohmann::json handleWrite(const core::CallKwArgs& call)       { ProductSupplierInfo p(db_); p.setUserContext(extractContext_(call)); return p.write(call.ids(), call.arg(1)); }
    nlohmann::json handleUnlink(const core::CallKwArgs& call)      { ProductSupplierInfo p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids()); }
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call)   { ProductSupplierInfo p(db_); return p.fieldsGet(call.fields()); }
    nlohmann::json handleSearchCount(const core::CallKwArgs& call) { ProductSupplierInfo p(db_); p.setUserContext(extractContext_(call)); return p.searchCount(call.domain()); }
};

class ProductSupplierInfoListView : public core::BaseView {
public:
    std::string viewName()  const override { return "product.supplierinfo.list"; }
    std::string modelName() const override { return "product.supplierinfo"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Vendor Pricelists\">"
               "<field name=\"partner_id\"/>"
               "<field name=\"product_id\"/>"
               "<field name=\"product_code\"/>"
               "<field name=\"min_qty\"/>"
               "<field name=\"price\"/>"
               "<field name=\"delay\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"partner_id",   {{"type","many2one"}, {"string","Vendor"},  {"relation","res.partner"}}},
            {"product_id",   {{"type","many2one"}, {"string","Product"}, {"relation","product.product"}}},
            {"product_code", {{"type","char"},     {"string","Vendor Code"}}},
            {"min_qty",      {{"type","float"},    {"string","Min Qty"}}},
            {"price",        {{"type","monetary"}, {"string","Price"}}},
            {"delay",        {{"type","integer"},  {"string","Lead Time"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ================================================================
// PartKeepr PK2–PK4 — footprints, parameters + SI units, manufacturer parts
// ================================================================
static int partM2o(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && v.size() >= 1 && v[0].is_number_integer()) return v[0].get<int>();
    return 0;
}

// PK2 — part.footprint (a physical package/mounting pattern, e.g. SOIC-8, 0805)
class PartFootprint : public BaseModel<PartFootprint> {
public:
    static constexpr const char* MODEL_NAME = "part.footprint";
    static constexpr const char* TABLE_NAME = "part_footprint";
    std::string name, description;
    explicit PartFootprint(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}
    void registerFields() {
        fieldRegistry_.add({"name",        FieldType::Char, "Name", true});
        fieldRegistry_.add({"description", FieldType::Text, "Description"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name; j["description"] = description.empty() ? nlohmann::json(false) : nlohmann::json(description);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")        && j["name"].is_string())        name        = j["name"].get<std::string>();
        if (j.contains("description") && j["description"].is_string()) description = j["description"].get<std::string>();
    }
    std::vector<std::string> validate() const override { std::vector<std::string> e; if (name.empty()) e.push_back("Name is required"); return e; }
};

// PK3 — part.unit (an SI unit, e.g. Ohm/Ω, Farad/F, Volt/V)
class PartUnit : public BaseModel<PartUnit> {
public:
    static constexpr const char* MODEL_NAME = "part.unit";
    static constexpr const char* TABLE_NAME = "part_unit";
    std::string name, symbol;
    explicit PartUnit(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}
    void registerFields() {
        fieldRegistry_.add({"name",   FieldType::Char, "Unit", true});
        fieldRegistry_.add({"symbol", FieldType::Char, "Symbol"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name; j["symbol"] = symbol.empty() ? nlohmann::json(false) : nlohmann::json(symbol);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")   && j["name"].is_string())   name   = j["name"].get<std::string>();
        if (j.contains("symbol") && j["symbol"].is_string()) symbol = j["symbol"].get<std::string>();
    }
    std::vector<std::string> validate() const override { std::vector<std::string> e; if (name.empty()) e.push_back("Name is required"); return e; }
};

// PK3 — part.parameter (a product's parametric spec: name + numeric value + unit).
// value_numeric stays NUMERIC (scientific range), NOT micro-fixed-point.
class PartParameter : public BaseModel<PartParameter> {
public:
    static constexpr const char* MODEL_NAME = "part.parameter";
    static constexpr const char* TABLE_NAME = "part_parameter";
    int         productId = 0;
    std::string name;
    double      valueNumeric = 0.0;
    int         unitId = 0;
    std::string valueText;
    explicit PartParameter(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}
    void registerFields() {
        fieldRegistry_.add({"product_id",    FieldType::Many2one,"Product", false, false, true, true, "product.product"});
        fieldRegistry_.add({"name",          FieldType::Char,    "Parameter", true});
        fieldRegistry_.add({"value_numeric", FieldType::Float,   "Value"});
        fieldRegistry_.add({"unit_id",       FieldType::Many2one,"Unit", false, false, true, true, "part.unit"});
        fieldRegistry_.add({"value_text",    FieldType::Char,    "Text Value"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["product_id"]    = productId > 0 ? nlohmann::json(productId) : nlohmann::json(false);
        j["name"]          = name;
        j["value_numeric"] = valueNumeric;
        j["unit_id"]       = unitId > 0 ? nlohmann::json(unitId) : nlohmann::json(false);
        j["value_text"]    = valueText.empty() ? nlohmann::json(false) : nlohmann::json(valueText);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")          && j["name"].is_string())          name         = j["name"].get<std::string>();
        if (j.contains("value_numeric") && j["value_numeric"].is_number()) valueNumeric = j["value_numeric"].get<double>();
        if (j.contains("value_text")    && j["value_text"].is_string())    valueText    = j["value_text"].get<std::string>();
        if (j.contains("product_id")) productId = partM2o(j["product_id"]);
        if (j.contains("unit_id"))    unitId    = partM2o(j["unit_id"]);
    }
    std::vector<std::string> validate() const override { std::vector<std::string> e; if (name.empty()) e.push_back("Parameter name is required"); return e; }
};

// PK4 — part.manufacturer.info (manufacturer part numbers / MPN)
class PartManufacturerInfo : public BaseModel<PartManufacturerInfo> {
public:
    static constexpr const char* MODEL_NAME = "part.manufacturer.info";
    static constexpr const char* TABLE_NAME = "part_manufacturer_info";
    int         productId = 0;
    int         manufacturerId = 0;
    std::string partNumber, notes;
    explicit PartManufacturerInfo(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}
    void registerFields() {
        fieldRegistry_.add({"product_id",      FieldType::Many2one,"Product", false, false, true, true, "product.product"});
        fieldRegistry_.add({"manufacturer_id", FieldType::Many2one,"Manufacturer", true, false, true, true, "res.partner"});
        fieldRegistry_.add({"part_number",     FieldType::Char,    "Manufacturer Part Number", true});
        fieldRegistry_.add({"notes",           FieldType::Text,    "Notes"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["product_id"]      = productId > 0 ? nlohmann::json(productId) : nlohmann::json(false);
        j["manufacturer_id"] = manufacturerId > 0 ? nlohmann::json(manufacturerId) : nlohmann::json(false);
        j["part_number"]     = partNumber;
        j["notes"]           = notes.empty() ? nlohmann::json(false) : nlohmann::json(notes);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("part_number") && j["part_number"].is_string()) partNumber = j["part_number"].get<std::string>();
        if (j.contains("notes")       && j["notes"].is_string())       notes      = j["notes"].get<std::string>();
        if (j.contains("product_id"))      productId      = partM2o(j["product_id"]);
        if (j.contains("manufacturer_id")) manufacturerId = partM2o(j["manufacturer_id"]);
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (manufacturerId <= 0) e.push_back("Manufacturer is required");
        if (partNumber.empty())  e.push_back("Part number is required");
        return e;
    }
};

// part.parameter viewmodel — filtered by product_id, plus the parametric
// search that IS the electronics-catalogue differentiator (find parts by a
// parameter's numeric range, e.g. resistance between 1k and 10k).
class PartParameterViewModel : public core::BaseViewModel {
public:
    explicit PartParameterViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",  handleSearchRead)
        REGISTER_METHOD("read",         handleRead)
        REGISTER_MUTATOR("create",       handleCreate)
        REGISTER_MUTATOR("write",        handleWrite)
        REGISTER_MUTATOR("unlink",       handleUnlink)
        REGISTER_METHOD("fields_get",   handleFieldsGet)
        REGISTER_METHOD("search_parts", handleSearchParts)
    }
    std::string modelName() const override { return "part.parameter"; }
private:
    std::shared_ptr<DbConnection> db_;
    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        int prodFilter = 0;
        { auto d = call.domain(); if (d.is_array()) for (const auto& c : d)
            if (c.is_array() && c.size()==3 && c[0].is_string() && c[0].get<std::string>()=="product_id" && c[2].is_number_integer())
                prodFilter = c[2].get<int>(); }
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT pa.id, pa.product_id, pa.name, pa.value_numeric, pa.value_text,
                   pa.unit_id, u.name AS unit_name, u.symbol AS unit_symbol
            FROM part_parameter pa LEFT JOIN part_unit u ON u.id = pa.unit_id )";
        pqxx::params p;
        if (prodFilter > 0) { sql += " WHERE pa.product_id=$1"; p.append(prodFilter); }
        sql += " ORDER BY pa.name, pa.id";
        auto res = prodFilter > 0 ? txn.exec(sql, p) : txn.exec(sql);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]            = row["id"].as<int>();
            j["product_id"]    = row["product_id"].is_null() ? nlohmann::json(false) : nlohmann::json(row["product_id"].as<int>());
            j["name"]          = row["name"].is_null() ? "" : row["name"].c_str();
            j["value_numeric"] = row["value_numeric"].as<double>(0.0);
            j["value_text"]    = row["value_text"].is_null() ? nlohmann::json(false) : nlohmann::json(row["value_text"].c_str());
            j["unit_id"]       = row["unit_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["unit_id"].as<int>(), row["unit_symbol"].is_null() ? (row["unit_name"].is_null()?"":std::string(row["unit_name"].c_str())) : std::string(row["unit_symbol"].c_str())});
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const core::CallKwArgs& call)      { PartParameter p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids(), call.fields()); }
    nlohmann::json handleCreate(const core::CallKwArgs& call)    { PartParameter p(db_); p.setUserContext(extractContext_(call)); return p.create(call.arg(0)); }
    nlohmann::json handleWrite(const core::CallKwArgs& call)     { PartParameter p(db_); p.setUserContext(extractContext_(call)); return p.write(call.ids(), call.arg(1)); }
    nlohmann::json handleUnlink(const core::CallKwArgs& call)    { PartParameter p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids()); }
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call) { PartParameter p(db_); return p.fieldsGet(call.fields()); }

    // Parametric search: find products whose parameter `name` has a numeric
    // value within [min, max] (either bound optional).
    nlohmann::json handleSearchParts(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const std::string pname = (v.is_object() && v.contains("name") && v["name"].is_string()) ? v["name"].get<std::string>() : "";
        const bool hasMin = v.is_object() && v.contains("min") && v["min"].is_number();
        const bool hasMax = v.is_object() && v.contains("max") && v["max"].is_number();
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT DISTINCT pp.id, pp.name, pa.value_numeric
            FROM part_parameter pa JOIN product_product pp ON pp.id = pa.product_id WHERE 1=1 )";
        pqxx::params p; int n = 0;
        if (!pname.empty()) { sql += " AND pa.name=$" + std::to_string(++n); p.append(pname); }
        if (hasMin)         { sql += " AND pa.value_numeric >= $" + std::to_string(++n); p.append(v["min"].get<double>()); }
        if (hasMax)         { sql += " AND pa.value_numeric <= $" + std::to_string(++n); p.append(v["max"].get<double>()); }
        sql += " ORDER BY pa.value_numeric, pp.name LIMIT 500";
        auto res = n ? txn.exec(sql, p) : txn.exec(sql);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res)
            arr.push_back({{"id", row["id"].as<int>()},
                           {"name", row["name"].is_null()?"":row["name"].c_str()},
                           {"value", row["value_numeric"].as<double>(0.0)}});
        return arr;
    }
};

// part.manufacturer.info viewmodel — MPN lines, filtered by product_id.
class PartManufacturerInfoViewModel : public core::BaseViewModel {
public:
    explicit PartManufacturerInfoViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read", handleSearchRead)
        REGISTER_METHOD("read",        handleRead)
        REGISTER_MUTATOR("create",      handleCreate)
        REGISTER_MUTATOR("write",       handleWrite)
        REGISTER_MUTATOR("unlink",      handleUnlink)
        REGISTER_METHOD("fields_get",  handleFieldsGet)
    }
    std::string modelName() const override { return "part.manufacturer.info"; }
private:
    std::shared_ptr<DbConnection> db_;
    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        int prodFilter = 0;
        { auto d = call.domain(); if (d.is_array()) for (const auto& c : d)
            if (c.is_array() && c.size()==3 && c[0].is_string() && c[0].get<std::string>()=="product_id" && c[2].is_number_integer())
                prodFilter = c[2].get<int>(); }
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT m.id, m.product_id, m.manufacturer_id, rp.name AS manufacturer_name,
                   m.part_number, m.notes
            FROM part_manufacturer_info m LEFT JOIN res_partner rp ON rp.id = m.manufacturer_id )";
        pqxx::params p;
        if (prodFilter > 0) { sql += " WHERE m.product_id=$1"; p.append(prodFilter); }
        sql += " ORDER BY m.id";
        auto res = prodFilter > 0 ? txn.exec(sql, p) : txn.exec(sql);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]              = row["id"].as<int>();
            j["product_id"]      = row["product_id"].is_null() ? nlohmann::json(false) : nlohmann::json(row["product_id"].as<int>());
            j["manufacturer_id"] = row["manufacturer_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["manufacturer_id"].as<int>(), row["manufacturer_name"].is_null()?"":std::string(row["manufacturer_name"].c_str())});
            j["part_number"]     = row["part_number"].is_null() ? "" : row["part_number"].c_str();
            j["notes"]           = row["notes"].is_null() ? nlohmann::json(false) : nlohmann::json(row["notes"].c_str());
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const core::CallKwArgs& call)      { PartManufacturerInfo p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids(), call.fields()); }
    nlohmann::json handleCreate(const core::CallKwArgs& call)    { PartManufacturerInfo p(db_); p.setUserContext(extractContext_(call)); return p.create(call.arg(0)); }
    nlohmann::json handleWrite(const core::CallKwArgs& call)     { PartManufacturerInfo p(db_); p.setUserContext(extractContext_(call)); return p.write(call.ids(), call.arg(1)); }
    nlohmann::json handleUnlink(const core::CallKwArgs& call)    { PartManufacturerInfo p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids()); }
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call) { PartManufacturerInfo p(db_); return p.fieldsGet(call.fields()); }
};

class PartCatalogListView : public core::BaseView {
    std::string model_, name_, label_, extra_;
public:
    PartCatalogListView(std::string model, std::string name, std::string label, std::string extra)
        : model_(std::move(model)), name_(std::move(name)), label_(std::move(label)), extra_(std::move(extra)) {}
    std::string viewName()  const override { return name_; }
    std::string modelName() const override { return model_; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"" + label_ + "\"><field name=\"name\"/>" + extra_ + "</list>";
    }
    nlohmann::json fields() const override {
        nlohmann::json f = {{"name", {{"type","char"},{"string","Name"}}}};
        if (model_ == "part.unit") f["symbol"] = {{"type","char"},{"string","Symbol"}};
        else f["description"] = {{"type","text"},{"string","Description"}};
        return f;
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ================================================================
// 4. MODULE — method implementations
// ================================================================

ProductModule::ProductModule(core::ModelFactory&     models,
                             core::ServiceFactory&   services,
                             core::ViewModelFactory& viewModels,
                             core::ViewFactory&      views)
    : models_(models), services_(services),
      viewModels_(viewModels), views_(views)
{}

std::string ProductModule::moduleName() const { return "product"; }

void ProductModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("product.category", [db]{ return std::make_shared<ProductCategory>(db); });
    models_.registerCreator("product.product",  [db]{ return std::make_shared<ProductProduct>(db); });
    models_.registerCreator("product.supplierinfo", [db]{ return std::make_shared<ProductSupplierInfo>(db); });
    models_.registerCreator("part.footprint",          [db]{ return std::make_shared<PartFootprint>(db); });
    models_.registerCreator("part.unit",               [db]{ return std::make_shared<PartUnit>(db); });
    models_.registerCreator("part.parameter",          [db]{ return std::make_shared<PartParameter>(db); });
    models_.registerCreator("part.manufacturer.info",  [db]{ return std::make_shared<PartManufacturerInfo>(db); });
}

void ProductModule::registerServices() {}

void ProductModule::registerViewModels() {
    auto db = services_.db();
    // Custom VM — returns parent name, child_count, product_count
    viewModels_.registerCreator("product.category", [db]{
        return std::make_shared<ProductCategoryViewModel>(db);
    });
    viewModels_.registerCreator("product.product", [db]{
        return std::make_shared<GenericViewModel<ProductProduct>>(db);
    });
    viewModels_.registerCreator("product.supplierinfo", [db]{
        return std::make_shared<ProductSupplierInfoViewModel>(db);
    });
    viewModels_.registerCreator("part.footprint", [db]{ return std::make_shared<GenericViewModel<PartFootprint>>(db); });
    viewModels_.registerCreator("part.unit",      [db]{ return std::make_shared<GenericViewModel<PartUnit>>(db); });
    viewModels_.registerCreator("part.parameter", [db]{ return std::make_shared<PartParameterViewModel>(db); });
    viewModels_.registerCreator("part.manufacturer.info", [db]{ return std::make_shared<PartManufacturerInfoViewModel>(db); });
}

void ProductModule::registerViews() {
    views_.registerCreator("product.category.list", []{ return std::make_shared<ProductCategoryListView>(); });
    views_.registerCreator("product.category.form", []{ return std::make_shared<ProductCategoryFormView>(); });
    views_.registerCreator("product.product.list",  []{ return std::make_shared<ProductProductListView>(); });
    views_.registerCreator("product.product.form",  []{ return std::make_shared<ProductProductFormView>(); });
    views_.registerCreator("product.supplierinfo.list", []{ return std::make_shared<ProductSupplierInfoListView>(); });
    views_.registerCreator("part.footprint.list", []{ return std::make_shared<PartCatalogListView>("part.footprint","part.footprint.list","Footprints","<field name=\"description\"/>"); });
    views_.registerCreator("part.unit.list",      []{ return std::make_shared<PartCatalogListView>("part.unit","part.unit.list","Units of Measure","<field name=\"symbol\"/>"); });
}

void ProductModule::registerRoutes() {}

void ProductModule::initialize() {
    ensureSchema_();
    seedCategories_();
    seedMenus_();
}

void ProductModule::ensureSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS product_category (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            parent_id   INTEGER REFERENCES product_category(id) ON DELETE SET NULL,
            active      BOOLEAN NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");
    // Costing GL config on the category (optional overrides; NULL → seeded defaults).
    txn.exec("ALTER TABLE product_category ADD COLUMN IF NOT EXISTS property_stock_valuation_account_id INTEGER REFERENCES account_account(id) ON DELETE SET NULL");
    txn.exec("ALTER TABLE product_category ADD COLUMN IF NOT EXISTS property_stock_journal_id           INTEGER REFERENCES account_journal(id) ON DELETE SET NULL");
    txn.exec("ALTER TABLE product_category ADD COLUMN IF NOT EXISTS property_stock_account_input_id     INTEGER REFERENCES account_account(id) ON DELETE SET NULL");
    txn.exec("ALTER TABLE product_category ADD COLUMN IF NOT EXISTS property_stock_account_output_id    INTEGER REFERENCES account_account(id) ON DELETE SET NULL");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS product_product (
            id               SERIAL PRIMARY KEY,
            name             VARCHAR NOT NULL,
            default_code     VARCHAR,
            barcode          VARCHAR,
            description      TEXT,
            type             VARCHAR NOT NULL DEFAULT 'consu',
            categ_id         INTEGER REFERENCES product_category(id) ON DELETE SET NULL,
            uom_id           INTEGER NOT NULL REFERENCES uom_uom(id) DEFAULT 1,
            uom_po_id        INTEGER NOT NULL REFERENCES uom_uom(id) DEFAULT 1,
            list_price       NUMERIC(16,4) NOT NULL DEFAULT 0,
            standard_price   NUMERIC(16,4) NOT NULL DEFAULT 0,
            volume           NUMERIC(16,4) NOT NULL DEFAULT 0,
            weight           NUMERIC(16,4) NOT NULL DEFAULT 0,
            sale_ok          BOOLEAN NOT NULL DEFAULT TRUE,
            purchase_ok      BOOLEAN NOT NULL DEFAULT TRUE,
            company_id       INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            active           BOOLEAN NOT NULL DEFAULT TRUE,
            create_date      TIMESTAMP DEFAULT now(),
            write_date       TIMESTAMP DEFAULT now()
        )
    )");

    // Migrations for new columns
    // qty_available: on-hand cache (BIGINT micro-units), refreshed by the quant
    // engine (core/StockQuant) whenever a validated move touches this product.
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS qty_available BIGINT NOT NULL DEFAULT 0");
    // Costing: method + cached valued quantity/value (BIGINT micro-units).
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS cost_method VARCHAR NOT NULL DEFAULT 'standard'");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS quantity_svl BIGINT NOT NULL DEFAULT 0");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS value_svl BIGINT NOT NULL DEFAULT 0");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS tracking VARCHAR NOT NULL DEFAULT 'none'");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS expense_ok BOOLEAN NOT NULL DEFAULT FALSE");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS image_1920 TEXT");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS description_sale TEXT");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS description_purchase TEXT");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS income_account_id INTEGER REFERENCES account_account(id) ON DELETE SET NULL");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS expense_account_id INTEGER REFERENCES account_account(id) ON DELETE SET NULL");
    // Sales tab fields
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS invoice_policy     VARCHAR NOT NULL DEFAULT 'order'");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS sale_line_warn     VARCHAR NOT NULL DEFAULT 'no-message'");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS sale_line_warn_msg TEXT");
    // Purchase tab fields
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS purchase_method       VARCHAR NOT NULL DEFAULT 'purchase'");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS purchase_lead_time    NUMERIC(8,2) NOT NULL DEFAULT 0");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS purchase_line_warn    VARCHAR NOT NULL DEFAULT 'no-message'");
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS purchase_line_warn_msg TEXT");

    // Vendor pricelists (product.supplierinfo): which vendors sell a product,
    // at what price / MOQ / lead time. price/min_qty are BIGINT micro-units.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS product_supplierinfo (
            id           SERIAL PRIMARY KEY,
            product_id   INTEGER REFERENCES product_product(id) ON DELETE CASCADE,
            partner_id   INTEGER NOT NULL REFERENCES res_partner(id) ON DELETE CASCADE,
            product_name VARCHAR,
            product_code VARCHAR,
            min_qty      BIGINT  NOT NULL DEFAULT 0,
            price        BIGINT  NOT NULL DEFAULT 0,
            delay        INTEGER NOT NULL DEFAULT 1,
            sequence     INTEGER NOT NULL DEFAULT 10,
            company_id   INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date  TIMESTAMP DEFAULT now(),
            write_date   TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_supplierinfo_product ON product_supplierinfo(product_id)");

    // PartKeepr PK2-PK4 — parts catalogue.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS part_footprint (
            id SERIAL PRIMARY KEY, name VARCHAR NOT NULL, description TEXT,
            create_date TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS part_unit (
            id SERIAL PRIMARY KEY, name VARCHAR NOT NULL, symbol VARCHAR,
            create_date TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS part_parameter (
            id            SERIAL PRIMARY KEY,
            product_id    INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
            name          VARCHAR NOT NULL,
            value_numeric NUMERIC(24,9) NOT NULL DEFAULT 0,
            unit_id       INTEGER REFERENCES part_unit(id) ON DELETE SET NULL,
            value_text    VARCHAR,
            create_date   TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_part_parameter_product ON part_parameter(product_id)");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_part_parameter_name ON part_parameter(name)");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS part_manufacturer_info (
            id              SERIAL PRIMARY KEY,
            product_id      INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
            manufacturer_id INTEGER NOT NULL REFERENCES res_partner(id) ON DELETE CASCADE,
            part_number     VARCHAR NOT NULL,
            notes           TEXT,
            create_date     TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_part_mfg_product ON part_manufacturer_info(product_id)");
    // A product's footprint (PK2).
    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS footprint_id INTEGER REFERENCES part_footprint(id) ON DELETE SET NULL");

    txn.commit();
}

void ProductModule::seedCategories_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    // Root + basic categories (existing)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (1, 'All',      NULL),
            (2, 'Goods',    1),
            (3, 'Services', 1)
        ON CONFLICT (id) DO NOTHING
    )");

    // Electronics top-level (parent = 1 = All)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (10, 'Electronics',          1),
            (41, 'Mechanical & Hardware',1),
            (45, 'PCB & Fabrication',    1),
            (48, 'Cables & Wire',        1)
        ON CONFLICT (id) DO NOTHING
    )");

    // Electronics > Passives (parent=10)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (11, 'Passives',             10),
            (12, 'Resistors',            11),
            (13, 'Capacitors',           11),
            (14, 'Inductors & Coils',    11),
            (15, 'Crystals & Oscillators',11)
        ON CONFLICT (id) DO NOTHING
    )");

    // Electronics > Semiconductors (parent=10)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (16, 'Semiconductors',       10),
            (17, 'Diodes',               16),
            (18, 'Transistors',          16),
            (19, 'Voltage Regulators',   16),
            (20, 'Operational Amplifiers',16),
            (21, 'Logic ICs',            16),
            (22, 'Microcontrollers',     16),
            (23, 'Memory ICs',           16),
            (24, 'Interface ICs',        16),
            (25, 'RF & Wireless',        16),
            (26, 'Sensors',              16),
            (38, 'Power Management ICs', 16),
            (39, 'Optocouplers',         16)
        ON CONFLICT (id) DO NOTHING
    )");

    // Electronics > Display & LED (parent=10)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (27, 'Display & LED',        10),
            (28, 'Discrete LEDs',        27),
            (29, 'LED Drivers',          27),
            (30, 'Display Modules',      27)
        ON CONFLICT (id) DO NOTHING
    )");

    // Electronics > Electromechanical (parent=10)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (31, 'Electromechanical',    10),
            (32, 'Relays',               31),
            (33, 'Switches & Buttons',   31),
            (34, 'Connectors',           31),
            (35, 'Motors & Actuators',   31),
            (36, 'Fuses & Protection',   31)
        ON CONFLICT (id) DO NOTHING
    )");

    // Electronics > Power (parent=10)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (37, 'Power',                10),
            (40, 'Power Modules',        37)
        ON CONFLICT (id) DO NOTHING
    )");

    // Resistor sub-types (parent=12)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (51, 'Through-Hole Resistors', 12),
            (52, 'SMD Resistors',           12),
            (53, 'Potentiometers',          12),
            (54, 'Resistor Networks',       12)
        ON CONFLICT (id) DO NOTHING
    )");

    // Capacitor sub-types (parent=13)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (55, 'Ceramic Capacitors (MLCC)', 13),
            (56, 'Electrolytic Capacitors',   13),
            (57, 'Tantalum Capacitors',        13),
            (58, 'Film Capacitors',            13),
            (59, 'Supercapacitors',            13)
        ON CONFLICT (id) DO NOTHING
    )");

    // Diode sub-types (parent=17)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (60, 'Signal / Switching Diodes', 17),
            (61, 'Rectifier Diodes',           17),
            (62, 'Schottky Diodes',            17),
            (63, 'Zener Diodes',               17),
            (64, 'TVS / ESD Protection',       17)
        ON CONFLICT (id) DO NOTHING
    )");

    // Transistor sub-types (parent=18)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (65, 'NPN BJT',          18),
            (66, 'PNP BJT',          18),
            (67, 'N-Channel MOSFET', 18),
            (68, 'P-Channel MOSFET', 18),
            (69, 'JFET',             18)
        ON CONFLICT (id) DO NOTHING
    )");

    // Voltage regulator sub-types (parent=19)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (70, 'Linear Regulators (LDO)', 19),
            (71, 'Buck Regulators',          19),
            (72, 'Boost Regulators',         19),
            (73, 'Buck-Boost Regulators',    19),
            (74, 'Voltage References',        19)
        ON CONFLICT (id) DO NOTHING
    )");

    // Sensor sub-types (parent=26)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (75, 'Temperature Sensors',   26),
            (76, 'Humidity Sensors',      26),
            (77, 'Pressure Sensors',      26),
            (78, 'IMU / Accelerometer',   26),
            (79, 'Magnetic / Hall Effect',26),
            (80, 'Light / Proximity',     26),
            (81, 'Current Monitors',      26)
        ON CONFLICT (id) DO NOTHING
    )");

    // Mechanical sub-types
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (42, 'Fasteners',          41),
            (43, 'Heatsinks & Thermal',41),
            (44, 'Enclosures',         41),
            (46, 'Blank PCBs',         45),
            (47, 'Prototyping Board',  45),
            (49, 'Hook-Up Wire',       48),
            (50, 'Cables & Adapters',  48)
        ON CONFLICT (id) DO NOTHING
    )");

    // Connector sub-types (parent=34)
    txn.exec(R"(
        INSERT INTO product_category (id, name, parent_id) VALUES
            (82, 'Pin Headers',          34),
            (83, 'JST Connectors',       34),
            (84, 'USB Connectors',       34),
            (85, 'Screw Terminals',      34),
            (86, 'Audio / RF Connectors',34)
        ON CONFLICT (id) DO NOTHING
    )");

    txn.exec("SELECT setval('product_category_id_seq', GREATEST((SELECT MAX(id) FROM product_category), 100), true)");

    // ir_act_window entries — IDs 9/10 are owned by product; account uses 32/33 for invoices/bills
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, path, context) VALUES
            (9,  'Products',           'product.product',      'list,form', 'products',            '{}'),
            (10, 'Product Categories', 'product.category',     'list,form', 'product-categories',  '{}'),
            (11, 'Vendor Pricelists',  'product.supplierinfo', 'list,form', 'vendor-pricelists',   '{}'),
            (12, 'Footprints',         'part.footprint',       'list,form', 'footprints',          '{}'),
            (13, 'Part Units',         'part.unit',            'list,form', 'part-units',          '{}'),
            (15, 'Parametric Search',  'part.search',          'list',      'parametric-search',   '{}')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
                view_mode=EXCLUDED.view_mode, path=EXCLUDED.path, domain=NULL
    )");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
    txn.commit();
}

void ProductModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    // Level 1: Products direct link (under Products app, id=50)
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (51, 'Products', 50, 10, 9)
        ON CONFLICT (id) DO UPDATE
            SET action_id=EXCLUDED.action_id
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (58, 'Parametric Search', 50, 15, 15)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    // Level 2: Categories under Configuration section (id=52, created by UomModule)
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (54, 'Categories', 52, 20, 10)
        ON CONFLICT (id) DO UPDATE
            SET action_id=EXCLUDED.action_id
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (55, 'Vendor Pricelists', 52, 30, 11),
            (56, 'Footprints',        52, 40, 12),
            (57, 'Part Units',        52, 50, 13)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");
    txn.commit();
}

} // namespace odoo::modules::product

// =============================================================
// modules/product/ProductModule.cpp
// =============================================================
#include "ProductModule.hpp"
#include <drogon/drogon.h>   // LOG_INFO (docs/096)
#include "BaseModel.hpp"
#include "RecordRuleSql.hpp"
#include "DecimalPrecision.hpp"
#include "BaseView.hpp"
#include "GenericViewModel.hpp"
#include "BaseViewModel.hpp"
#include "Money.hpp"
#include "DbConnection.hpp"
#include "LabelRenderer.hpp"      // docs/099 — QR + label SVG
#include "SessionManager.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <algorithm>

namespace odoo::modules::product {

using namespace odoo::infrastructure;
using namespace odoo::core;

/// docs/097 — parse "4k7", "100n", "2R2", "10 uF". Defined further down, but
/// declared here because the parametric search uses it before that point.
static bool parseSiValue(const std::string& text, double& number, double& mult);

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
// 2b. PRODUCT.PRODUCT VIEW MODEL
//
// Generic in every respect but one: a variant created through the API must end
// up with a TEMPLATE. Creating `product.product` directly used to leave
// product_tmpl_id NULL, which produces a product that:
//
//   * the variant screens cannot show (they list variants OF a template),
//   * no pricelist rule keyed on a template can price,
//   * and every global integrity check counts as broken — which is how this
//     was found: product-variants asserts "no product is without a template"
//     across the whole database, and started failing the moment any earlier
//     test created a product through the API.
//
// The template is synthesised from the product's own values, so the pair is
// consistent from the moment it exists. There is no FK on product_tmpl_id,
// which is exactly why nothing objected before.
// ================================================================

class ProductProductViewModel : public core::GenericViewModel<ProductProduct> {
public:
    explicit ProductProductViewModel(std::shared_ptr<DbConnection> db)
        : core::GenericViewModel<ProductProduct>(db), pdb_(db)
    {
        // Re-registering "create" replaces the generic one inherited above.
        REGISTER_MUTATOR("create", handleCreateWithTemplate)
    }

private:
    std::shared_ptr<DbConnection> pdb_;

    nlohmann::json handleCreateWithTemplate(const core::CallKwArgs& call) {
        auto res = handleCreate(call);
        if (res.is_number_integer()) {
            const int id = res.get<int>();
            if (id > 0) ensureTemplate_(id);
        }
        return res;
    }

    void ensureTemplate_(int productId) {
        auto conn = pdb_->acquire();
        pqxx::work txn{conn.get()};

        auto r = txn.exec("SELECT product_tmpl_id FROM product_product WHERE id=$1",
                          pqxx::params{productId});
        if (r.empty()) return;

        // A dangling id counts as missing: pointing at a template that was
        // deleted is worse than pointing at nothing, because it silently
        // adopts whatever later takes that id.
        if (!r[0][0].is_null()) {
            auto e = txn.exec("SELECT 1 FROM product_template WHERE id=$1",
                              pqxx::params{r[0][0].as<int>()});
            if (!e.empty()) return;
        }

        auto ins = txn.exec(
            "INSERT INTO product_template "
            "  (name, default_code, type, categ_id, uom_id, uom_po_id, list_price, "
            "   standard_price, sale_ok, purchase_ok, active, company_id) "
            "SELECT name, default_code, type, categ_id, uom_id, uom_po_id, list_price, "
            "       standard_price, sale_ok, purchase_ok, TRUE, company_id "
            "  FROM product_product WHERE id=$1 RETURNING id",
            pqxx::params{productId});
        if (ins.empty()) return;

        txn.exec("UPDATE product_product SET product_tmpl_id=$1 WHERE id=$2",
                 pqxx::params{ins[0][0].as<int>(), productId});
        txn.commit();
    }
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
        REGISTER_METHOD("tree",            handleTree)
        REGISTER_METHOD("detail",          handleDetail)
    }

    std::string modelName() const override { return "product.category"; }

    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        // Domain filter.
        //
        // This used to understand only `active` and `parent_id` and SILENTLY
        // IGNORE every other leaf, so `[["name","=","PCB Assembly"]]` matched
        // nothing in the chain, the clause stayed 1=1, and the call returned all
        // 80 categories. A filter that quietly widens is the worst failure mode
        // available: the caller believes it asked a narrow question and gets an
        // answer to a different one.
        //
        // Unknown fields and operators are now REFUSED. Values are bound as $n
        // (S-49) — only the column name is chosen, and only from this allowlist.
        std::string whereClause = "1=1";
        pqxx::params params;
        int pn = 0;
        const auto& domain = call.domain();
        if (domain.is_array()) {
            for (const auto& leaf : domain) {
                if (!leaf.is_array() || leaf.size() < 3) continue;
                if (!leaf[0].is_string() || !leaf[1].is_string()) continue;
                const std::string field = leaf[0].get<std::string>();
                const std::string op    = leaf[1].get<std::string>();
                const auto& val = leaf[2];

                if (field == "active") {
                    const bool want = val.is_boolean() ? val.get<bool>() : true;
                    whereClause += want ? " AND pc.active = TRUE" : " AND pc.active = FALSE";
                } else if (field == "parent_id") {
                    if (val.is_null() || val == false)
                        whereClause += (op == "!=") ? " AND pc.parent_id IS NOT NULL"
                                                    : " AND pc.parent_id IS NULL";
                    else if (val.is_number_integer()) {
                        whereClause += " AND pc.parent_id " + std::string(op == "!=" ? "<>" : "=")
                                     + " $" + std::to_string(++pn);
                        params.append(val.get<int>());
                    }
                } else if (field == "id" && val.is_number_integer()) {
                    whereClause += " AND pc.id " + std::string(op == "!=" ? "<>" : "=")
                                 + " $" + std::to_string(++pn);
                    params.append(val.get<int>());
                } else if (field == "name" || field == "display_name") {
                    if (!val.is_string())
                        throw infrastructure::ValidationError("The '" + field + "' filter needs text.");
                    const std::string s = val.get<std::string>();
                    if (op == "=" || op == "!=") {
                        whereClause += " AND pc.name " + std::string(op == "!=" ? "<>" : "=")
                                     + " $" + std::to_string(++pn);
                        params.append(s);
                    } else if (op == "ilike" || op == "like" || op == "=ilike") {
                        whereClause += " AND pc.name ILIKE $" + std::to_string(++pn);
                        params.append(op == "=ilike" ? s : "%" + s + "%");
                    } else {
                        throw infrastructure::ValidationError(
                            "Unsupported operator '" + op + "' on category name.");
                    }
                } else {
                    throw infrastructure::ValidationError(
                        "Cannot filter categories on '" + field +
                        "'. Supported: id, name, parent_id, active.");
                }
            }
        }

        int limit  = call.limit()  > 0 ? call.limit()  : 500;
        int offset = call.offset();

        const std::string sql =
            "SELECT pc.id, pc.name, pc.active, pc.parent_id, "
            "COALESCE(par.name,'') AS parent_name, "
            "(SELECT COUNT(*) FROM product_category c2 WHERE c2.parent_id = pc.id) AS child_count, "
            "(SELECT COUNT(*) FROM product_product pp WHERE pp.categ_id = pc.id) AS product_count "
            "FROM product_category pc "
            "LEFT JOIN product_category par ON par.id = pc.parent_id "
            "WHERE " + whereClause + " "
            "ORDER BY pc.name "
            "LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);
        auto rows = pn ? txn.exec(sql, params) : txn.exec(sql);

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

    // A category's name is its whole identity — it is the label in the tree and
    // in every product's category picker. This handler writes raw SQL and so
    // never reaches ProductCategory::validate(), which meant `create({})`
    // quietly inserted a nameless row that rendered as a blank line in the
    // sidebar and could not be told apart from any other blank one. 29 of them
    // had accumulated, one per test-suite run (docs/092).
    static std::string requireName_(const nlohmann::json& v) {
        std::string name = v.is_string() ? v.get<std::string>() : std::string();
        const auto b = name.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            throw infrastructure::ValidationError("Category name is required.");
        const auto e = name.find_last_not_of(" \t\r\n");
        return name.substr(b, e - b + 1);
    }

    nlohmann::json handleCreate(const core::CallKwArgs& call) {
        const auto vals = call.arg(0);
        if (!vals.is_object()) throw std::runtime_error("create: args[0] must be a dict");
        std::string name   = requireName_(vals.contains("name") ? vals["name"] : nlohmann::json());
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
            // Renaming to blank is the same defect as creating blank.
            if (vals.contains("name"))
                txn.exec("UPDATE product_category SET name=$1, write_date=now() WHERE id=$2",
                         pqxx::params{requireName_(vals["name"]), id});
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

    // ----------------------------------------------------------
    // tree — the whole category hierarchy in ONE call.
    //
    // The screen is a tree, so it is fetched as a tree. Walking the levels
    // with a search_read per expanded node would be a request per click and
    // would make the counts inconsistent between levels, because each would
    // be measured at a different moment.
    //
    // Two product counts per node, and they answer different questions:
    //   direct_count — products filed in exactly this category
    //   total_count  — products anywhere beneath it
    // A category showing "0" while its children hold hundreds is the thing
    // that makes people distrust the screen.
    // ----------------------------------------------------------
    nlohmann::json handleTree(const core::CallKwArgs& call) {
        const auto& kw = call.kwargs;
        const bool includeArchived =
            kw.contains("include_archived") && kw["include_archived"].is_boolean()
                ? kw["include_archived"].get<bool>() : false;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        // Recursive descendant counts, computed in the database. Doing this in
        // the client would mean shipping every product id to the browser.
        auto rows = txn.exec(
            "WITH RECURSIVE descend AS ("
            "    SELECT id AS root, id AS node FROM product_category"
            "  UNION ALL"
            "    SELECT d.root, c.id FROM product_category c"
            "      JOIN descend d ON c.parent_id = d.node"
            "), totals AS ("
            "    SELECT d.root, COUNT(p.id) AS total"
            "      FROM descend d LEFT JOIN product_product p ON p.categ_id = d.node"
            "     GROUP BY d.root"
            ") "
            "SELECT c.id, c.name, c.parent_id, c.active, "
            "       (SELECT COUNT(*) FROM product_product p WHERE p.categ_id = c.id) AS direct_count, "
            "       COALESCE(t.total, 0) AS total_count, "
            "       (SELECT COUNT(*) FROM product_category k WHERE k.parent_id = c.id) AS child_count "
            "  FROM product_category c "
            "  LEFT JOIN totals t ON t.root = c.id "
            + std::string(includeArchived ? "" : " WHERE c.active = TRUE ") +
            " ORDER BY c.parent_id NULLS FIRST, c.name",
            pqxx::params{});

        nlohmann::json nodes = nlohmann::json::array();
        for (const auto& r : rows) {
            nodes.push_back({
                {"id",           r["id"].as<int>()},
                {"name",         r["name"].c_str()},
                {"parent_id",    r["parent_id"].is_null() ? 0 : r["parent_id"].as<int>()},
                {"active",       r["active"].as<bool>(true)},
                {"direct_count", r["direct_count"].as<int>(0)},
                {"total_count",  r["total_count"].as<int>(0)},
                {"child_count",  r["child_count"].as<int>(0)},
            });
        }
        return {{"nodes", nodes}, {"count", nodes.size()}};
    }

    // ----------------------------------------------------------
    // detail — everything the right-hand panel shows for one category.
    //
    // One call rather than five: the panel is opened by a click, and a click
    // that fires five requests shows its sections popping in one at a time.
    // ----------------------------------------------------------
    nlohmann::json handleDetail(const core::CallKwArgs& call) {
        int id = 0;
        if (call.args.is_array() && !call.args.empty()) {
            const auto& a = call.args[0];
            if (a.is_number_integer())                     id = a.get<int>();
            else if (a.is_object() && a.contains("id"))    id = a["id"].get<int>();
            else if (a.is_array() && !a.empty() && a[0].is_number_integer())
                                                           id = a[0].get<int>();
        }
        if (id <= 0)
            throw infrastructure::ValidationError("detail: an existing category id is required.");

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        auto cat = txn.exec(
            "SELECT c.id, c.name, c.parent_id, c.active, c.create_date, c.write_date, "
            "       p.name AS parent_name, "
            "       c.property_stock_valuation_account_id AS acc_val, "
            "       c.property_stock_journal_id           AS acc_jrn, "
            "       c.property_stock_account_input_id     AS acc_in, "
            "       c.property_stock_account_output_id    AS acc_out "
            "  FROM product_category c "
            "  LEFT JOIN product_category p ON p.id = c.parent_id "
            " WHERE c.id = $1", pqxx::params{id});
        if (cat.empty())
            throw infrastructure::ValidationError("No category with id " + std::to_string(id) + ".");

        // The full path, built by walking up. It is what tells you where you
        // are once the tree is scrolled and the parent is off-screen.
        auto pathRows = txn.exec(
            "WITH RECURSIVE up AS ("
            "    SELECT id, name, parent_id, 0 AS depth FROM product_category WHERE id = $1"
            "  UNION ALL"
            "    SELECT c.id, c.name, c.parent_id, u.depth + 1"
            "      FROM product_category c JOIN up u ON c.id = u.parent_id"
            ") SELECT id, name FROM up ORDER BY depth DESC", pqxx::params{id});
        nlohmann::json path = nlohmann::json::array();
        for (const auto& r : pathRows)
            path.push_back({{"id", r["id"].as<int>()}, {"name", r["name"].c_str()}});

        auto counts = txn.exec(
            "WITH RECURSIVE descend AS ("
            "    SELECT $1::int AS node"
            "  UNION ALL"
            "    SELECT c.id FROM product_category c JOIN descend d ON c.parent_id = d.node"
            ") "
            "SELECT (SELECT COUNT(*) FROM product_product WHERE categ_id = $1) AS direct, "
            "       (SELECT COUNT(*) FROM product_product WHERE categ_id IN (SELECT node FROM descend)) AS total, "
            "       (SELECT COUNT(*) FROM product_category WHERE parent_id = $1) AS children, "
            "       (SELECT COUNT(*) FROM descend) - 1 AS descendants",
            pqxx::params{id});

        // A sample of what is actually filed here — the question anyone asks
        // next. Capped, because a category can hold thousands.
        auto prods = txn.exec(
            "SELECT id, name, COALESCE(default_code,'') AS code, "
            "       COALESCE(list_price,0) AS list_price, active "
            "  FROM product_product WHERE categ_id = $1 "
            " ORDER BY COALESCE(default_code, name) LIMIT 25", pqxx::params{id});
        nlohmann::json products = nlohmann::json::array();
        for (const auto& r : prods)
            products.push_back({
                {"id",         r["id"].as<int>()},
                {"name",       r["name"].c_str()},
                {"code",       r["code"].c_str()},
                {"list_price", core::Money::fromMicros(r["list_price"].as<long long>(0)).toJson()},
                {"active",     r["active"].as<bool>(true)},
            });

        auto kids = txn.exec(
            "SELECT c.id, c.name, "
            "       (SELECT COUNT(*) FROM product_product p WHERE p.categ_id = c.id) AS direct_count "
            "  FROM product_category c WHERE c.parent_id = $1 ORDER BY c.name",
            pqxx::params{id});
        nlohmann::json children = nlohmann::json::array();
        for (const auto& r : kids)
            children.push_back({{"id", r["id"].as<int>()},
                                {"name", r["name"].c_str()},
                                {"direct_count", r["direct_count"].as<int>(0)}});

        auto accName = [&](const char* col) -> nlohmann::json {
            if (cat[0][col].is_null()) return nullptr;
            const int aid = cat[0][col].as<int>();
            auto a = txn.exec("SELECT name FROM account_account WHERE id=$1", pqxx::params{aid});
            if (a.empty()) return nullptr;
            return nlohmann::json{{"id", aid}, {"name", a[0][0].c_str()}};
        };

        return {
            {"id",          cat[0]["id"].as<int>()},
            {"name",        cat[0]["name"].c_str()},
            {"parent_id",   cat[0]["parent_id"].is_null() ? 0 : cat[0]["parent_id"].as<int>()},
            {"parent_name", cat[0]["parent_name"].is_null() ? "" : cat[0]["parent_name"].c_str()},
            {"active",      cat[0]["active"].as<bool>(true)},
            {"create_date", cat[0]["create_date"].is_null() ? "" : cat[0]["create_date"].c_str()},
            {"write_date",  cat[0]["write_date"].is_null()  ? "" : cat[0]["write_date"].c_str()},
            {"path",        path},
            {"counts", {
                {"direct",      counts[0]["direct"].as<int>(0)},
                {"total",       counts[0]["total"].as<int>(0)},
                {"children",    counts[0]["children"].as<int>(0)},
                {"descendants", counts[0]["descendants"].as<int>(0)},
            }},
            {"products", products},
            {"children_list", children},
            {"accounts", {
                {"valuation", accName("acc_val")},
                {"journal",   nullptr},
                {"input",     accName("acc_in")},
                {"output",    accName("acc_out")},
            }},
        };
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
    // docs/097 — value_base is what parametric search compares on, so it has to
    // be recomputed on every write. A parameter saved through this screen
    // without it would simply never appear in a range search: no error, no row.
    void normalise_(pqxx::transaction_base& txn, int paramId) {
        txn.exec("UPDATE part_parameter p SET "
                 "  value_base = p.value_numeric * COALESCE(u.factor, 1), "
                 "  quantity_kind = u.quantity_kind "
                 "FROM part_unit u WHERE u.id = p.unit_id AND p.id = $1", pqxx::params{paramId});
        txn.exec("UPDATE part_parameter SET value_base = value_numeric, quantity_kind = NULL "
                 "WHERE id = $1 AND unit_id IS NULL", pqxx::params{paramId});
    }
    void normaliseIds_(const std::vector<int>& ids) {
        if (ids.empty()) return;
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        for (int id : ids) normalise_(txn, id);
        txn.commit();
    }
    nlohmann::json handleCreate(const core::CallKwArgs& call)    {
        PartParameter p(db_); p.setUserContext(extractContext_(call));
        const int newId = p.create(call.arg(0));
        if (newId > 0) normaliseIds_({newId});
        return newId;
    }
    nlohmann::json handleWrite(const core::CallKwArgs& call)     {
        PartParameter p(db_); p.setUserContext(extractContext_(call));
        const auto res = p.write(call.ids(), call.arg(1));
        normaliseIds_(call.ids());
        return res;
    }
    nlohmann::json handleUnlink(const core::CallKwArgs& call)    { PartParameter p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids()); }
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call) { PartParameter p(db_); return p.fieldsGet(call.fields()); }

    /**
     * Parametric search (docs/097).
     *
     * Bounds are compared on `value_base` — every value in the SI base of its
     * quantity — so 4.7 kΩ, 4700 Ω and "4k7" are the same number and all three
     * match a 4k–5k range. Comparing `value_numeric` (as this did) meant a part
     * entered in kΩ never matched one entered in Ω, which made the whole screen
     * quietly untrustworthy.
     *
     * Bounds may be written the way component values are written: "4k7",
     * "100n", "2R2", "10 uF". A bare number is taken in the given `unit`, or in
     * the base unit when none is given.
     */
    nlohmann::json handleSearchParts(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        auto str = [&](const char* k) -> std::string {
            return (v.is_object() && v.contains(k) && v[k].is_string()) ? v[k].get<std::string>() : std::string{};
        };
        const std::string pname = str("name");
        const std::string unit  = str("unit");

        auto conn = db_->acquire(); pqxx::work txn{conn.get()};

        // A supplied unit fixes both the scale and the quantity, so a resistance
        // range cannot accidentally match a capacitance that shares a number.
        double factor = 1.0; std::string kind;
        if (!unit.empty()) {
            auto u = txn.exec("SELECT factor, quantity_kind FROM part_unit WHERE symbol=$1",
                              pqxx::params{unit});
            if (u.empty())
                throw infrastructure::ValidationError("Unknown unit '" + unit + "'.");
            factor = u[0][0].as<double>(1);
            kind   = u[0][1].is_null() ? "" : u[0][1].c_str();
        }

        // Accept a bound as a number or as written text ("4k7").
        auto bound = [&](const char* k, double& out) -> bool {
            if (!v.is_object() || !v.contains(k)) return false;
            if (v[k].is_number()) { out = v[k].get<double>() * factor; return true; }
            if (v[k].is_string()) {
                const std::string t = v[k].get<std::string>();
                if (t.empty()) return false;
                double num = 0, mul = 1;
                if (!parseSiValue(t, num, mul))
                    throw infrastructure::ValidationError("Cannot read value '" + t + "'.");
                // A prefix written into the text already scaled the number; the
                // unit's own factor then takes it to the base.
                out = num * factor;
                return true;
            }
            return false;
        };
        double lo = 0, hi = 0;
        const bool hasMin = bound("min", lo);
        const bool hasMax = bound("max", hi);

        std::string sql =
            "SELECT DISTINCT pp.id, pp.name, pa.value_base, pa.value_numeric, "
            "       COALESCE(u.symbol,'') AS sym, COALESCE(pa.quantity_kind,'') AS kind "
            "FROM part_parameter pa "
            "JOIN product_product pp ON pp.id = pa.product_id "
            "LEFT JOIN part_unit u ON u.id = pa.unit_id "
            "WHERE pp.active ";
        pqxx::params p; int n = 0;
        if (!pname.empty()) { sql += " AND pa.name=$" + std::to_string(++n); p.append(pname); }
        if (!kind.empty())  { sql += " AND pa.quantity_kind=$" + std::to_string(++n); p.append(kind); }
        if (hasMin)         { sql += " AND pa.value_base >= $" + std::to_string(++n); p.append(lo); }
        if (hasMax)         { sql += " AND pa.value_base <= $" + std::to_string(++n); p.append(hi); }
        sql += " ORDER BY pa.value_base, pp.name LIMIT 500";
        auto res = n ? txn.exec(sql, p) : txn.exec(sql);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            const double vb = row["value_base"].is_null() ? 0.0 : row["value_base"].as<double>(0.0);
            arr.push_back({{"id", row["id"].as<int>()},
                           {"name", row["name"].is_null() ? "" : row["name"].c_str()},
                           // Both are returned: the base value is what matched,
                           // the entered value is what the user will recognise.
                           {"value", row["value_numeric"].as<double>(0.0)},
                           {"value_base", vb},
                           {"unit", row["sym"].c_str()},
                           {"quantity", row["kind"].c_str()}});
        }
        return arr;
    }
};

/*
 * part.catalog — the faceted parts browser (docs/098).
 *
 * Modelled on how a distributor catalogue actually behaves, because that is the
 * interaction electronics buyers already know:
 *
 *   - within one facet the selected values are OR'd  (0402 or 0603)
 *   - across facets they are AND'd                   (0402 AND YAGEO)
 *   - a facet's value counts are computed with *its own* selection removed,
 *     so after picking 0402 the Package facet still shows how many 0603 there
 *     are. Counting with the facet's own clause applied would collapse every
 *     other value to zero and make multi-select useless.
 *
 * Two methods: `facets` returns the filter strip for the current state, and
 * `search` returns the page of rows. They share one filter parser so the strip
 * can never describe a different result set than the table below it.
 *
 * S-49: a facet key is matched against a fixed shape ("mfr", "pkg",
 * "param:<name>") and the <name> is *bound* as $n, never interpolated. An
 * unrecognised key is dropped. No user text reaches SQL as an identifier.
 */
class PartCatalogViewModel : public core::BaseViewModel {
public:
    explicit PartCatalogViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("facets",     handleFacets)
        REGISTER_METHOD("search",     handleSearch)
        REGISTER_METHOD("categories", handleCategories)
    }
    std::string modelName() const override { return "part.catalog"; }

private:
    std::shared_ptr<DbConnection> db_;

    // ---- parsed filter state ------------------------------------------------
    struct EnumSel {
        std::string key, param;
        std::vector<std::string> values;
    };
    struct RangeSel {
        std::string key, param, unit, kind;
        nlohmann::json minJ, maxJ;
        bool hasMin = false, hasMax = false;
        double lo = 0, hi = 0;
    };
    struct Query {
        int  categId = 0;
        std::string text;
        bool inStock = false;
        std::vector<EnumSel>  enums;
        std::vector<RangeSel> ranges;
    };

    // A key is either one of two fixed product-level facets or a parameter
    // facet naming its parameter. Anything else is not a facet we serve.
    static bool keyOk_(const std::string& key, std::string& param) {
        if (key == "mfr" || key == "pkg") { param.clear(); return true; }
        if (key.rfind("param:", 0) == 0 && key.size() > 6) { param = key.substr(6); return true; }
        return false;
    }

    static std::string inList_(const std::vector<std::string>& vals, pqxx::params& p, int& n) {
        std::string s = "(";
        for (size_t i = 0; i < vals.size(); ++i) {
            if (i) s += ",";
            s += "$" + std::to_string(++n);
            p.append(vals[i]);
        }
        return s + ")";
    }

    Query parse_(const nlohmann::json& v) const {
        Query q;
        if (!v.is_object()) return q;
        if (v.contains("categ_id") && v["categ_id"].is_number_integer()) q.categId = v["categ_id"].get<int>();
        if (v.contains("q")        && v["q"].is_string())                q.text    = v["q"].get<std::string>();
        if (v.contains("in_stock") && v["in_stock"].is_boolean())        q.inStock = v["in_stock"].get<bool>();

        if (v.contains("enum") && v["enum"].is_object()) {
            for (auto it = v["enum"].begin(); it != v["enum"].end(); ++it) {
                EnumSel e; e.key = it.key();
                if (!keyOk_(e.key, e.param) || !it.value().is_array()) continue;
                for (const auto& x : it.value())
                    if (x.is_string() && !x.get<std::string>().empty()) e.values.push_back(x.get<std::string>());
                if (!e.values.empty()) q.enums.push_back(std::move(e));
            }
        }
        if (v.contains("range") && v["range"].is_object()) {
            for (auto it = v["range"].begin(); it != v["range"].end(); ++it) {
                RangeSel r; r.key = it.key();
                if (!keyOk_(r.key, r.param) || r.param.empty() || !it.value().is_object()) continue;
                const auto& o = it.value();
                if (o.contains("unit") && o["unit"].is_string()) r.unit = o["unit"].get<std::string>();
                if (o.contains("min")) r.minJ = o["min"];
                if (o.contains("max")) r.maxJ = o["max"];
                q.ranges.push_back(std::move(r));
            }
        }
        return q;
    }

    // A range bound arrives either as a number or as written text ("4k7"). The
    // unit fixes both the scale and the quantity kind, so a resistance range can
    // never match a capacitance that happens to share a number.
    void resolveRanges_(pqxx::work& txn, Query& q) const {
        for (auto& r : q.ranges) {
            double factor = 1.0;
            if (!r.unit.empty()) {
                auto u = txn.exec("SELECT factor, quantity_kind FROM part_unit WHERE symbol=$1",
                                  pqxx::params{r.unit});
                if (u.empty())
                    throw infrastructure::ValidationError("Unknown unit '" + r.unit + "'.");
                factor = u[0][0].as<double>(1.0);
                r.kind = u[0][1].is_null() ? "" : u[0][1].c_str();
            }
            auto bound = [&](const nlohmann::json& j, double& out) -> bool {
                if (j.is_number()) { out = j.get<double>() * factor; return true; }
                if (j.is_string()) {
                    const std::string t = j.get<std::string>();
                    if (t.empty()) return false;
                    double num = 0, mul = 1;
                    if (!parseSiValue(t, num, mul))
                        throw infrastructure::ValidationError("Cannot read value '" + t + "'.");
                    out = num * factor;
                    return true;
                }
                return false;
            };
            r.hasMin = bound(r.minJ, r.lo);
            r.hasMax = bound(r.maxJ, r.hi);
        }
    }

    // The shared WHERE. `skipKey` drops one facet's own clauses so that facet
    // can count its unselected values; pass "" for the true result set.
    std::string where_(const Query& q, const std::string& skipKey, pqxx::params& p, int& n) const {
        std::string w = " WHERE pp.active ";

        // Company scope (docs/094): rows shared across companies carry NULL.
        const auto& ctx = core::CurrentUser::get();
        if (!ctx.allowedCompanyIds.empty()) {
            w += " AND (pp.company_id IS NULL OR pp.company_id IN (";
            for (size_t i = 0; i < ctx.allowedCompanyIds.size(); ++i) {
                if (i) w += ",";
                w += "$" + std::to_string(++n);
                p.append(ctx.allowedCompanyIds[i]);
            }
            w += "))";
        }
        if (q.categId > 0) {
            // The whole subtree — picking "Resistors" must show SMD ones too.
            w += " AND pp.categ_id IN (WITH RECURSIVE sub AS ("
                 "SELECT id FROM product_category WHERE id=$" + std::to_string(++n) +
                 " UNION ALL SELECT c.id FROM product_category c JOIN sub ON c.parent_id=sub.id"
                 ") SELECT id FROM sub)";
            p.append(q.categId);
        }
        if (!q.text.empty()) {
            const std::string ph = "$" + std::to_string(++n);
            p.append("%" + q.text + "%");
            w += " AND (pp.name ILIKE " + ph +
                 " OR COALESCE(pp.default_code,'') ILIKE " + ph +
                 " OR COALESCE(pp.description,'') ILIKE " + ph +
                 " OR EXISTS (SELECT 1 FROM part_manufacturer_info mi"
                 "            WHERE mi.product_id=pp.id AND mi.part_number ILIKE " + ph + "))";
        }
        if (q.inStock) w += " AND pp.qty_available > 0";

        for (const auto& e : q.enums) {
            if (e.key == skipKey || e.values.empty()) continue;
            if (e.key == "mfr") {
                w += " AND EXISTS (SELECT 1 FROM part_manufacturer_info mi"
                     " JOIN res_partner rp ON rp.id=mi.manufacturer_id"
                     " WHERE mi.product_id=pp.id AND rp.name IN " + inList_(e.values, p, n) + ")";
            } else if (e.key == "pkg") {
                w += " AND EXISTS (SELECT 1 FROM part_footprint f"
                     " WHERE f.id=pp.footprint_id AND f.name IN " + inList_(e.values, p, n) + ")";
            } else {
                const std::string ph = "$" + std::to_string(++n);
                p.append(e.param);
                w += " AND EXISTS (SELECT 1 FROM part_parameter pa WHERE pa.product_id=pp.id"
                     " AND pa.name=" + ph + " AND pa.value_text IN " + inList_(e.values, p, n) + ")";
            }
        }
        for (const auto& r : q.ranges) {
            if (r.key == skipKey || (!r.hasMin && !r.hasMax)) continue;
            const std::string ph = "$" + std::to_string(++n);
            p.append(r.param);
            w += " AND EXISTS (SELECT 1 FROM part_parameter pa WHERE pa.product_id=pp.id"
                 " AND pa.name=" + ph + " AND pa.value_base IS NOT NULL";
            if (!r.kind.empty()) { w += " AND pa.quantity_kind=$" + std::to_string(++n); p.append(r.kind); }
            if (r.hasMin)        { w += " AND pa.value_base >= $"  + std::to_string(++n); p.append(r.lo); }
            if (r.hasMax)        { w += " AND pa.value_base <= $"  + std::to_string(++n); p.append(r.hi); }
            w += ")";
        }
        return w;
    }

    static pqxx::result run_(pqxx::work& txn, const std::string& sql, const pqxx::params& p, int n) {
        return n ? txn.exec(sql, p) : txn.exec(sql);
    }

    // ---- facets -------------------------------------------------------------
    nlohmann::json handleFacets(const core::CallKwArgs& call) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        Query q = parse_(call.arg(0));
        resolveRanges_(txn, q);

        nlohmann::json out;
        {
            pqxx::params p; int n = 0;
            const std::string sql = "SELECT count(*) FROM product_product pp" + where_(q, "", p, n);
            out["total"] = run_(txn, sql, p, n)[0][0].as<long>(0);
        }

        nlohmann::json facets = nlohmann::json::array();

        auto enumFacet = [&](const char* key, const char* label,
                             const std::string& joins, const std::string& labelExpr) {
            pqxx::params p; int n = 0;
            const std::string w = where_(q, key, p, n);
            const std::string sql = "SELECT " + labelExpr + " AS lbl, count(DISTINCT pp.id) AS cnt "
                                    "FROM product_product pp " + joins + w +
                                    " GROUP BY 1 ORDER BY cnt DESC, 1 LIMIT 300";
            auto res = run_(txn, sql, p, n);
            if (res.empty()) return;
            nlohmann::json vals = nlohmann::json::array();
            for (const auto& row : res)
                vals.push_back({{"v", row["lbl"].c_str()}, {"n", row["cnt"].as<long>(0)}});
            facets.push_back({{"key", key}, {"label", label}, {"kind", "enum"}, {"values", vals}});
        };

        enumFacet("mfr", "Manufacturer",
                  "JOIN part_manufacturer_info mi ON mi.product_id=pp.id "
                  "JOIN res_partner rp ON rp.id=mi.manufacturer_id",
                  "rp.name");
        enumFacet("pkg", "Package",
                  "JOIN part_footprint f ON f.id=pp.footprint_id",
                  "f.name");

        // Which parameters are worth a column, and is each enumerated or numeric?
        // Discovered against the *full* filter so the strip only offers attributes
        // that the current result set actually has.
        std::vector<std::tuple<std::string, long, long, std::string>> params;
        {
            pqxx::params p; int n = 0;
            const std::string w = where_(q, "", p, n);
            const std::string sql =
                "SELECT pa.name AS nm, "
                "       count(*) FILTER (WHERE pa.value_base IS NOT NULL) AS num_n, "
                "       count(*) FILTER (WHERE COALESCE(pa.value_text,'') <> '') AS txt_n, "
                "       COALESCE(max(pa.quantity_kind),'') AS kind, "
                "       count(DISTINCT pa.product_id) AS cover "
                "FROM product_product pp JOIN part_parameter pa ON pa.product_id=pp.id"
                + w + " GROUP BY 1 ORDER BY cover DESC, 1 LIMIT 20";
            for (const auto& row : run_(txn, sql, p, n))
                params.emplace_back(row["nm"].c_str(), row["num_n"].as<long>(0),
                                    row["txt_n"].as<long>(0), row["kind"].c_str());
        }

        for (const auto& [nm, numN, txtN, kind] : params) {
            const std::string key = "param:" + nm;
            // A parameter is a range only when it carries a quantity kind — i.e. a
            // real unit. Counting numerics alone would misread a text attribute:
            // seedPartUnits_ backfills value_base = value_numeric (0) for every
            // unitless parameter, so "Thick Film" would look perfectly numeric.
            if (!kind.empty() && numN > 0) {
                // Numeric: offer a min/max span plus the units of its kind.
                pqxx::params p; int n = 0;
                const std::string w = where_(q, key, p, n);
                const std::string ph = "$" + std::to_string(++n);
                p.append(nm);
                const std::string sql =
                    "SELECT min(pa.value_base) AS lo, max(pa.value_base) AS hi "
                    "FROM product_product pp JOIN part_parameter pa ON pa.product_id=pp.id"
                    + w + " AND pa.name=" + ph + " AND pa.value_base IS NOT NULL";
                auto res = txn.exec(sql, p);
                if (res.empty() || res[0]["lo"].is_null()) continue;

                nlohmann::json units = nlohmann::json::array();
                if (!kind.empty()) {
                    for (const auto& u : txn.exec(
                            "SELECT symbol, factor FROM part_unit WHERE quantity_kind=$1 ORDER BY factor",
                            pqxx::params{kind}))
                        units.push_back({{"symbol", u[0].c_str()}, {"factor", u[1].as<double>(1.0)}});
                }
                facets.push_back({{"key", key}, {"label", nm}, {"kind", "range"},
                                  {"quantity_kind", kind},
                                  {"min", res[0]["lo"].as<double>(0.0)},
                                  {"max", res[0]["hi"].as<double>(0.0)},
                                  {"units", units}});
            } else if (txtN > 0) {
                pqxx::params p; int n = 0;
                const std::string w = where_(q, key, p, n);
                const std::string ph = "$" + std::to_string(++n);
                p.append(nm);
                const std::string sql =
                    "SELECT pa.value_text AS lbl, count(DISTINCT pp.id) AS cnt "
                    "FROM product_product pp JOIN part_parameter pa ON pa.product_id=pp.id"
                    + w + " AND pa.name=" + ph + " AND COALESCE(pa.value_text,'') <> '' "
                    " GROUP BY 1 ORDER BY cnt DESC, 1 LIMIT 300";
                auto res = txn.exec(sql, p);
                if (res.empty()) continue;
                nlohmann::json vals = nlohmann::json::array();
                for (const auto& row : res)
                    vals.push_back({{"v", row["lbl"].c_str()}, {"n", row["cnt"].as<long>(0)}});
                facets.push_back({{"key", key}, {"label", nm}, {"kind", "enum"}, {"values", vals}});
            }
        }

        out["facets"] = facets;
        return out;
    }

    // ---- categories (the page's scope selector) -----------------------------
    nlohmann::json handleCategories(const core::CallKwArgs&) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        // The tree is small and the rollup is a parent-walk, so it is clearer —
        // and portable — to do it here than to nest a correlated recursive CTE.
        struct Node { std::string path; int parent = 0, depth = 1; long own = 0, total = 0; };
        std::map<int, Node> nodes;
        std::map<int, std::string> rawName;
        std::map<int, int> parentOf;
        for (const auto& row : txn.exec("SELECT id, name, parent_id FROM product_category")) {
            const int id = row[0].as<int>();
            rawName[id]  = row[1].c_str();
            parentOf[id] = row[2].is_null() ? 0 : row[2].as<int>(0);
        }
        for (const auto& row : txn.exec(
                "SELECT categ_id, count(*) FROM product_product WHERE active AND categ_id IS NOT NULL GROUP BY 1"))
            nodes[row[0].as<int>()].own = row[1].as<long>(0);

        for (const auto& [id, nm] : rawName) {
            Node& nd = nodes[id];
            nd.parent = parentOf.count(id) ? parentOf.at(id) : 0;
            // Build the path by walking up; a cycle would loop forever, so cap it.
            std::string path = nm;
            int depth = 1, cur = nd.parent, guard = 0;
            while (cur > 0 && rawName.count(cur) && ++guard < 32) {
                path = rawName.at(cur) + " / " + path;
                ++depth;
                cur = parentOf.count(cur) ? parentOf.at(cur) : 0;
            }
            nd.path = path;
            nd.depth = depth;
        }
        // Roll each category's own count up through its ancestors.
        for (const auto& [id, nd] : nodes) {
            if (nd.own <= 0) continue;
            nodes[id].total += nd.own;
            int cur = parentOf.count(id) ? parentOf.at(id) : 0, guard = 0;
            while (cur > 0 && nodes.count(cur) && ++guard < 32) {
                nodes[cur].total += nd.own;
                cur = parentOf.count(cur) ? parentOf.at(cur) : 0;
            }
        }

        // Only branches that actually hold parts — an empty one is a dead end.
        std::vector<std::pair<std::string, int>> ordered;
        for (const auto& [id, nd] : nodes)
            if (nd.total > 0 && !nd.path.empty()) ordered.emplace_back(nd.path, id);
        std::sort(ordered.begin(), ordered.end());

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& [path, id] : ordered)
            arr.push_back({{"id", id}, {"path", path},
                           {"depth", nodes[id].depth}, {"n", nodes[id].total}});
        return arr;
    }

    // ---- the result page ----------------------------------------------------
    nlohmann::json handleSearch(const core::CallKwArgs& call) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        const auto v = call.arg(0);
        Query q = parse_(v);
        resolveRanges_(txn, q);

        int limit = 25, offset = 0;
        if (v.is_object()) {
            if (v.contains("limit")  && v["limit"].is_number_integer())
                limit  = std::max(1, std::min(200, v["limit"].get<int>()));
            if (v.contains("offset") && v["offset"].is_number_integer())
                offset = std::max(0, v["offset"].get<int>());
        }
        // ORDER BY is chosen from a fixed map — never assembled from user text (S-49).
        static const std::map<std::string, std::string> kSorts = {
            {"name",  "pp.name"},
            {"code",  "COALESCE(pp.default_code,'')"},
            {"price", "pp.list_price"},
            {"stock", "pp.qty_available"},
        };
        std::string sortKey = "name", dir = "asc";
        if (v.is_object()) {
            if (v.contains("sort") && v["sort"].is_string() && kSorts.count(v["sort"].get<std::string>()))
                sortKey = v["sort"].get<std::string>();
            if (v.contains("dir") && v["dir"].is_string() && v["dir"].get<std::string>() == "desc")
                dir = "desc";
        }

        nlohmann::json out;
        {
            pqxx::params p; int n = 0;
            const std::string sql = "SELECT count(*) FROM product_product pp" + where_(q, "", p, n);
            out["total"] = run_(txn, sql, p, n)[0][0].as<long>(0);
        }

        pqxx::params p; int n = 0;
        const std::string w = where_(q, "", p, n);
        const std::string sql =
            "SELECT pp.id, pp.name, COALESCE(pp.default_code,'') AS code, "
            "       COALESCE(pp.description,'') AS descr, "
            "       pp.qty_available, pp.list_price, "
            "       COALESCE(c.name,'') AS categ, COALESCE(f.name,'') AS pkg, "
            "       COALESCE((SELECT rp.name FROM part_manufacturer_info mi "
            "                  JOIN res_partner rp ON rp.id=mi.manufacturer_id "
            "                 WHERE mi.product_id=pp.id ORDER BY mi.id LIMIT 1),'') AS mfr, "
            "       COALESCE((SELECT mi.part_number FROM part_manufacturer_info mi "
            "                 WHERE mi.product_id=pp.id ORDER BY mi.id LIMIT 1),'') AS mpn "
            "FROM product_product pp "
            "LEFT JOIN product_category c ON c.id=pp.categ_id "
            "LEFT JOIN part_footprint f ON f.id=pp.footprint_id"
            + w + " ORDER BY " + kSorts.at(sortKey) + " " + dir + ", pp.id "
            " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);

        nlohmann::json rows = nlohmann::json::array();
        std::vector<int> ids;
        for (const auto& row : run_(txn, sql, p, n)) {
            const int id = row["id"].as<int>();
            ids.push_back(id);
            rows.push_back({{"id", id},
                            {"name", row["name"].c_str()},
                            {"code", row["code"].c_str()},
                            {"description", row["descr"].c_str()},
                            // bigint micros in the column, a plain number on the wire
                            {"qty_available", row["qty_available"].as<long long>(0) / 1000000.0},
                            {"list_price",    row["list_price"].as<long long>(0)    / 1000000.0},
                            {"categ", row["categ"].c_str()},
                            {"package", row["pkg"].c_str()},
                            {"manufacturer", row["mfr"].c_str()},
                            {"mpn", row["mpn"].c_str()},
                            {"params", nlohmann::json::array()}});
        }

        // One extra round trip for the parameters of just this page of rows.
        if (!ids.empty()) {
            std::string idList;
            for (size_t i = 0; i < ids.size(); ++i) {
                if (i) idList += ",";
                idList += std::to_string(ids[i]);   // ints from our own SELECT
            }
            std::map<int, size_t> pos;
            for (size_t i = 0; i < ids.size(); ++i) pos[ids[i]] = i;
            for (const auto& row : txn.exec(
                    "SELECT pa.product_id, pa.name, COALESCE(pa.value_text,'') AS txt, "
                    "       pa.value_numeric, COALESCE(u.symbol,'') AS sym "
                    "FROM part_parameter pa LEFT JOIN part_unit u ON u.id=pa.unit_id "
                    "WHERE pa.product_id IN (" + idList + ") ORDER BY pa.product_id, pa.name")) {
                const int pid = row["product_id"].as<int>();
                auto it = pos.find(pid);
                if (it == pos.end()) continue;
                const std::string txt = row["txt"].c_str();
                const std::string sym = row["sym"].c_str();
                std::string shown = txt;
                if (shown.empty()) {
                    std::ostringstream os;
                    os << row["value_numeric"].as<double>(0.0);
                    shown = os.str();
                    if (!sym.empty()) shown += " " + sym;
                }
                rows[it->second]["params"].push_back({{"name", row["name"].c_str()}, {"value", shown}});
            }
        }

        out["rows"]   = rows;
        out["limit"]  = limit;
        out["offset"] = offset;
        return out;
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

// ================================================================
// docs/096 — templates, attributes, variants
// ================================================================

class ProductTemplate : public BaseModel<ProductTemplate> {
public:
    static constexpr const char* MODEL_NAME = "product.template";
    static constexpr const char* TABLE_NAME = "product_template";
    explicit ProductTemplate(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    std::string name, defaultCode, description, descriptionSale, descriptionPurchase;
    std::string type = "product", tracking = "none";
    std::string invoicePolicy = "order", purchaseMethod = "purchase";
    std::string image1920;
    int    categId = 0, uomId = 1, uomPoId = 1, companyId = 0;
    int    incomeAccountId = 0, expenseAccountId = 0;
    double listPrice = 0.0, standardPrice = 0.0;
    bool   saleOk = true, purchaseOk = true, active = true;

    void registerFields() {
        fieldRegistry_.add({"name",            FieldType::Char,    "Product Name", true});
        fieldRegistry_.add({"default_code",    FieldType::Char,    "Internal Reference"});
        fieldRegistry_.add({"description",     FieldType::Text,    "Description"});
        fieldRegistry_.add({"description_sale",FieldType::Text,    "Sales Description"});
        fieldRegistry_.add({"description_purchase", FieldType::Text, "Purchase Description"});
        fieldRegistry_.add({"type",            FieldType::Char,    "Product Type"});
        fieldRegistry_.add({"categ_id",        FieldType::Many2one,"Category", false, false, true, true, "product.category"});
        fieldRegistry_.add({"uom_id",          FieldType::Many2one,"Unit of Measure", false, false, true, true, "uom.uom"});
        fieldRegistry_.add({"uom_po_id",       FieldType::Many2one,"Purchase UoM", false, false, true, true, "uom.uom"});
        FieldDef lp{"list_price", FieldType::Monetary, "Sales Price"};        lp.scaled = true;
        FieldDef sp{"standard_price", FieldType::Monetary, "Cost"};           sp.scaled = true;
        fieldRegistry_.add(lp); fieldRegistry_.add(sp);
        fieldRegistry_.add({"tracking",        FieldType::Char,    "Tracking"});
        fieldRegistry_.add({"invoice_policy",  FieldType::Char,    "Invoicing Policy"});
        fieldRegistry_.add({"purchase_method", FieldType::Char,    "Bill Control"});
        fieldRegistry_.add({"income_account_id",  FieldType::Many2one, "Income Account", false, false, true, true, "account.account"});
        fieldRegistry_.add({"expense_account_id", FieldType::Many2one, "Expense Account", false, false, true, true, "account.account"});
        fieldRegistry_.add({"sale_ok",         FieldType::Boolean, "Can be Sold"});
        fieldRegistry_.add({"purchase_ok",     FieldType::Boolean, "Can be Purchased"});
        fieldRegistry_.add({"image_1920",      FieldType::Text,    "Image"});
        fieldRegistry_.add({"company_id",      FieldType::Many2one,"Company", false, false, true, true, "res.company"});
        fieldRegistry_.add({"active",          FieldType::Boolean, "Active"});
    }
    void serializeFields(nlohmann::json& j) const {
        j["name"]=name; j["default_code"]=defaultCode; j["description"]=description;
        j["description_sale"]=descriptionSale; j["description_purchase"]=descriptionPurchase;
        j["type"]=type; j["categ_id"]=categId; j["uom_id"]=uomId; j["uom_po_id"]=uomPoId;
        j["list_price"]=listPrice; j["standard_price"]=standardPrice; j["tracking"]=tracking;
        j["invoice_policy"]=invoicePolicy; j["purchase_method"]=purchaseMethod;
        j["income_account_id"]=incomeAccountId; j["expense_account_id"]=expenseAccountId;
        j["sale_ok"]=saleOk; j["purchase_ok"]=purchaseOk; j["image_1920"]=image1920;
        j["company_id"]=companyId; j["active"]=active;
    }
    void deserializeFields(const nlohmann::json& j) {
        auto S=[&](const char* k, std::string& v){ if(j.contains(k)&&j[k].is_string()) v=j[k].get<std::string>(); };
        auto I=[&](const char* k, int& v){ if(j.contains(k)&&j[k].is_number_integer()) v=j[k].get<int>();
                                            else if(j.contains(k)&&j[k].is_array()&&!j[k].empty()&&j[k][0].is_number_integer()) v=j[k][0].get<int>(); };
        auto D=[&](const char* k, double& v){ if(j.contains(k)&&j[k].is_number()) v=j[k].get<double>(); };
        auto B=[&](const char* k, bool& v){ if(j.contains(k)&&j[k].is_boolean()) v=j[k].get<bool>(); };
        S("name",name); S("default_code",defaultCode); S("description",description);
        S("description_sale",descriptionSale); S("description_purchase",descriptionPurchase);
        S("type",type); S("tracking",tracking); S("invoice_policy",invoicePolicy);
        S("purchase_method",purchaseMethod); S("image_1920",image1920);
        I("categ_id",categId); I("uom_id",uomId); I("uom_po_id",uomPoId); I("company_id",companyId);
        I("income_account_id",incomeAccountId); I("expense_account_id",expenseAccountId);
        D("list_price",listPrice); D("standard_price",standardPrice);
        B("sale_ok",saleOk); B("purchase_ok",purchaseOk); B("active",active);
    }
    std::vector<std::string> validate() const {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Product name is required.");
        return e;
    }
};

class ProductAttribute : public BaseModel<ProductAttribute> {
public:
    static constexpr const char* MODEL_NAME = "product.attribute";
    static constexpr const char* TABLE_NAME = "product_attribute";
    explicit ProductAttribute(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}
    std::string name; int sequence = 10;
    void registerFields() {
        fieldRegistry_.add({"name",     FieldType::Char,    "Attribute", true});
        fieldRegistry_.add({"sequence", FieldType::Integer, "Sequence"});
    }
    void serializeFields(nlohmann::json& j) const { j["name"]=name; j["sequence"]=sequence; }
    void deserializeFields(const nlohmann::json& j) {
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("sequence") && j["sequence"].is_number_integer()) sequence = j["sequence"].get<int>();
    }
    std::vector<std::string> validate() const {
        if (name.empty()) return {"Attribute name is required."};
        return {};
    }
};

class ProductAttributeValue : public BaseModel<ProductAttributeValue> {
public:
    static constexpr const char* MODEL_NAME = "product.attribute.value";
    static constexpr const char* TABLE_NAME = "product_attribute_value";
    explicit ProductAttributeValue(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}
    std::string name; int attributeId = 0, sequence = 10;
    void registerFields() {
        fieldRegistry_.add({"attribute_id", FieldType::Many2one, "Attribute", true, false, true, true, "product.attribute"});
        fieldRegistry_.add({"name",         FieldType::Char,     "Value", true});
        fieldRegistry_.add({"sequence",     FieldType::Integer,  "Sequence"});
    }
    void serializeFields(nlohmann::json& j) const {
        j["attribute_id"]=attributeId; j["name"]=name; j["sequence"]=sequence;
    }
    void deserializeFields(const nlohmann::json& j) {
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("sequence") && j["sequence"].is_number_integer()) sequence = j["sequence"].get<int>();
        if (j.contains("attribute_id")) {
            if (j["attribute_id"].is_number_integer()) attributeId = j["attribute_id"].get<int>();
            else if (j["attribute_id"].is_array() && !j["attribute_id"].empty() && j["attribute_id"][0].is_number_integer())
                attributeId = j["attribute_id"][0].get<int>();
        }
    }
    std::vector<std::string> validate() const {
        std::vector<std::string> e;
        if (name.empty())     e.push_back("Value name is required.");
        if (attributeId <= 0) e.push_back("An attribute is required.");
        return e;
    }
};

/**
 * docs/096 — product.template, plus the attribute lines and variant generation.
 *
 * Generation is the whole point of the feature, and its contract is:
 *
 *   * every combination of the chosen attribute values gets exactly one
 *     variant, and running it twice creates nothing new — the combination, not
 *     the name, is the identity;
 *   * a variant whose combination is no longer possible is ARCHIVED, never
 *     deleted. Stock moves, invoice lines and BoMs point at product_product
 *     rows; deleting one would either fail on a foreign key or, worse, cascade
 *     into accounting history;
 *   * a template with no attributes still has exactly one variant, so a plain
 *     product behaves as it always did.
 */
class ProductTemplateViewModel : public core::GenericViewModel<ProductTemplate> {
public:
    explicit ProductTemplateViewModel(std::shared_ptr<DbConnection> db)
        : core::GenericViewModel<ProductTemplate>(db), db2_(std::move(db))
    {
        REGISTER_METHOD("read_attribute_lines", handleReadLines)
        REGISTER_MUTATOR("set_attribute_line",   handleSetLine)
        REGISTER_MUTATOR("remove_attribute_line",handleRemoveLine)
        REGISTER_MUTATOR("generate_variants",    handleGenerate)
        REGISTER_METHOD("read_variants",         handleReadVariants)
    }

private:
    std::shared_ptr<DbConnection> db2_;

    static int argId(const core::CallKwArgs& call, const char* key) {
        const auto v = call.arg(0);
        if (v.is_object() && v.contains(key)) {
            const auto& x = v[key];
            if (x.is_number_integer()) return x.get<int>();
            if (x.is_array() && !x.empty() && x[0].is_number_integer()) return x[0].get<int>();
        }
        if (!call.ids().empty()) return call.ids().front();
        return 0;
    }

    nlohmann::json handleReadLines(const core::CallKwArgs& call) {
        const int tmpl = argId(call, "product_tmpl_id");
        if (tmpl <= 0) throw infrastructure::ValidationError("A product template is required.");
        auto conn = db2_->acquire(); pqxx::work txn{conn.get()};
        nlohmann::json out = nlohmann::json::array();
        for (const auto& l : txn.exec(
                 "SELECT l.id, l.attribute_id, a.name FROM product_template_attribute_line l "
                 "JOIN product_attribute a ON a.id=l.attribute_id "
                 "WHERE l.product_tmpl_id=$1 ORDER BY l.sequence, l.id", pqxx::params{tmpl})) {
            nlohmann::json line{{"id", l[0].as<int>()}, {"attribute_id", l[1].as<int>()},
                                {"attribute", l[2].c_str()}};
            nlohmann::json vals = nlohmann::json::array();
            for (const auto& v : txn.exec(
                     "SELECT tv.id, tv.value_id, av.name, tv.price_extra "
                     "FROM product_template_attribute_value tv "
                     "JOIN product_attribute_value av ON av.id=tv.value_id "
                     "WHERE tv.line_id=$1 AND tv.active ORDER BY av.sequence, av.id", pqxx::params{l[0].as<int>()}))
                vals.push_back({{"id", v[0].as<int>()}, {"value_id", v[1].as<int>()},
                                {"name", v[2].c_str()},
                                {"price_extra", core::Money::fromMicros(v[3].as<long long>(0)).toJson()}});
            line["values"] = std::move(vals);
            out.push_back(std::move(line));
        }
        return out;
    }

    /// Create or replace one attribute line: which values apply, and their extras.
    nlohmann::json handleSetLine(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw infrastructure::ValidationError("set_attribute_line needs a dict.");
        const int tmpl = argId(call, "product_tmpl_id");
        int attr = 0;
        if (v.contains("attribute_id")) {
            if (v["attribute_id"].is_number_integer()) attr = v["attribute_id"].get<int>();
            else if (v["attribute_id"].is_array() && !v["attribute_id"].empty())
                attr = v["attribute_id"][0].get<int>();
        }
        if (tmpl <= 0 || attr <= 0)
            throw infrastructure::ValidationError("A template and an attribute are required.");

        auto conn = db2_->acquire(); pqxx::work txn{conn.get()};
        auto ex = txn.exec("SELECT id FROM product_template_attribute_line "
                           "WHERE product_tmpl_id=$1 AND attribute_id=$2",
                           pqxx::params{tmpl, attr});
        int lineId;
        if (ex.empty())
            lineId = txn.exec("INSERT INTO product_template_attribute_line (product_tmpl_id, attribute_id) "
                              "VALUES ($1,$2) RETURNING id", pqxx::params{tmpl, attr})[0][0].as<int>();
        else lineId = ex[0][0].as<int>();

        // Replace the value set wholesale, but keep the ids of values that stay,
        // because variants reference them — deleting and re-inserting an
        // unchanged value would orphan every variant that uses it.
        std::set<int> keep;
        if (v.contains("values") && v["values"].is_array()) {
            for (const auto& item : v["values"]) {
                int valueId = 0; long long extra = 0;
                if (item.is_number_integer()) valueId = item.get<int>();
                else if (item.is_object()) {
                    if (item.contains("value_id") && item["value_id"].is_number_integer())
                        valueId = item["value_id"].get<int>();
                    if (item.contains("price_extra") && item["price_extra"].is_number())
                        extra = core::Money::fromJson(item["price_extra"].get<double>()).micros();
                }
                if (valueId <= 0) continue;
                auto e2 = txn.exec("SELECT id FROM product_template_attribute_value "
                                   "WHERE line_id=$1 AND value_id=$2", pqxx::params{lineId, valueId});
                if (e2.empty())
                    keep.insert(txn.exec("INSERT INTO product_template_attribute_value (line_id, value_id, price_extra) "
                                         "VALUES ($1,$2,$3) RETURNING id",
                                         pqxx::params{lineId, valueId, extra})[0][0].as<int>());
                else {
                    // Reviving a previously-dropped value REUSES its id, which is
                    // what lets an archived variant come back as itself rather
                    // than as a duplicate.
                    txn.exec("UPDATE product_template_attribute_value "
                             "SET price_extra=$1, active=TRUE WHERE id=$2",
                             pqxx::params{extra, e2[0][0].as<int>()});
                    keep.insert(e2[0][0].as<int>());
                }
            }
        }
        std::string keepList;
        for (int k : keep) { if (!keepList.empty()) keepList += ","; keepList += std::to_string(k); }
        // Deactivate rather than delete — see the schema comment.
        txn.exec("UPDATE product_template_attribute_value SET active=FALSE WHERE line_id=" +
                 std::to_string(lineId) +
                 (keepList.empty() ? "" : " AND id NOT IN (" + keepList + ")"));
        txn.commit();
        return {{"ok", true}, {"line_id", lineId}};
    }

    nlohmann::json handleRemoveLine(const core::CallKwArgs& call) {
        const int lineId = argId(call, "line_id");
        if (lineId <= 0) throw infrastructure::ValidationError("A line id is required.");
        auto conn = db2_->acquire(); pqxx::work txn{conn.get()};
        txn.exec("DELETE FROM product_template_attribute_line WHERE id=$1", pqxx::params{lineId});
        txn.commit();
        return {{"ok", true}};
    }

    nlohmann::json handleReadVariants(const core::CallKwArgs& call) {
        const int tmpl = argId(call, "product_tmpl_id");
        if (tmpl <= 0) throw infrastructure::ValidationError("A product template is required.");
        auto conn = db2_->acquire(); pqxx::work txn{conn.get()};
        nlohmann::json out = nlohmann::json::array();
        for (const auto& p : txn.exec(
                 "SELECT p.id, p.name, p.default_code, p.list_price, p.active, "
                 "  COALESCE(string_agg(av.name, ' / ' ORDER BY av.id), '') AS combo "
                 "FROM product_product p "
                 "LEFT JOIN product_variant_combination c ON c.product_id=p.id "
                 "LEFT JOIN product_template_attribute_value tv ON tv.id=c.ptav_id "
                 "LEFT JOIN product_attribute_value av ON av.id=tv.value_id "
                 "WHERE p.product_tmpl_id=$1 "
                 "GROUP BY p.id, p.name, p.default_code, p.list_price, p.active "
                 "ORDER BY p.id", pqxx::params{tmpl}))
            out.push_back({{"id", p[0].as<int>()}, {"name", p[1].is_null() ? "" : p[1].c_str()},
                           {"default_code", p[2].is_null() ? "" : p[2].c_str()},
                           {"list_price", core::Money::fromMicros(p[3].as<long long>(0)).toJson()},
                           {"active", p[4].as<bool>(true)},
                           {"combination", p[5].is_null() ? "" : p[5].c_str()}});
        return out;
    }

    nlohmann::json handleGenerate(const core::CallKwArgs& call) {
        const int tmpl = argId(call, "product_tmpl_id");
        if (tmpl <= 0) throw infrastructure::ValidationError("A product template is required.");

        auto conn = db2_->acquire(); pqxx::work txn{conn.get()};
        auto t = txn.exec(
            "SELECT name, default_code, description, type, categ_id, uom_id, uom_po_id, "
            "       list_price, standard_price, tracking, invoice_policy, purchase_method, "
            "       income_account_id, expense_account_id, sale_ok, purchase_ok, company_id "
            "FROM product_template WHERE id=$1", pqxx::params{tmpl});
        if (t.empty()) throw infrastructure::ValidationError("No such product template.");
        const auto& T = t[0];
        const std::string baseName = T[0].is_null() ? "Unnamed" : T[0].c_str();
        const long long   basePrice = T[7].as<long long>(0);

        // One vector of (ptav_id, value name, price_extra) per attribute line.
        struct Val { int ptav; std::string name; long long extra; };
        std::vector<std::vector<Val>> axes;
        for (const auto& l : txn.exec("SELECT id FROM product_template_attribute_line "
                                      "WHERE product_tmpl_id=$1 ORDER BY sequence, id",
                                      pqxx::params{tmpl})) {
            std::vector<Val> vals;
            for (const auto& v : txn.exec(
                     "SELECT tv.id, av.name, tv.price_extra FROM product_template_attribute_value tv "
                     "JOIN product_attribute_value av ON av.id=tv.value_id "
                     "WHERE tv.line_id=$1 AND tv.active ORDER BY av.sequence, av.id",
                     pqxx::params{l[0].as<int>()}))
                vals.push_back({v[0].as<int>(), v[1].c_str(), v[2].as<long long>(0)});
            // An attribute with no values selected would multiply the whole
            // cartesian product by zero and wipe out every variant. Skip it.
            if (!vals.empty()) axes.push_back(std::move(vals));
        }

        // Cartesian product of the axes; empty axes give one empty combination,
        // which is exactly the "plain product, one variant" case.
        std::vector<std::vector<Val>> combos{{}};
        for (const auto& axis : axes) {
            std::vector<std::vector<Val>> next;
            for (const auto& sofar : combos)
                for (const auto& v : axis) { auto c = sofar; c.push_back(v); next.push_back(std::move(c)); }
            combos.swap(next);
        }

        // Existing variants, keyed by their combination, so re-running is a no-op.
        std::map<std::string, int> existing;
        for (const auto& p : txn.exec(
                 "SELECT p.id, COALESCE(string_agg(c.ptav_id::text, ',' ORDER BY c.ptav_id), '') "
                 "FROM product_product p LEFT JOIN product_variant_combination c ON c.product_id=p.id "
                 "WHERE p.product_tmpl_id=$1 GROUP BY p.id", pqxx::params{tmpl}))
            existing[p[1].is_null() ? "" : p[1].c_str()] = p[0].as<int>();

        int created = 0, reactivated = 0, archived = 0;
        std::set<int> live;
        for (const auto& combo : combos) {
            std::vector<int> ids;
            std::string suffix;
            long long extra = 0;
            for (const auto& v : combo) {
                ids.push_back(v.ptav);
                extra += v.extra;
                if (!suffix.empty()) suffix += ", ";
                suffix += v.name;
            }
            std::sort(ids.begin(), ids.end());
            std::string key;
            for (int id : ids) { if (!key.empty()) key += ","; key += std::to_string(id); }

            auto it = existing.find(key);
            if (it != existing.end()) {
                live.insert(it->second);
                // Price follows the template plus this combination's extras.
                txn.exec("UPDATE product_product SET list_price=$1, active=TRUE WHERE id=$2",
                         pqxx::params{basePrice + extra, it->second});
                ++reactivated;
                continue;
            }
            const std::string vname = suffix.empty() ? baseName : (baseName + " (" + suffix + ")");
            auto ins = txn.exec(
                "INSERT INTO product_product (name, default_code, description, type, categ_id, "
                "  uom_id, uom_po_id, list_price, standard_price, tracking, invoice_policy, "
                "  purchase_method, income_account_id, expense_account_id, sale_ok, purchase_ok, "
                "  company_id, product_tmpl_id, active) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,TRUE) RETURNING id",
                pqxx::params{vname, T[1].is_null() ? nullptr : T[1].c_str(),
                             T[2].is_null() ? nullptr : T[2].c_str(),
                             T[3].is_null() ? std::string("product") : std::string(T[3].c_str()),
                             T[4].is_null() ? nullptr : T[4].c_str(),
                             T[5].is_null() ? nullptr : T[5].c_str(),
                             T[6].is_null() ? nullptr : T[6].c_str(),
                             basePrice + extra, T[8].as<long long>(0),
                             T[9].is_null() ? std::string("none") : std::string(T[9].c_str()),
                             T[10].is_null() ? std::string("order") : std::string(T[10].c_str()),
                             T[11].is_null() ? std::string("purchase") : std::string(T[11].c_str()),
                             T[12].is_null() ? nullptr : T[12].c_str(),
                             T[13].is_null() ? nullptr : T[13].c_str(),
                             T[14].as<bool>(true), T[15].as<bool>(true),
                             T[16].is_null() ? nullptr : T[16].c_str(), tmpl});
            const int pid = ins[0][0].as<int>();
            for (int ptav : ids)
                txn.exec("INSERT INTO product_variant_combination (product_id, ptav_id) VALUES ($1,$2) "
                         "ON CONFLICT DO NOTHING", pqxx::params{pid, ptav});
            live.insert(pid);
            ++created;
        }

        // Archive, never delete: these ids are referenced by stock moves and
        // invoice lines, and history must not move under them.
        for (const auto& [key, pid] : existing) {
            if (live.count(pid)) continue;
            txn.exec("UPDATE product_product SET active=FALSE WHERE id=$1", pqxx::params{pid});
            ++archived;
        }
        txn.commit();

        LOG_INFO << "[product] template " << tmpl << ": " << created << " created, "
                 << reactivated << " kept, " << archived << " archived";
        return {{"ok", true}, {"created", created}, {"kept", reactivated},
                {"archived", archived}, {"variants", (int)combos.size()}};
    }
};

// ================================================================
// docs/097 — SI value parsing
// ================================================================
/**
 * @brief Parse the way component values are actually written down.
 *
 * Accepts `4700`, `4.7k`, `4k7`, `100n`, `2R2`, `1M5`, `10 uF`, `4.7kΩ`.
 *
 * The `4k7` form — prefix used as the decimal point — is the one that matters:
 * it is what appears on schematics, in BOMs and in every distributor listing,
 * precisely because it survives a photocopier when a `.` does not. A parser
 * that only handles `4.7k` rejects half of real input.
 *
 * `R` is the resistance stand-in for the same reason (`2R2` = 2.2 Ω).
 *
 * @param text   the written value
 * @param mult   out: the SI multiplier implied by the prefix (1 when none)
 * @param number out: the numeric part, prefix applied
 * @returns false when there is no number at all
 */
static bool parseSiValue(const std::string& text, double& number, double& mult) {
    static const std::map<char, double> kPrefix = {
        {'p',1e-12},{'n',1e-9},{'u',1e-6},{'m',1e-3},
        {'k',1e3},{'K',1e3},{'M',1e6},{'G',1e9},{'T',1e12},
        {'R',1.0},   // 2R2 = 2.2 ohm
    };
    std::string s;
    for (char c : text) if (!std::isspace(static_cast<unsigned char>(c))) s += c;
    if (s.empty()) return false;
    // "µ" is two bytes in UTF-8; normalise it to 'u' before scanning.
    size_t mu;
    while ((mu = s.find("\xc2\xb5")) != std::string::npos) s.replace(mu, 2, "u");

    // Find a prefix letter that sits between digits (4k7) or after them (4.7k).
    std::size_t pi = std::string::npos;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (kPrefix.count(s[i]) && i > 0 &&
            (std::isdigit(static_cast<unsigned char>(s[i-1])) || s[i-1] == '.')) { pi = i; break; }
    }

    try {
        if (pi == std::string::npos) {
            mult = 1.0;
            number = std::stod(s);           // trailing unit text is ignored by stod
            return true;
        }
        mult = kPrefix.at(s[pi]);
        const std::string head = s.substr(0, pi);
        std::string tail;
        for (std::size_t i = pi + 1; i < s.size(); ++i) {
            if (std::isdigit(static_cast<unsigned char>(s[i]))) tail += s[i];
            else break;                       // stop at the unit symbol
        }
        // 4k7 -> 4.7 ; 4.7k -> 4.7 ; 4k -> 4
        const double whole = std::stod(head);
        double v = whole;
        if (!tail.empty() && head.find('.') == std::string::npos)
            v = std::stod(head + "." + tail);
        number = v * mult;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// ================================================================
// docs/096 — pricelists
// ================================================================

class ProductPricelist : public BaseModel<ProductPricelist> {
public:
    static constexpr const char* MODEL_NAME = "product.pricelist";
    static constexpr const char* TABLE_NAME = "product_pricelist";
    explicit ProductPricelist(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}
    std::string name; int currencyId = 0, companyId = 0, sequence = 10; bool active = true;
    void registerFields() {
        fieldRegistry_.add({"name",        FieldType::Char,    "Pricelist", true});
        fieldRegistry_.add({"currency_id", FieldType::Many2one,"Currency", false, false, true, true, "res.currency"});
        fieldRegistry_.add({"company_id",  FieldType::Many2one,"Company",  false, false, true, true, "res.company"});
        fieldRegistry_.add({"sequence",    FieldType::Integer, "Sequence"});
        fieldRegistry_.add({"active",      FieldType::Boolean, "Active"});
    }
    void serializeFields(nlohmann::json& j) const {
        j["name"]=name; j["currency_id"]=currencyId; j["company_id"]=companyId;
        j["sequence"]=sequence; j["active"]=active;
    }
    void deserializeFields(const nlohmann::json& j) {
        if (j.contains("name") && j["name"].is_string()) name=j["name"].get<std::string>();
        auto I=[&](const char* k,int& v){ if(j.contains(k)&&j[k].is_number_integer()) v=j[k].get<int>();
            else if(j.contains(k)&&j[k].is_array()&&!j[k].empty()&&j[k][0].is_number_integer()) v=j[k][0].get<int>(); };
        I("currency_id",currencyId); I("company_id",companyId); I("sequence",sequence);
        if (j.contains("active") && j["active"].is_boolean()) active=j["active"].get<bool>();
    }
    std::vector<std::string> validate() const {
        if (name.empty()) return {"Pricelist name is required."};
        return {};
    }
};

class ProductPricelistItem : public BaseModel<ProductPricelistItem> {
public:
    static constexpr const char* MODEL_NAME = "product.pricelist.item";
    static constexpr const char* TABLE_NAME = "product_pricelist_item";
    explicit ProductPricelistItem(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}
    std::string appliedOn = "3_global", computePrice = "fixed", base = "list_price";
    std::string dateStart, dateEnd;
    int    pricelistId = 0, productId = 0, productTmplId = 0, categId = 0, sequence = 10;
    double minQuantity = 0.0, fixedPrice = 0.0, percentPrice = 0.0;
    double priceDiscount = 0.0, priceSurcharge = 0.0;
    void registerFields() {
        fieldRegistry_.add({"pricelist_id",  FieldType::Many2one,"Pricelist", true, false, true, true, "product.pricelist"});
        fieldRegistry_.add({"applied_on",    FieldType::Char,    "Applied On"});
        fieldRegistry_.add({"product_id",    FieldType::Many2one,"Variant", false,false,true,true,"product.product"});
        fieldRegistry_.add({"product_tmpl_id",FieldType::Many2one,"Product", false,false,true,true,"product.template"});
        fieldRegistry_.add({"categ_id",      FieldType::Many2one,"Category", false,false,true,true,"product.category"});
        FieldDef mq{"min_quantity", FieldType::Float, "Min. Quantity"};       mq.scaled = true;
        FieldDef fp{"fixed_price",  FieldType::Monetary, "Fixed Price"};      fp.scaled = true;
        FieldDef pp{"percent_price",FieldType::Float, "Percentage"};          pp.scaled = true;
        FieldDef pd{"price_discount",FieldType::Float, "Discount %"};         pd.scaled = true;
        FieldDef ps{"price_surcharge",FieldType::Monetary, "Surcharge"};      ps.scaled = true;
        fieldRegistry_.add(mq); fieldRegistry_.add(fp); fieldRegistry_.add(pp);
        fieldRegistry_.add(pd); fieldRegistry_.add(ps);
        fieldRegistry_.add({"date_start",    FieldType::Date,    "Start Date"});
        fieldRegistry_.add({"date_end",      FieldType::Date,    "End Date"});
        fieldRegistry_.add({"compute_price", FieldType::Char,    "Compute Price"});
        fieldRegistry_.add({"base",          FieldType::Char,    "Based On"});
        fieldRegistry_.add({"sequence",      FieldType::Integer, "Sequence"});
    }
    void serializeFields(nlohmann::json& j) const {
        j["pricelist_id"]=pricelistId; j["applied_on"]=appliedOn; j["product_id"]=productId;
        j["product_tmpl_id"]=productTmplId; j["categ_id"]=categId; j["min_quantity"]=minQuantity;
        j["fixed_price"]=fixedPrice; j["percent_price"]=percentPrice;
        j["price_discount"]=priceDiscount; j["price_surcharge"]=priceSurcharge;
        // An unset date must go to SQL as NULL, not "". A DATE column rejects
        // the empty string outright, which failed every create that left the
        // promotion window blank — i.e. almost all of them.
        j["date_start"] = dateStart.empty() ? nlohmann::json(nullptr) : nlohmann::json(dateStart);
        j["date_end"]   = dateEnd.empty()   ? nlohmann::json(nullptr) : nlohmann::json(dateEnd);
        j["compute_price"]=computePrice; j["base"]=base; j["sequence"]=sequence;
    }
    void deserializeFields(const nlohmann::json& j) {
        auto S=[&](const char* k,std::string& v){ if(j.contains(k)&&j[k].is_string()) v=j[k].get<std::string>(); };
        auto I=[&](const char* k,int& v){ if(j.contains(k)&&j[k].is_number_integer()) v=j[k].get<int>();
            else if(j.contains(k)&&j[k].is_array()&&!j[k].empty()&&j[k][0].is_number_integer()) v=j[k][0].get<int>(); };
        auto D=[&](const char* k,double& v){ if(j.contains(k)&&j[k].is_number()) v=j[k].get<double>(); };
        S("applied_on",appliedOn); S("compute_price",computePrice); S("base",base);
        S("date_start",dateStart); S("date_end",dateEnd);
        I("pricelist_id",pricelistId); I("product_id",productId); I("product_tmpl_id",productTmplId);
        I("categ_id",categId); I("sequence",sequence);
        D("min_quantity",minQuantity); D("fixed_price",fixedPrice); D("percent_price",percentPrice);
        D("price_discount",priceDiscount); D("price_surcharge",priceSurcharge);
    }
    std::vector<std::string> validate() const {
        std::vector<std::string> e;
        if (pricelistId <= 0) e.push_back("A pricelist is required.");
        return e;
    }
};

/**
 * docs/096 — resolving a price.
 *
 * Rules are ordered NARROWEST FIRST: a rule for this exact variant beats one
 * for its product, which beats one for its category, which beats a global
 * rule. Within the same specificity, `sequence` then id decide. The first
 * match wins — quantity breaks work because a min_quantity rule that does not
 * apply simply is not a match.
 *
 * All money is micro-units end to end; the only division is the percentage,
 * and it is done on int64 to avoid a float creeping into a price.
 */
class ProductPricelistViewModel : public core::GenericViewModel<ProductPricelist> {
public:
    explicit ProductPricelistViewModel(std::shared_ptr<DbConnection> db)
        : core::GenericViewModel<ProductPricelist>(db), db2_(std::move(db))
    {
        REGISTER_METHOD("get_price",       handleGetPrice)
        REGISTER_METHOD("price_breakdown", handleBreakdown)
    }

    /// Shared with the sale module: the unit price for (list, product, qty, date).
    static long long resolve(pqxx::transaction_base& txn, int pricelistId,
                             int productId, long long qtyMicros,
                             const std::string& date, std::string* why = nullptr) {
        auto p = txn.exec("SELECT list_price, standard_price, categ_id, product_tmpl_id "
                          "FROM product_product WHERE id=$1", pqxx::params{productId});
        if (p.empty()) throw infrastructure::ValidationError("No such product.");
        const long long listPrice = p[0][0].as<long long>(0);
        const long long costPrice = p[0][1].as<long long>(0);

        if (pricelistId <= 0) { if (why) *why = "no pricelist: list price"; return listPrice; }

        auto rows = txn.exec(
            "SELECT id, applied_on, compute_price, base, fixed_price, percent_price, "
            "       price_discount, price_surcharge "
            "FROM product_pricelist_item i "
            "WHERE i.pricelist_id = $1 "
            "  AND i.min_quantity <= $2 "
            "  AND (i.date_start IS NULL OR i.date_start <= $3::date) "
            "  AND (i.date_end   IS NULL OR i.date_end   >= $3::date) "
            "  AND ( (i.applied_on='0_product_variant'  AND i.product_id      = $4) "
            "     OR (i.applied_on='1_product'          AND i.product_tmpl_id = $5) "
            "     OR (i.applied_on='2_product_category' AND i.categ_id        = $6) "
            "     OR  i.applied_on='3_global' ) "
            // Narrowest first: the applied_on codes sort that way by design.
            "ORDER BY i.applied_on ASC, i.sequence ASC, i.id ASC LIMIT 1",
            pqxx::params{pricelistId, qtyMicros, date, productId,
                         p[0][3].is_null() ? 0 : p[0][3].as<int>(),
                         p[0][2].is_null() ? 0 : p[0][2].as<int>()});
        if (rows.empty()) { if (why) *why = "no rule matched: list price"; return listPrice; }

        const auto& r = rows[0];
        const std::string mode = r[2].c_str();
        const long long baseVal = (std::string(r[3].c_str()) == "standard_price") ? costPrice : listPrice;
        long long out = listPrice;
        if (mode == "fixed") {
            out = r[4].as<long long>(0);
            if (why) *why = "fixed price (rule " + std::string(r[0].c_str()) + ")";
        } else if (mode == "percentage") {
            // percent stored scaled: 10.0 -> 10_000_000 micro-percent.
            const long long pct = r[5].as<long long>(0);
            out = baseVal - (baseVal / 1000000LL) * pct / 100LL;
            if (why) *why = "percentage off (rule " + std::string(r[0].c_str()) + ")";
        } else {   // formula: base, minus a discount %, plus a surcharge
            const long long disc = r[6].as<long long>(0);
            const long long surch = r[7].as<long long>(0);
            out = baseVal - (baseVal / 1000000LL) * disc / 100LL + surch;
            if (why) *why = "formula on " + std::string(r[3].c_str()) +
                            " (rule " + std::string(r[0].c_str()) + ")";
        }
        return out < 0 ? 0 : out;
    }

private:
    std::shared_ptr<DbConnection> db2_;

    struct Args { int list = 0, product = 0; long long qty = 1000000; std::string date; };
    static Args parse(const core::CallKwArgs& call) {
        Args a;
        const auto v = call.arg(0);
        auto I=[&](const char* k)->int{
            if (!v.is_object() || !v.contains(k)) return 0;
            if (v[k].is_number_integer()) return v[k].get<int>();
            if (v[k].is_array() && !v[k].empty() && v[k][0].is_number_integer()) return v[k][0].get<int>();
            return 0;
        };
        a.list = I("pricelist_id"); a.product = I("product_id");
        if (v.is_object() && v.contains("quantity") && v["quantity"].is_number())
            a.qty = core::Money::fromJson(v["quantity"].get<double>()).micros();
        if (v.is_object() && v.contains("date") && v["date"].is_string())
            a.date = v["date"].get<std::string>();
        if (a.date.empty()) a.date = "CURRENT";
        return a;
    }

    nlohmann::json handleGetPrice(const core::CallKwArgs& call) {
        const Args a = parse(call);
        if (a.product <= 0) throw infrastructure::ValidationError("A product is required.");
        auto conn = db2_->acquire(); pqxx::work txn{conn.get()};
        const std::string d = (a.date == "CURRENT")
            ? txn.exec("SELECT CURRENT_DATE::text")[0][0].c_str() : a.date;
        std::string why;
        const long long px = resolve(txn, a.list, a.product, a.qty, d, &why);
        txn.commit();
        return {{"price", core::Money::fromMicros(px).toJson()}, {"reason", why}};
    }

    /// Every rule that could apply, in the order they are considered — so a
    /// surprising price can be explained instead of argued about.
    nlohmann::json handleBreakdown(const core::CallKwArgs& call) {
        const Args a = parse(call);
        if (a.product <= 0 || a.list <= 0)
            throw infrastructure::ValidationError("A pricelist and a product are required.");
        auto conn = db2_->acquire(); pqxx::work txn{conn.get()};
        const std::string d = (a.date == "CURRENT")
            ? txn.exec("SELECT CURRENT_DATE::text")[0][0].c_str() : a.date;
        auto p = txn.exec("SELECT categ_id, product_tmpl_id FROM product_product WHERE id=$1",
                          pqxx::params{a.product});
        nlohmann::json out = nlohmann::json::array();
        if (p.empty()) return out;
        for (const auto& r : txn.exec(
                 "SELECT id, applied_on, compute_price, min_quantity, date_start, date_end, sequence "
                 "FROM product_pricelist_item i WHERE i.pricelist_id=$1 "
                 "  AND ( (i.applied_on='0_product_variant'  AND i.product_id      = $2) "
                 "     OR (i.applied_on='1_product'          AND i.product_tmpl_id = $3) "
                 "     OR (i.applied_on='2_product_category' AND i.categ_id        = $4) "
                 "     OR  i.applied_on='3_global' ) "
                 "ORDER BY i.applied_on ASC, i.sequence ASC, i.id ASC",
                 pqxx::params{a.list, a.product,
                              p[0][1].is_null() ? 0 : p[0][1].as<int>(),
                              p[0][0].is_null() ? 0 : p[0][0].as<int>()})) {
            const long long minq = r[3].as<long long>(0);
            const bool qtyOk = minq <= a.qty;
            out.push_back({{"id", r[0].as<int>()}, {"applied_on", r[1].c_str()},
                           {"compute_price", r[2].c_str()},
                           {"min_quantity", core::Money::fromMicros(minq).toJson()},
                           {"applies", qtyOk}});
        }
        txn.commit();
        return out;
    }
};

// ================================================================
// docs/097 — the part-lookup contract
// ================================================================
/**
 * The exchange with an external agent, in three calls:
 *
 *   describe  — c-erp states its vocabulary: the category tree, the unit
 *               symbols, the parameter names already in use. The agent targets
 *               THIS, so its answer needs no fuzzy matching afterwards.
 *   submit    — the agent posts what it found. c-erp validates and STAGES it;
 *               nothing touches a product yet.
 *   apply     — a human confirms, and only then is a product written.
 *
 * The staging step is the whole design. An agent that browses the web will
 * sometimes be confidently wrong, and a wrong resistance that lands silently in
 * the catalogue is a part someone solders. Everything arrives as a proposal
 * with its issues attached.
 */
class PartLookupViewModel : public core::BaseViewModel {
public:
    explicit PartLookupViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("describe",      handleDescribe)
        REGISTER_METHOD("search_read",   handleList)
        REGISTER_METHOD("read",          handleReadOne)
        REGISTER_MUTATOR("submit",       handleSubmit)
        REGISTER_MUTATOR("apply",        handleApply)
        REGISTER_MUTATOR("reject",       handleReject)
        REGISTER_METHOD("fields_get",    handleFieldsGet)
    }
    std::string modelName() const override { return "part.lookup"; }

private:
    std::shared_ptr<DbConnection> db_;

    static std::string S(const nlohmann::json& j, const char* k) {
        return (j.is_object() && j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string{};
    }

    /// The vocabulary the agent should answer in.
    nlohmann::json handleDescribe(const core::CallKwArgs&) {
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        nlohmann::json out;

        nlohmann::json cats = nlohmann::json::array();
        for (const auto& r : txn.exec(
                 "WITH RECURSIVE t AS ("
                 "  SELECT id, name, parent_id, name::text AS path FROM product_category WHERE parent_id IS NULL "
                 "  UNION ALL "
                 "  SELECT c.id, c.name, c.parent_id, t.path || ' / ' || c.name "
                 "  FROM product_category c JOIN t ON c.parent_id = t.id) "
                 "SELECT id, path FROM t ORDER BY path"))
            cats.push_back({{"id", r[0].as<int>()}, {"path", r[1].c_str()}});
        out["categories"] = cats;

        nlohmann::json units = nlohmann::json::array();
        for (const auto& r : txn.exec(
                 "SELECT symbol, name, quantity_kind, factor FROM part_unit "
                 "WHERE quantity_kind IS NOT NULL ORDER BY quantity_kind, factor"))
            units.push_back({{"symbol", r[0].c_str()}, {"name", r[1].c_str()},
                             {"quantity", r[2].c_str()}, {"factor_to_base", r[3].as<double>(1)}});
        out["units"] = units;

        nlohmann::json pnames = nlohmann::json::array();
        for (const auto& r : txn.exec("SELECT DISTINCT name FROM part_parameter ORDER BY name"))
            pnames.push_back(r[0].c_str());
        out["known_parameters"] = pnames;

        nlohmann::json fps = nlohmann::json::array();
        for (const auto& r : txn.exec("SELECT name FROM part_footprint ORDER BY name"))
            fps.push_back(r[0].c_str());
        out["footprints"] = fps;

        out["value_formats"] = nlohmann::json::array({"4700", "4.7k", "4k7", "100n", "2R2", "10 uF"});
        out["schema_version"] = 1;
        txn.commit();
        return out;
    }

    /**
     * Validate a proposed result and stage it.
     *
     * Validation never rejects outright — it records issues alongside the
     * payload so a reviewer sees exactly which fields to distrust. An agent
     * being wrong about one parameter should not throw away the datasheet URL
     * and the manufacturer it got right.
     */
    nlohmann::json handleSubmit(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw infrastructure::ValidationError("submit expects an object.");
        const std::string query = S(v, "query");
        const std::string mpn   = S(v, "mpn");
        if (query.empty() && mpn.empty())
            throw infrastructure::ValidationError("Either query or mpn is required.");

        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        nlohmann::json issues = nlohmann::json::array();

        // Category: accept an id or a path, and say so when neither resolves.
        int categId = 0;
        if (v.contains("category_id") && v["category_id"].is_number_integer()) {
            categId = v["category_id"].get<int>();
            if (txn.exec("SELECT 1 FROM product_category WHERE id=$1", pqxx::params{categId}).empty()) {
                issues.push_back({{"field","category_id"},{"level","error"},
                                  {"message","No category with id " + std::to_string(categId)}});
                categId = 0;
            }
        } else if (!S(v, "category_path").empty()) {
            const std::string path = S(v, "category_path");
            auto leaf = path.substr(path.find_last_of('/') == std::string::npos
                                        ? 0 : path.find_last_of('/') + 1);
            while (!leaf.empty() && leaf.front() == ' ') leaf.erase(leaf.begin());
            auto r = txn.exec("SELECT id FROM product_category WHERE name=$1 LIMIT 1", pqxx::params{leaf});
            if (r.empty())
                issues.push_back({{"field","category_path"},{"level","warning"},
                                  {"message","Unknown category '" + path + "' — pick one on review"}});
            else categId = r[0][0].as<int>();
        } else {
            issues.push_back({{"field","category"},{"level","warning"},
                              {"message","No category proposed"}});
        }

        // Parameters: every unit must be one we know, and every value parseable.
        if (v.contains("parameters") && v["parameters"].is_array()) {
            int idx = 0;
            for (const auto& p : v["parameters"]) {
                const std::string pn = S(p, "name");
                const std::string us = S(p, "unit");
                const std::string raw = p.contains("value") && p["value"].is_string()
                                            ? p["value"].get<std::string>()
                                            : (p.contains("value") && p["value"].is_number()
                                                   ? std::to_string(p["value"].get<double>()) : std::string{});
                if (pn.empty())
                    issues.push_back({{"field","parameters[" + std::to_string(idx) + "].name"},
                                      {"level","error"},{"message","Parameter name is required"}});
                if (!us.empty() &&
                    txn.exec("SELECT 1 FROM part_unit WHERE symbol=$1", pqxx::params{us}).empty())
                    issues.push_back({{"field","parameters[" + std::to_string(idx) + "].unit"},
                                      {"level","error"},
                                      {"message","Unknown unit '" + us + "' — see describe.units"}});
                double num = 0, mul = 1;
                if (!raw.empty() && !parseSiValue(raw, num, mul))
                    issues.push_back({{"field","parameters[" + std::to_string(idx) + "].value"},
                                      {"level","error"},
                                      {"message","Cannot read value '" + raw + "'"}});
                ++idx;
            }
        }

        double conf = 0.0;
        if (v.contains("confidence") && v["confidence"].is_number())
            conf = v["confidence"].get<double>();
        if (conf < 0 || conf > 1)
            issues.push_back({{"field","confidence"},{"level","warning"},
                              {"message","confidence should be between 0 and 1"}});

        bool hasError = false;
        for (const auto& i : issues) if (i.value("level", "") == "error") hasError = true;

        auto ins = txn.exec(
            "INSERT INTO part_lookup_result (query, mpn, manufacturer, state, payload, issues, "
            "  categ_id, source, confidence, company_id) "
            "VALUES ($1, NULLIF($2,''), NULLIF($3,''), $4, $5::jsonb, $6::jsonb, "
            "        NULLIF($7,'')::int, NULLIF($8,''), $9, $10) RETURNING id",
            pqxx::params{query.empty() ? mpn : query, mpn, S(v,"manufacturer"),
                         std::string(hasError ? "invalid" : "pending"),
                         v.dump(), issues.dump(),
                         categId > 0 ? std::to_string(categId) : std::string{},
                         S(v,"source"), conf, core::CurrentUser::get().companyId});
        const int id = ins[0][0].as<int>();
        txn.commit();
        return {{"ok", !hasError}, {"id", id},
                {"state", hasError ? "invalid" : "pending"}, {"issues", issues}};
    }

    nlohmann::json handleList(const core::CallKwArgs& call) {
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string state;
        { auto d = call.domain(); if (d.is_array()) for (const auto& c : d)
            if (c.is_array() && c.size()==3 && c[0].is_string() &&
                c[0].get<std::string>()=="state" && c[2].is_string())
                state = c[2].get<std::string>(); }
        std::string sql =
            "SELECT id, query, mpn, manufacturer, state, confidence, source, "
            "       COALESCE(jsonb_array_length(issues),0) AS nissues, "
            "       to_char(create_date,'YYYY-MM-DD HH24:MI') AS created, product_id "
            "FROM part_lookup_result ";
        pqxx::params p;
        if (!state.empty()) { sql += "WHERE state=$1 "; p.append(state); }
        sql += "ORDER BY id DESC LIMIT 200";
        auto res = state.empty() ? txn.exec(sql) : txn.exec(sql, p);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : res)
            arr.push_back({{"id", r[0].as<int>()}, {"query", r[1].c_str()},
                           {"mpn", r[2].is_null() ? "" : r[2].c_str()},
                           {"manufacturer", r[3].is_null() ? "" : r[3].c_str()},
                           {"state", r[4].c_str()}, {"confidence", r[5].as<double>(0)},
                           {"source", r[6].is_null() ? "" : r[6].c_str()},
                           {"issues", r[7].as<int>(0)}, {"created", r[8].c_str()},
                           {"product_id", r[9].is_null() ? 0 : r[9].as<int>()}});
        return arr;
    }

    nlohmann::json handleReadOne(const core::CallKwArgs& call) {
        if (call.ids().empty()) throw infrastructure::ValidationError("An id is required.");
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        auto r = txn.exec("SELECT id, query, mpn, manufacturer, state, payload, issues, "
                          "       categ_id, confidence, source, product_id "
                          "FROM part_lookup_result WHERE id=$1", pqxx::params{call.ids().front()});
        if (r.empty()) throw infrastructure::ValidationError("No such lookup result.");
        return {{"id", r[0][0].as<int>()}, {"query", r[0][1].c_str()},
                {"mpn", r[0][2].is_null() ? "" : r[0][2].c_str()},
                {"manufacturer", r[0][3].is_null() ? "" : r[0][3].c_str()},
                {"state", r[0][4].c_str()},
                {"payload", nlohmann::json::parse(r[0][5].c_str())},
                {"issues", nlohmann::json::parse(r[0][6].c_str())},
                {"categ_id", r[0][7].is_null() ? 0 : r[0][7].as<int>()},
                {"confidence", r[0][8].as<double>(0)},
                {"source", r[0][9].is_null() ? "" : r[0][9].c_str()},
                {"product_id", r[0][10].is_null() ? 0 : r[0][10].as<int>()}};
    }

    nlohmann::json handleReject(const core::CallKwArgs& call) {
        if (call.ids().empty()) throw infrastructure::ValidationError("An id is required.");
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        txn.exec("UPDATE part_lookup_result SET state='rejected', write_date=now() WHERE id=$1",
                 pqxx::params{call.ids().front()});
        txn.commit();
        return {{"ok", true}};
    }

    /**
     * Write a staged result into the catalogue.
     *
     * Creates the product when none is given, then upserts its parameters,
     * manufacturer part number and footprint. Parameters are stored twice: as
     * the value a person typed, and as `value_base` for searching.
     */
    nlohmann::json handleApply(const core::CallKwArgs& call) {
        const auto a = call.arg(0);
        const int id = (a.is_object() && a.contains("id") && a["id"].is_number_integer())
                           ? a["id"].get<int>()
                           : (call.ids().empty() ? 0 : call.ids().front());
        if (id <= 0) throw infrastructure::ValidationError("A lookup result id is required.");

        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        auto r = txn.exec("SELECT payload, categ_id, state FROM part_lookup_result WHERE id=$1",
                          pqxx::params{id});
        if (r.empty()) throw infrastructure::ValidationError("No such lookup result.");
        if (std::string(r[0][2].c_str()) == "applied")
            throw infrastructure::ValidationError("This result has already been applied.");
        const nlohmann::json v = nlohmann::json::parse(r[0][0].c_str());

        // The reviewer's choices win over the agent's.
        int categId = r[0][1].is_null() ? 0 : r[0][1].as<int>();
        if (a.is_object() && a.contains("category_id") && a["category_id"].is_number_integer())
            categId = a["category_id"].get<int>();
        int productId = (a.is_object() && a.contains("product_id") && a["product_id"].is_number_integer())
                            ? a["product_id"].get<int>() : 0;

        const std::string mpn  = S(v, "mpn");
        const std::string name = !S(v, "name").empty() ? S(v, "name")
                               : (!mpn.empty() ? mpn : S(v, "query"));

        if (productId <= 0) {
            const std::string desc = S(v, "description");
            auto ins = txn.exec(
                "INSERT INTO product_product (name, default_code, description, type, "
                "  uom_id, uom_po_id, list_price, standard_price, sale_ok, purchase_ok, active, company_id) "
                "VALUES ($1, NULLIF($2,''), NULLIF($3,''), 'product',1,1,0,0,TRUE,TRUE,TRUE,$4) RETURNING id",
                pqxx::params{name, mpn, desc, core::CurrentUser::get().companyId});
            productId = ins[0][0].as<int>();
            if (categId > 0)
                txn.exec("UPDATE product_product SET categ_id=$1 WHERE id=$2",
                         pqxx::params{categId, productId});
        } else if (categId > 0) {
            txn.exec("UPDATE product_product SET categ_id=$1 WHERE id=$2",
                     pqxx::params{categId, productId});
        }

        int nParams = 0;
        if (v.contains("parameters") && v["parameters"].is_array()) {
            for (const auto& p : v["parameters"]) {
                const std::string pn = S(p, "name");
                if (pn.empty()) continue;
                const std::string us = S(p, "unit");
                std::string raw;
                if (p.contains("value") && p["value"].is_string()) raw = p["value"].get<std::string>();
                else if (p.contains("value") && p["value"].is_number())
                    raw = std::to_string(p["value"].get<double>());

                int unitId = 0; double factor = 1; std::string kind;
                if (!us.empty()) {
                    auto u = txn.exec("SELECT id, factor, quantity_kind FROM part_unit WHERE symbol=$1",
                                      pqxx::params{us});
                    if (!u.empty()) { unitId = u[0][0].as<int>(); factor = u[0][1].as<double>(1);
                                      kind = u[0][2].is_null() ? "" : u[0][2].c_str(); }
                }
                double num = 0, mul = 1;
                const bool numeric = !raw.empty() && parseSiValue(raw, num, mul);
                // The SI prefix in the text and the unit symbol are two ways of
                // saying the same thing. "4.7" + "kΩ" and "4k7" + "Ω" must land
                // on the same base value, so the parsed number is scaled by the
                // unit's factor and the text's own prefix is already inside it.
                const double base = numeric ? num * factor : 0.0;

                // NULLIF/CAST keeps the optional columns honest: an absent unit
                // or a non-numeric value must land as SQL NULL, not as 0.
                auto ex = txn.exec("SELECT id FROM part_parameter WHERE product_id=$1 AND name=$2",
                                   pqxx::params{productId, pn});
                const std::string unitTxt = unitId > 0 ? std::to_string(unitId) : std::string{};
                const std::string baseTxt = numeric   ? std::to_string(base)    : std::string{};
                if (ex.empty())
                    txn.exec("INSERT INTO part_parameter (product_id, name, value_numeric, unit_id, "
                             "  value_text, value_base, quantity_kind) "
                             "VALUES ($1,$2,$3::double precision, NULLIF($4,'')::int, "
                             "        NULLIF($5,''), NULLIF($6,'')::double precision, NULLIF($7,''))",
                             pqxx::params{productId, pn, std::to_string(numeric ? num : 0.0),
                                          unitTxt, numeric ? std::string{} : raw, baseTxt, kind});
                else
                    txn.exec("UPDATE part_parameter SET value_numeric=$1::double precision, "
                             "  unit_id=NULLIF($2,'')::int, value_text=NULLIF($3,''), "
                             "  value_base=NULLIF($4,'')::double precision, "
                             "  quantity_kind=NULLIF($5,'') WHERE id=$6",
                             pqxx::params{std::to_string(numeric ? num : 0.0), unitTxt,
                                          numeric ? std::string{} : raw, baseTxt, kind,
                                          ex[0][0].as<int>()});
                ++nParams;
            }
        }

        // Manufacturer part number, if the manufacturer is already a contact.
        const std::string manuf = S(v, "manufacturer");
        if (!manuf.empty() && !mpn.empty()) {
            auto m = txn.exec("SELECT id FROM res_partner WHERE name=$1 LIMIT 1", pqxx::params{manuf});
            int mid = m.empty()
                ? txn.exec("INSERT INTO res_partner (name, is_company, active) VALUES ($1,TRUE,TRUE) "
                           "RETURNING id", pqxx::params{manuf})[0][0].as<int>()
                : m[0][0].as<int>();
            // The casts are required, not decorative: $3 appears both in the
            // SELECT list (where PostgreSQL can deduce nothing) and in the
            // WHERE (where it is text), and that disagreement is reported as
            // "inconsistent types deduced for parameter $3".
            txn.exec("INSERT INTO part_manufacturer_info (product_id, manufacturer_id, part_number) "
                     "SELECT $1::int, $2::int, $3::varchar WHERE NOT EXISTS ("
                     "  SELECT 1 FROM part_manufacturer_info "
                     "  WHERE product_id=$1::int AND manufacturer_id=$2::int AND part_number=$3::varchar)",
                     pqxx::params{productId, mid, mpn});
        }

        // Footprint, only when it is one we already know — inventing footprints
        // from scraped text is how a library fills with near-duplicates.
        const std::string fp = S(v, "footprint");
        if (!fp.empty()) {
            auto f = txn.exec("SELECT id FROM part_footprint WHERE name=$1", pqxx::params{fp});
            if (!f.empty())
                txn.exec("UPDATE product_product SET footprint_id=$1 WHERE id=$2",
                         pqxx::params{f[0][0].as<int>(), productId});
        }

        txn.exec("UPDATE part_lookup_result SET state='applied', product_id=$1, write_date=now() "
                 "WHERE id=$2", pqxx::params{productId, id});
        txn.commit();

        LOG_INFO << "[lookup] result " << id << " applied to product " << productId
                 << " (" << nParams << " parameters)";
        return {{"ok", true}, {"product_id", productId}, {"parameters", nParams}};
    }

    nlohmann::json handleFieldsGet(const core::CallKwArgs&) {
        return {
            {"query",        {{"type","char"},   {"string","Query"}}},
            {"mpn",          {{"type","char"},   {"string","MPN"}}},
            {"manufacturer", {{"type","char"},   {"string","Manufacturer"}}},
            {"state",        {{"type","char"},   {"string","State"}}},
            {"confidence",   {{"type","float"},  {"string","Confidence"}}},
            {"source",       {{"type","char"},   {"string","Source"}}},
        };
    }
};

void ProductModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("product.category", [db]{ return std::make_shared<ProductCategory>(db); });
    models_.registerCreator("product.product",  [db]{ return std::make_shared<ProductProduct>(db); });
    models_.registerCreator("product.supplierinfo", [db]{ return std::make_shared<ProductSupplierInfo>(db); });
    models_.registerCreator("part.footprint",          [db]{ return std::make_shared<PartFootprint>(db); });
    models_.registerCreator("part.unit",               [db]{ return std::make_shared<PartUnit>(db); });
    models_.registerCreator("part.parameter",          [db]{ return std::make_shared<PartParameter>(db); });
    models_.registerCreator("part.manufacturer.info",  [db]{ return std::make_shared<PartManufacturerInfo>(db); });
    // docs/096
    models_.registerCreator("product.template",        [db]{ return std::make_shared<ProductTemplate>(db); });
    models_.registerCreator("product.attribute",       [db]{ return std::make_shared<ProductAttribute>(db); });
    models_.registerCreator("product.attribute.value", [db]{ return std::make_shared<ProductAttributeValue>(db); });
    models_.registerCreator("product.pricelist",       [db]{ return std::make_shared<ProductPricelist>(db); });
    models_.registerCreator("product.pricelist.item",  [db]{ return std::make_shared<ProductPricelistItem>(db); });
}

void ProductModule::registerServices() {}

void ProductModule::registerViewModels() {
    auto db = services_.db();
    // Custom VM — returns parent name, child_count, product_count
    viewModels_.registerCreator("product.category", [db]{
        return std::make_shared<ProductCategoryViewModel>(db);
    });
    // Not the generic one: a variant created through the API must come with a
    // template. See ProductProductViewModel.
    viewModels_.registerCreator("product.product", [db]{
        return std::make_shared<ProductProductViewModel>(db);
    });
    viewModels_.registerCreator("product.supplierinfo", [db]{
        return std::make_shared<ProductSupplierInfoViewModel>(db);
    });
    viewModels_.registerCreator("part.footprint", [db]{ return std::make_shared<GenericViewModel<PartFootprint>>(db); });
    viewModels_.registerCreator("part.unit",      [db]{ return std::make_shared<GenericViewModel<PartUnit>>(db); });
    viewModels_.registerCreator("part.parameter", [db]{ return std::make_shared<PartParameterViewModel>(db); });
    viewModels_.registerCreator("part.catalog",   [db]{ return std::make_shared<PartCatalogViewModel>(db); });
    viewModels_.registerCreator("part.manufacturer.info", [db]{ return std::make_shared<PartManufacturerInfoViewModel>(db); });
    // docs/096
    viewModels_.registerCreator("product.template",        [db]{ return std::make_shared<ProductTemplateViewModel>(db); });
    viewModels_.registerCreator("product.attribute",       [db]{ return std::make_shared<GenericViewModel<ProductAttribute>>(db); });
    viewModels_.registerCreator("product.attribute.value", [db]{ return std::make_shared<GenericViewModel<ProductAttributeValue>>(db); });
    viewModels_.registerCreator("product.pricelist",       [db]{ return std::make_shared<ProductPricelistViewModel>(db); });
    viewModels_.registerCreator("product.pricelist.item",  [db]{ return std::make_shared<GenericViewModel<ProductPricelistItem>>(db); });
    viewModels_.registerCreator("part.lookup",             [db]{ return std::make_shared<PartLookupViewModel>(db); });
}

// docs/096 — arches for the template and attribute screens. Without these,
// get_views returns nothing and the action opens on a permanent "Loading views…".
class ProductTemplateListView : public core::BaseView {
public:
    std::string viewName()  const override { return "product.template.list"; }
    std::string modelName() const override { return "product.template"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Product Templates\">"
               "<field name=\"name\"/><field name=\"default_code\"/>"
               "<field name=\"categ_id\"/><field name=\"list_price\"/>"
               "<field name=\"type\"/></list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",         {{"type","char"},    {"string","Product Name"}}},
            {"default_code", {{"type","char"},    {"string","Internal Reference"}}},
            {"categ_id",     {{"type","many2one"},{"string","Category"},{"relation","product.category"}}},
            {"list_price",   {{"type","monetary"},{"string","Sales Price"}}},
            {"type",         {{"type","char"},    {"string","Type"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};
class ProductTemplateFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "product.template.form"; }
    std::string modelName() const override { return "product.template"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Product Template\"><sheet><group>"
               "<field name=\"name\"/><field name=\"default_code\"/>"
               "<field name=\"categ_id\"/><field name=\"type\"/>"
               "<field name=\"list_price\"/><field name=\"standard_price\"/>"
               "<field name=\"uom_id\"/><field name=\"uom_po_id\"/>"
               "<field name=\"sale_ok\"/><field name=\"purchase_ok\"/>"
               "<field name=\"tracking\"/><field name=\"description\"/>"
               "</group></sheet></form>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",           {{"type","char"},    {"string","Product Name"}}},
            {"default_code",   {{"type","char"},    {"string","Internal Reference"}}},
            {"categ_id",       {{"type","many2one"},{"string","Category"},{"relation","product.category"}}},
            {"type",           {{"type","char"},    {"string","Type"}}},
            {"list_price",     {{"type","monetary"},{"string","Sales Price"}}},
            {"standard_price", {{"type","monetary"},{"string","Cost"}}},
            {"uom_id",         {{"type","many2one"},{"string","Unit of Measure"},{"relation","uom.uom"}}},
            {"uom_po_id",      {{"type","many2one"},{"string","Purchase UoM"},{"relation","uom.uom"}}},
            {"sale_ok",        {{"type","boolean"}, {"string","Can be Sold"}}},
            {"purchase_ok",    {{"type","boolean"}, {"string","Can be Purchased"}}},
            {"tracking",       {{"type","char"},    {"string","Tracking"}}},
            {"description",    {{"type","text"},    {"string","Description"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};
class ProductAttributeListView : public core::BaseView {
public:
    std::string viewName()  const override { return "product.attribute.list"; }
    std::string modelName() const override { return "product.attribute"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Attributes\"><field name=\"name\"/>"
               "<field name=\"sequence\"/></list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",     {{"type","char"},   {"string","Attribute"}}},
            {"sequence", {{"type","integer"},{"string","Sequence"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};
class ProductAttributeFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "product.attribute.form"; }
    std::string modelName() const override { return "product.attribute"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Attribute\"><sheet><group>"
               "<field name=\"name\"/><field name=\"sequence\"/>"
               "</group></sheet></form>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",     {{"type","char"},   {"string","Attribute"}}},
            {"sequence", {{"type","integer"},{"string","Sequence"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};


class ProductPricelistListView : public core::BaseView {
public:
    std::string viewName()  const override { return "product.pricelist.list"; }
    std::string modelName() const override { return "product.pricelist"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Pricelists\"><field name=\"name\"/>"
               "<field name=\"currency_id\"/><field name=\"sequence\"/></list>";
    }
    nlohmann::json fields() const override {
        return {{"name",{{"type","char"},{"string","Pricelist"}}},
                {"currency_id",{{"type","many2one"},{"string","Currency"},{"relation","res.currency"}}},
                {"sequence",{{"type","integer"},{"string","Sequence"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};
class ProductPricelistFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "product.pricelist.form"; }
    std::string modelName() const override { return "product.pricelist"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Pricelist\"><sheet><group><field name=\"name\"/>"
               "<field name=\"currency_id\"/><field name=\"sequence\"/>"
               "<field name=\"active\"/></group></sheet></form>";
    }
    nlohmann::json fields() const override {
        return {{"name",{{"type","char"},{"string","Pricelist"}}},
                {"currency_id",{{"type","many2one"},{"string","Currency"},{"relation","res.currency"}}},
                {"sequence",{{"type","integer"},{"string","Sequence"}}},
                {"active",{{"type","boolean"},{"string","Active"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};
class ProductPricelistItemListView : public core::BaseView {
public:
    std::string viewName()  const override { return "product.pricelist.item.list"; }
    std::string modelName() const override { return "product.pricelist.item"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Price Rules\"><field name=\"pricelist_id\"/>"
               "<field name=\"applied_on\"/><field name=\"product_id\"/>"
               "<field name=\"min_quantity\"/><field name=\"compute_price\"/>"
               "<field name=\"fixed_price\"/><field name=\"sequence\"/></list>";
    }
    nlohmann::json fields() const override {
        return {{"pricelist_id",{{"type","many2one"},{"string","Pricelist"},{"relation","product.pricelist"}}},
                {"applied_on",{{"type","char"},{"string","Applied On"}}},
                {"product_id",{{"type","many2one"},{"string","Variant"},{"relation","product.product"}}},
                {"min_quantity",{{"type","float"},{"string","Min. Qty"}}},
                {"compute_price",{{"type","char"},{"string","Compute"}}},
                {"fixed_price",{{"type","monetary"},{"string","Fixed Price"}}},
                {"sequence",{{"type","integer"},{"string","Sequence"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

void ProductModule::registerViews() {
    views_.registerCreator("product.category.list", []{ return std::make_shared<ProductCategoryListView>(); });
    views_.registerCreator("product.category.form", []{ return std::make_shared<ProductCategoryFormView>(); });
    views_.registerCreator("product.product.list",  []{ return std::make_shared<ProductProductListView>(); });
    views_.registerCreator("product.product.form",  []{ return std::make_shared<ProductProductFormView>(); });
    views_.registerCreator("product.supplierinfo.list", []{ return std::make_shared<ProductSupplierInfoListView>(); });
    views_.registerCreator("part.footprint.list", []{ return std::make_shared<PartCatalogListView>("part.footprint","part.footprint.list","Footprints","<field name=\"description\"/>"); });
    views_.registerCreator("part.unit.list",      []{ return std::make_shared<PartCatalogListView>("part.unit","part.unit.list","Units of Measure","<field name=\"symbol\"/>"); });
    // docs/096
    views_.registerCreator("product.template.list",  []{ return std::make_shared<ProductTemplateListView>(); });
    views_.registerCreator("product.template.form",  []{ return std::make_shared<ProductTemplateFormView>(); });
    views_.registerCreator("product.attribute.list", []{ return std::make_shared<ProductAttributeListView>(); });
    views_.registerCreator("product.attribute.form", []{ return std::make_shared<ProductAttributeFormView>(); });
    views_.registerCreator("product.pricelist.list", []{ return std::make_shared<ProductPricelistListView>(); });
    views_.registerCreator("product.pricelist.form", []{ return std::make_shared<ProductPricelistFormView>(); });
    views_.registerCreator("product.pricelist.item.list", []{ return std::make_shared<ProductPricelistItemListView>(); });
}

// ---------------------------------------------------------------
// registerRoutes — label printing (docs/099)
//
// Labels are served as SVG, and a sheet of them as an HTML page that prints
// from the browser. There is no PDF step and no image library: the browser
// already rasterises vector art at the printer's resolution, which is exactly
// what a 2mm-tall part code needs.
// ---------------------------------------------------------------
static drogon::HttpResponsePtr labelError(int status, const std::string& msg) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    resp->setContentTypeCode(drogon::CT_TEXT_HTML);
    resp->setBody("<html><body><h2>Error: " + msg + "</h2></body></html>");
    return resp;
}

void ProductModule::registerRoutes() {
    auto db       = services_.db();
    auto sessions = services_.sessions();
    bool devMode  = services_.devMode();   // SEC-28: gate ex.what() disclosure

    auto checkAuth = [sessions](const drogon::HttpRequestPtr& req) -> bool {
        if (!sessions) return false;
        const std::string sid = req->getCookie(infrastructure::SessionManager::cookieName());
        if (sid.empty()) return false;
        auto sess = sessions->get(sid);
        return sess.has_value() && sess->isAuthenticated();
    };

    auto svgResponse = [](const std::string& body) {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setStatusCode(drogon::k200OK);
        r->setContentTypeString("image/svg+xml; charset=utf-8");
        r->setBody(body);
        return r;
    };

    // Read one product's label content. Kept in one place so a single label and
    // a sheet of them can never disagree about what a label says.
    auto specFor = [](pqxx::work& txn, int id, const std::string& payloadMode,
                      const std::string& host, double w, double h, bool showText)
                   -> core::LabelSpec {
        auto res = txn.exec(
            "SELECT pp.id, pp.name, COALESCE(pp.default_code,'') AS code, "
            "       COALESCE(pp.barcode,'') AS barcode, COALESCE(f.name,'') AS pkg, "
            "       COALESCE(c.name,'') AS categ "
            "FROM product_product pp "
            "LEFT JOIN part_footprint f ON f.id = pp.footprint_id "
            "LEFT JOIN product_category c ON c.id = pp.categ_id "
            "WHERE pp.id = $1 AND pp.active",
            pqxx::params{id});
        if (res.empty())
            throw std::runtime_error("No such product: " + std::to_string(id));

        const std::string name    = res[0]["name"].c_str();
        const std::string code    = res[0]["code"].c_str();
        const std::string barcode = res[0]["barcode"].c_str();
        const std::string pkg     = res[0]["pkg"].c_str();
        const std::string categ   = res[0]["categ"].c_str();

        core::LabelSpec spec;
        spec.widthMm  = w;
        spec.heightMm = h;
        spec.showText = showText;
        // A URL payload lets a phone camera open the product; a code payload is
        // what an existing warehouse scanner expects. Neither is right for
        // everyone, so it is a parameter rather than a decision.
        if (payloadMode == "url") {
            spec.payload = "http://" + (host.empty() ? std::string("localhost:8069") : host)
                         + "/#action=products&view=form&id=" + std::to_string(id);
        } else {
            spec.payload = !barcode.empty() ? barcode
                         : (!code.empty() ? code : ("PRODUCT-" + std::to_string(id)));
        }
        spec.title    = !code.empty() ? code : ("#" + std::to_string(id));
        spec.subtitle = name;
        spec.extra    = !pkg.empty() ? pkg : categ;
        return spec;
    };

    // --- one label -------------------------------------------------------
    drogon::app().registerHandler(
        "/label/product/{1}",
        [db, checkAuth, devMode, svgResponse, specFor](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& idStr)
        {
            if (!checkAuth(req)) { cb(labelError(401, "Please sign in.")); return; }
            int id = 0;
            try { id = std::stoi(idStr); }
            catch (...) { cb(labelError(400, "Invalid product id")); return; }

            auto num = [&req](const char* k, double dflt, double lo, double hi) {
                const std::string v = req->getParameter(k);
                if (v.empty()) return dflt;
                try { return std::max(lo, std::min(hi, std::stod(v))); }
                catch (...) { return dflt; }
            };
            const double w = num("w", 50.0, 10.0, 297.0);
            const double h = num("h", 25.0,  8.0, 210.0);
            const bool showText = req->getParameter("text") != "0";
            const std::string mode = req->getParameter("payload");

            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                auto spec = specFor(txn, id, mode, req->getHeader("host"), w, h, showText);
                txn.commit();
                cb(svgResponse(core::renderLabelSvg(spec)));
            } catch (const infrastructure::PoolExhaustedException& e) {
                LOG_ERROR << "[product/label] pool: " << e.what();
                cb(labelError(503, "The server is temporarily overloaded. Please retry."));
            } catch (const std::runtime_error& ex) {
                cb(labelError(404, devMode ? ex.what() : "Record not found"));
            } catch (const std::exception& ex) {
                LOG_ERROR << "[product/label] " << ex.what();
                cb(labelError(500, devMode ? ex.what() : "An internal error occurred"));
            }
        },
        {drogon::Get});

    // --- a printable sheet -----------------------------------------------
    drogon::app().registerHandler(
        "/labels/sheet",
        [db, checkAuth, devMode, specFor](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            if (!checkAuth(req)) { cb(labelError(401, "Please sign in.")); return; }

            // ids=1,2,3 — parsed to ints here, so nothing textual reaches SQL.
            std::vector<int> ids;
            {
                const std::string raw = req->getParameter("ids");
                std::string cur;
                for (size_t i = 0; i <= raw.size(); ++i) {
                    if (i == raw.size() || raw[i] == ',') {
                        if (!cur.empty()) {
                            try { ids.push_back(std::stoi(cur)); } catch (...) {}
                            cur.clear();
                        }
                    } else if (std::isdigit(static_cast<unsigned char>(raw[i]))) {
                        cur += raw[i];
                    }
                }
            }
            if (ids.empty()) { cb(labelError(400, "No product ids given (ids=1,2,3).")); return; }
            if (ids.size() > 500) ids.resize(500);   // a sheet, not a print job

            auto num = [&req](const char* k, double dflt, double lo, double hi) {
                const std::string v = req->getParameter(k);
                if (v.empty()) return dflt;
                try { return std::max(lo, std::min(hi, std::stod(v))); }
                catch (...) { return dflt; }
            };
            const double w    = num("w", 50.0, 10.0, 297.0);
            const double h    = num("h", 25.0,  8.0, 210.0);
            const double gap  = num("gap", 2.0, 0.0, 20.0);
            const int    cols = static_cast<int>(num("cols", 4, 1, 12));
            const int copies  = static_cast<int>(num("copies", 1, 1, 50));
            const bool showText = req->getParameter("text") != "0";
            const std::string mode = req->getParameter("payload");

            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                std::vector<core::LabelSpec> specs;
                for (const int id : ids) {
                    core::LabelSpec spec;
                    try {
                        spec = specFor(txn, id, mode, req->getHeader("host"), w, h, showText);
                    } catch (const std::runtime_error&) {
                        continue;   // a deleted id should not lose the whole sheet
                    }
                    for (int c = 0; c < copies; ++c) specs.push_back(spec);
                }
                txn.commit();
                if (specs.empty()) { cb(labelError(404, "None of those products exist.")); return; }

                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k200OK);
                r->setContentTypeCode(drogon::CT_TEXT_HTML);
                r->setBody(core::renderLabelSheetHtml(specs, cols, gap, "Part labels"));
                cb(r);
            } catch (const infrastructure::PoolExhaustedException& e) {
                LOG_ERROR << "[product/labels] pool: " << e.what();
                cb(labelError(503, "The server is temporarily overloaded. Please retry."));
            } catch (const std::exception& ex) {
                LOG_ERROR << "[product/labels] " << ex.what();
                cb(labelError(500, devMode ? ex.what() : "An internal error occurred"));
            }
        },
        {drogon::Get});

    // --- a bare QR, for anything that is not a product --------------------
    drogon::app().registerHandler(
        "/label/qr",
        [checkAuth, devMode, svgResponse](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            if (!checkAuth(req)) { cb(labelError(401, "Please sign in.")); return; }
            const std::string data = req->getParameter("data");
            if (data.empty()) { cb(labelError(400, "data is required")); return; }
            double size = 30.0;
            try { if (!req->getParameter("size").empty())
                      size = std::max(8.0, std::min(200.0, std::stod(req->getParameter("size")))); }
            catch (...) {}
            int quiet = 4;
            try { if (!req->getParameter("quiet").empty())
                      quiet = std::max(0, std::min(8, std::stoi(req->getParameter("quiet")))); }
            catch (...) {}
            try {
                cb(svgResponse(core::renderQrSvg(data, size, quiet)));
            } catch (const std::length_error& ex) {
                cb(labelError(400, devMode ? ex.what() : "That payload is too long for a QR code."));
            } catch (const std::exception& ex) {
                LOG_ERROR << "[product/qr] " << ex.what();
                cb(labelError(500, devMode ? ex.what() : "An internal error occurred"));
            }
        },
        {drogon::Get});

    LOG_INFO << "[product] label routes registered (docs/099)";
}

void ProductModule::initialize() {
    ensureSchema_();
    seedCategories_();
    seedPartUnits_();      // docs/097 — units before anything measures with them
    seedFootprints_();     // docs/098 — packages before anything is filtered by them
    migrateTemplates_();   // docs/096 — before menus, so the screens have data
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
    // A nameless category renders as a blank row in the sidebar tree and cannot
    // be told apart from any other blank one. The create handler now refuses an
    // empty name, but rows already written that way have to go, and the
    // constraint below stops any future path — a raw INSERT, a bad import —
    // putting one back. Both steps are safe to repeat.
    //
    // Only unreferenced blanks are removed: a blank category that somehow owns
    // products or children is a data problem to look at, not something to
    // delete underneath them. Those get a name instead.
    txn.exec(R"(
        DELETE FROM product_category c
         WHERE (c.name IS NULL OR btrim(c.name) = '')
           AND NOT EXISTS (SELECT 1 FROM product_product  p  WHERE p.categ_id  = c.id)
           AND NOT EXISTS (SELECT 1 FROM product_category ch WHERE ch.parent_id = c.id)
    )");
    txn.exec(R"(
        UPDATE product_category
           SET name = 'Unnamed category ' || id
         WHERE name IS NULL OR btrim(name) = ''
    )");
    txn.exec(R"(
        DO $$
        BEGIN
            IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'product_category_name_chk') THEN
                ALTER TABLE product_category
                    ADD CONSTRAINT product_category_name_chk CHECK (btrim(name) <> '');
            END IF;
        END $$;
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

    // ── docs/096: product templates, attributes and variants ─────────────
    //
    // A template is the thing a person means by "product" — "T-Shirt". A
    // variant is what stock, accounting and every order line actually move —
    // "T-Shirt (Red, L)". Until now only the variant existed, so anything sold
    // in two sizes was two unrelated records sharing nothing.
    //
    // product_product is deliberately left as the record everything else
    // references. Every existing row keeps its id and gains a template; no
    // other module has to change, and a single-variant product behaves exactly
    // as it did before.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS product_template (
            id                  SERIAL PRIMARY KEY,
            name                VARCHAR NOT NULL,
            default_code        VARCHAR,
            description         TEXT,
            description_sale    TEXT,
            description_purchase TEXT,
            type                VARCHAR NOT NULL DEFAULT 'product',
            categ_id            INTEGER REFERENCES product_category(id) ON DELETE SET NULL,
            uom_id              INTEGER REFERENCES uom_uom(id) ON DELETE SET NULL,
            uom_po_id           INTEGER REFERENCES uom_uom(id) ON DELETE SET NULL,
            list_price          BIGINT  NOT NULL DEFAULT 0,
            standard_price      BIGINT  NOT NULL DEFAULT 0,
            tracking            VARCHAR NOT NULL DEFAULT 'none',
            invoice_policy      VARCHAR NOT NULL DEFAULT 'order',
            purchase_method     VARCHAR NOT NULL DEFAULT 'purchase',
            income_account_id   INTEGER,
            expense_account_id  INTEGER,
            sale_ok             BOOLEAN NOT NULL DEFAULT TRUE,
            purchase_ok         BOOLEAN NOT NULL DEFAULT TRUE,
            image_1920          TEXT,
            company_id          INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            active              BOOLEAN NOT NULL DEFAULT TRUE,
            create_date         TIMESTAMP DEFAULT now(),
            write_date          TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS product_attribute (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            sequence    INTEGER NOT NULL DEFAULT 10,
            create_date TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS product_attribute_value (
            id           SERIAL PRIMARY KEY,
            attribute_id INTEGER NOT NULL REFERENCES product_attribute(id) ON DELETE CASCADE,
            name         VARCHAR NOT NULL,
            sequence     INTEGER NOT NULL DEFAULT 10,
            create_date  TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_pav_attribute ON product_attribute_value(attribute_id)");

    // Which attributes a given template varies on.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS product_template_attribute_line (
            id              SERIAL PRIMARY KEY,
            product_tmpl_id INTEGER NOT NULL REFERENCES product_template(id) ON DELETE CASCADE,
            attribute_id    INTEGER NOT NULL REFERENCES product_attribute(id) ON DELETE CASCADE,
            sequence        INTEGER NOT NULL DEFAULT 10,
            create_date     TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now(),
            UNIQUE (product_tmpl_id, attribute_id)
        )
    )");
    // Which of that attribute's values are in play, and what each adds to the price.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS product_template_attribute_value (
            id             SERIAL PRIMARY KEY,
            line_id        INTEGER NOT NULL REFERENCES product_template_attribute_line(id) ON DELETE CASCADE,
            value_id       INTEGER NOT NULL REFERENCES product_attribute_value(id) ON DELETE CASCADE,
            price_extra    BIGINT  NOT NULL DEFAULT 0,
            create_date    TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now(),
            UNIQUE (line_id, value_id)
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_ptav_line ON product_template_attribute_value(line_id)");
    // Values are deactivated, never deleted, for the same reason variants are:
    // product_variant_combination points here, so deleting a value cascades away
    // an ARCHIVED variant's combination. Its identity would vanish, and putting
    // the value back would then mint a second variant beside the first.
    txn.exec("ALTER TABLE product_template_attribute_value "
             "ADD COLUMN IF NOT EXISTS active BOOLEAN NOT NULL DEFAULT TRUE");

    txn.exec("ALTER TABLE product_product ADD COLUMN IF NOT EXISTS "
             "product_tmpl_id INTEGER REFERENCES product_template(id) ON DELETE CASCADE");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_pp_tmpl ON product_product(product_tmpl_id)");

    // The exact combination a variant stands for. One row per attribute, so a
    // variant's identity is the SET of its rows — which is what makes
    // "does this combination already exist" answerable in SQL.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS product_variant_combination (
            product_id INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
            ptav_id    INTEGER NOT NULL REFERENCES product_template_attribute_value(id) ON DELETE CASCADE,
            PRIMARY KEY (product_id, ptav_id)
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_pvc_product ON product_variant_combination(product_id)");

    // ── docs/097: units that electronics actually uses ───────────────────
    //
    // part_unit held exactly one row — Ohm — so a capacitance or a frequency
    // had nowhere to live, and parametric search compared raw numbers: 4.7 kΩ
    // and 4700 Ω did not match each other, and 100 nF sorted above 1 F.
    //
    // Every unit now declares WHAT it measures (quantity_kind) and HOW to reach
    // the SI base of that kind (factor). A parameter stores both the value the
    // user typed and `value_base`, the same value in base units — so comparison
    // is a plain numeric range on one column, across every prefix.
    txn.exec("ALTER TABLE part_unit ADD COLUMN IF NOT EXISTS quantity_kind VARCHAR");
    txn.exec("ALTER TABLE part_unit ADD COLUMN IF NOT EXISTS factor DOUBLE PRECISION NOT NULL DEFAULT 1");
    txn.exec("ALTER TABLE part_unit ADD COLUMN IF NOT EXISTS is_base BOOLEAN NOT NULL DEFAULT FALSE");
    txn.exec("CREATE UNIQUE INDEX IF NOT EXISTS part_unit_symbol_uq ON part_unit (symbol)");

    txn.exec("ALTER TABLE part_parameter ADD COLUMN IF NOT EXISTS value_base DOUBLE PRECISION");
    txn.exec("ALTER TABLE part_parameter ADD COLUMN IF NOT EXISTS quantity_kind VARCHAR");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_part_param_search "
             "ON part_parameter (name, quantity_kind, value_base)");

    // ── docs/097: staged lookup results ──────────────────────────────────
    //
    // An external agent proposes; a human disposes. A result is written here
    // first and only reaches product tables when someone applies it, so a
    // hallucinated datasheet figure cannot silently become the resistance of a
    // part you are about to solder.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS part_lookup_result (
            id            SERIAL PRIMARY KEY,
            query         VARCHAR NOT NULL,
            mpn           VARCHAR,
            manufacturer  VARCHAR,
            state         VARCHAR NOT NULL DEFAULT 'pending'
                          CHECK (state IN ('pending','applied','rejected','invalid')),
            payload       JSONB   NOT NULL,
            issues        JSONB   NOT NULL DEFAULT '[]'::jsonb,
            product_id    INTEGER REFERENCES product_product(id) ON DELETE SET NULL,
            categ_id      INTEGER REFERENCES product_category(id) ON DELETE SET NULL,
            source        VARCHAR,
            confidence    DOUBLE PRECISION,
            company_id    INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date   TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_plr_state ON part_lookup_result(state)");

    // ── docs/096: pricelists ─────────────────────────────────────────────
    //
    // Until now a product had exactly one price. A pricelist is an ordered set
    // of RULES, and the first one that matches wins — which is what lets a
    // trade customer, a quantity break and a dated promotion coexist without
    // any of them being the product's "real" price.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS product_pricelist (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            currency_id INTEGER REFERENCES res_currency(id) ON DELETE SET NULL,
            company_id  INTEGER REFERENCES res_company(id)  ON DELETE SET NULL,
            sequence    INTEGER NOT NULL DEFAULT 10,
            active      BOOLEAN NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS product_pricelist_item (
            id              SERIAL PRIMARY KEY,
            pricelist_id    INTEGER NOT NULL REFERENCES product_pricelist(id) ON DELETE CASCADE,
            -- how narrowly this rule applies; narrower rules are considered first
            applied_on      VARCHAR NOT NULL DEFAULT '3_global'
                            CHECK (applied_on IN ('0_product_variant','1_product','2_product_category','3_global')),
            product_id      INTEGER REFERENCES product_product(id)  ON DELETE CASCADE,
            product_tmpl_id INTEGER REFERENCES product_template(id) ON DELETE CASCADE,
            categ_id        INTEGER REFERENCES product_category(id) ON DELETE CASCADE,
            min_quantity    BIGINT  NOT NULL DEFAULT 0,
            date_start      DATE,
            date_end        DATE,
            compute_price   VARCHAR NOT NULL DEFAULT 'fixed'
                            CHECK (compute_price IN ('fixed','percentage','formula')),
            fixed_price     BIGINT  NOT NULL DEFAULT 0,
            percent_price   BIGINT  NOT NULL DEFAULT 0,
            base            VARCHAR NOT NULL DEFAULT 'list_price'
                            CHECK (base IN ('list_price','standard_price')),
            price_discount  BIGINT  NOT NULL DEFAULT 0,
            price_surcharge BIGINT  NOT NULL DEFAULT 0,
            sequence        INTEGER NOT NULL DEFAULT 10,
            create_date     TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_ppi_list ON product_pricelist_item(pricelist_id)");

    // Which pricelist a customer is on, and which one an order was priced with.
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS "
             "property_product_pricelist_id INTEGER REFERENCES product_pricelist(id) ON DELETE SET NULL");
    txn.exec("ALTER TABLE sale_order ADD COLUMN IF NOT EXISTS "
             "pricelist_id INTEGER REFERENCES product_pricelist(id) ON DELETE SET NULL");

    txn.commit();
}

// ----------------------------------------------------------
// migrateTemplates_ — docs/096
//
// Give every product that has no template one of its own, copying its fields
// across. Existing products become single-variant templates, which is exactly
// what they already were in substance — so nothing observable changes today,
// and tomorrow a second size can be added without inventing a second product.
//
// Runs every start and is idempotent: it only touches rows whose
// product_tmpl_id is still NULL.
// ----------------------------------------------------------
// ----------------------------------------------------------
// seedPartUnits_ — docs/097
//
// The unit vocabulary an electronics catalogue needs, each tagged with what it
// measures and its factor to that quantity's SI base. `factor` is what makes
// 4.7 kΩ and 4700 Ω the same number in `value_base`, which is the only reason
// a parametric range search can be a plain BETWEEN.
//
// Seeded by SYMBOL, which is unique — running this again updates the metadata
// of a unit someone has already used rather than creating a rival copy of it.
// ----------------------------------------------------------
void ProductModule::seedPartUnits_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    struct U { const char* sym; const char* name; const char* kind; double factor; bool base; };
    static const U kUnits[] = {
        // resistance
        {"Ω","Ohm","resistance",1,true}, {"mΩ","Milliohm","resistance",1e-3,false},
        {"kΩ","Kilohm","resistance",1e3,false}, {"MΩ","Megohm","resistance",1e6,false},
        // capacitance
        {"F","Farad","capacitance",1,true}, {"mF","Millifarad","capacitance",1e-3,false},
        {"µF","Microfarad","capacitance",1e-6,false}, {"nF","Nanofarad","capacitance",1e-9,false},
        {"pF","Picofarad","capacitance",1e-12,false},
        // inductance
        {"H","Henry","inductance",1,true}, {"mH","Millihenry","inductance",1e-3,false},
        {"µH","Microhenry","inductance",1e-6,false}, {"nH","Nanohenry","inductance",1e-9,false},
        // voltage
        {"V","Volt","voltage",1,true}, {"mV","Millivolt","voltage",1e-3,false},
        {"kV","Kilovolt","voltage",1e3,false},
        // current
        {"A","Ampere","current",1,true}, {"mA","Milliampere","current",1e-3,false},
        {"µA","Microampere","current",1e-6,false}, {"nA","Nanoampere","current",1e-9,false},
        // power
        {"W","Watt","power",1,true}, {"mW","Milliwatt","power",1e-3,false},
        {"kW","Kilowatt","power",1e3,false},
        // frequency
        {"Hz","Hertz","frequency",1,true}, {"kHz","Kilohertz","frequency",1e3,false},
        {"MHz","Megahertz","frequency",1e6,false}, {"GHz","Gigahertz","frequency",1e9,false},
        // charge / capacity
        {"Ah","Amp-hour","charge",3600,false}, {"mAh","Milliamp-hour","charge",3.6,false},
        {"C","Coulomb","charge",1,true},
        // time
        {"s","Second","time",1,true}, {"ms","Millisecond","time",1e-3,false},
        {"µs","Microsecond","time",1e-6,false}, {"ns","Nanosecond","time",1e-9,false},
        // temperature (offset units are not modelled; °C is treated as the base)
        {"°C","Degree Celsius","temperature",1,true},
        // dimensionless and mechanical
        {"%","Percent","ratio",1,true}, {"ppm","Parts per million","ratio",1e-6,false},
        {"mm","Millimetre","length",1e-3,false}, {"m","Metre","length",1,true},
        {"g","Gram","mass",1e-3,false}, {"kg","Kilogram","mass",1,true},
        {"dB","Decibel","gain",1,true},
        {"B","Byte","data",1,true}, {"kB","Kilobyte","data",1e3,false},
        {"MB","Megabyte","data",1e6,false},
    };

    for (const auto& u : kUnits)
        txn.exec("INSERT INTO part_unit (name, symbol, quantity_kind, factor, is_base) "
                 "VALUES ($1,$2,$3,$4,$5) "
                 "ON CONFLICT (symbol) DO UPDATE SET name=EXCLUDED.name, "
                 "  quantity_kind=EXCLUDED.quantity_kind, factor=EXCLUDED.factor, "
                 "  is_base=EXCLUDED.is_base",
                 pqxx::params{u.name, u.sym, u.kind, u.factor, u.base});

    // Existing parameters predate value_base; fill it in from their own unit.
    // Only NULLs are touched, so a corrected value is never overwritten.
    const auto n = txn.exec(
        "UPDATE part_parameter p SET value_base = p.value_numeric * u.factor, "
        "       quantity_kind = u.quantity_kind "
        "FROM part_unit u WHERE u.id = p.unit_id AND p.value_base IS NULL");
    if (n.affected_rows() > 0)
        LOG_INFO << "[parts] normalised " << n.affected_rows() << " existing parameter(s)";
    // A parameter with no unit is dimensionless; base value is the raw number.
    txn.exec("UPDATE part_parameter SET value_base = value_numeric "
             "WHERE value_base IS NULL AND unit_id IS NULL");

    txn.commit();
}

// seedFootprints_ — docs/098
//
// Package names are reference data in exactly the same sense as part_unit: a
// fixed public vocabulary that every catalogue, distributor and datasheet
// already shares. Seeding them is what lets the Package facet mean something on
// a fresh install instead of being empty until someone types 0402 by hand.
void ProductModule::seedFootprints_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    // ON CONFLICT needs something to conflict on, and two footprints with the
    // same name are the same footprint.
    txn.exec("CREATE UNIQUE INDEX IF NOT EXISTS part_footprint_name_uniq ON part_footprint (name)");

    struct F { const char* name; const char* descr; };
    static const F kFootprints[] = {
        // SMD chip (imperial), the ones a passives catalogue lives on
        {"01005","SMD chip 0.4 x 0.2 mm"},   {"0201","SMD chip 0.6 x 0.3 mm"},
        {"0402","SMD chip 1.0 x 0.5 mm"},    {"0603","SMD chip 1.6 x 0.8 mm"},
        {"0805","SMD chip 2.0 x 1.25 mm"},   {"1206","SMD chip 3.2 x 1.6 mm"},
        {"1210","SMD chip 3.2 x 2.5 mm"},    {"1812","SMD chip 4.5 x 3.2 mm"},
        {"2010","SMD chip 5.0 x 2.5 mm"},    {"2512","SMD chip 6.3 x 3.2 mm"},
        // discrete / small outline
        {"SOT-23","Small outline transistor, 3 lead"},
        {"SOT-23-5","Small outline transistor, 5 lead"},
        {"SOT-89","Small outline transistor, power"},
        {"SOT-223","Small outline transistor, 4 lead power"},
        {"SOD-123","Small outline diode"}, {"SOD-323","Small outline diode, compact"},
        {"SMA","DO-214AC surface mount diode"},
        {"SMB","DO-214AA surface mount diode"},
        {"SMC","DO-214AB surface mount diode"},
        // integrated circuits
        {"SOIC-8","Small outline IC, 8 lead"},   {"SOIC-14","Small outline IC, 14 lead"},
        {"SOIC-16","Small outline IC, 16 lead"}, {"MSOP-8","Mini small outline, 8 lead"},
        {"TSSOP-8","Thin shrink small outline, 8 lead"},
        {"TSSOP-14","Thin shrink small outline, 14 lead"},
        {"TSSOP-16","Thin shrink small outline, 16 lead"},
        {"TSSOP-20","Thin shrink small outline, 20 lead"},
        {"QFN-16","Quad flat no-lead, 16 pad"},  {"QFN-20","Quad flat no-lead, 20 pad"},
        {"QFN-24","Quad flat no-lead, 24 pad"},  {"QFN-32","Quad flat no-lead, 32 pad"},
        {"LQFP-32","Low profile quad flat pack, 32 lead"},
        {"LQFP-48","Low profile quad flat pack, 48 lead"},
        {"LQFP-64","Low profile quad flat pack, 64 lead"},
        // through hole
        {"DIP-8","Dual in-line, 8 pin"},   {"DIP-14","Dual in-line, 14 pin"},
        {"DIP-16","Dual in-line, 16 pin"}, {"DIP-28","Dual in-line, 28 pin"},
        {"TO-92","Transistor outline, plastic"},
        {"TO-220","Transistor outline, power tab"},
        {"TO-247","Transistor outline, high power"},
        {"DO-35","Axial glass diode"}, {"DO-41","Axial plastic diode"},
        {"Axial","Axial leaded, through hole"},
        {"Radial","Radial leaded, through hole"},
    };

    for (const auto& f : kFootprints)
        txn.exec("INSERT INTO part_footprint (name, description) VALUES ($1,$2) "
                 "ON CONFLICT (name) DO UPDATE SET description=EXCLUDED.description",
                 pqxx::params{f.name, f.descr});

    txn.commit();
}

void ProductModule::migrateTemplates_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    auto pending = txn.exec(
        "SELECT id, name, default_code, description, description_sale, description_purchase, "
        "       type, categ_id, uom_id, uom_po_id, list_price, standard_price, tracking, "
        "       invoice_policy, purchase_method, income_account_id, expense_account_id, "
        "       sale_ok, purchase_ok, image_1920, company_id, active "
        "FROM product_product WHERE product_tmpl_id IS NULL ORDER BY id");
    if (pending.empty()) { txn.commit(); return; }

    for (const auto& r : pending) {
        auto ins = txn.exec(
            "INSERT INTO product_template (name, default_code, description, description_sale, "
            "  description_purchase, type, categ_id, uom_id, uom_po_id, list_price, standard_price, "
            "  tracking, invoice_policy, purchase_method, income_account_id, expense_account_id, "
            "  sale_ok, purchase_ok, image_1920, company_id, active) "
            "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21) "
            "RETURNING id",
            pqxx::params{
                r[1].is_null() ? std::string("Unnamed") : std::string(r[1].c_str()),
                r[2].is_null() ? nullptr : r[2].c_str(),  r[3].is_null() ? nullptr : r[3].c_str(),
                r[4].is_null() ? nullptr : r[4].c_str(),  r[5].is_null() ? nullptr : r[5].c_str(),
                r[6].is_null() ? std::string("product") : std::string(r[6].c_str()),
                r[7].is_null() ? nullptr : r[7].c_str(),  r[8].is_null() ? nullptr : r[8].c_str(),
                r[9].is_null() ? nullptr : r[9].c_str(),
                r[10].is_null() ? 0LL : r[10].as<long long>(0),
                r[11].is_null() ? 0LL : r[11].as<long long>(0),
                r[12].is_null() ? std::string("none") : std::string(r[12].c_str()),
                r[13].is_null() ? std::string("order") : std::string(r[13].c_str()),
                r[14].is_null() ? std::string("purchase") : std::string(r[14].c_str()),
                r[15].is_null() ? nullptr : r[15].c_str(), r[16].is_null() ? nullptr : r[16].c_str(),
                r[17].as<bool>(true), r[18].as<bool>(true),
                r[19].is_null() ? nullptr : r[19].c_str(), r[20].is_null() ? nullptr : r[20].c_str(),
                r[21].as<bool>(true)});
        txn.exec("UPDATE product_product SET product_tmpl_id=$1 WHERE id=$2",
                 pqxx::params{ins[0][0].as<int>(), r[0].as<int>()});
    }
    LOG_INFO << "[product] gave " << pending.size() << " product(s) their own template";
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
            (13, 'Part Units',         'part.unit',            'list,form', 'part-units',          '{}')
            -- 15 was 'Parametric Search' (part.search), removed: a strict subset
            -- of Parts Catalogue, which does the same ranges and the same SI
            -- shorthand. The id stays retired rather than reused.
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
    // Menu 58 / action 15 were 'Parametric Search'. Removed as a duplicate of
    // Parts Catalogue. Deleted here rather than merely unseeded, because a seed
    // that stops writing a row leaves it behind on every database that already
    // has one -- the menu would survive the upgrade and open a screen that no
    // longer exists. Both ids stay retired; see scripts/verify_menu_ids.sh.
    txn.exec("DELETE FROM ir_ui_menu WHERE id = 58");
    txn.exec("DELETE FROM ir_act_window WHERE id = 15");

    // docs/096 — templates and attributes. Ids from scripts/verify_menu_ids.sh:
    // actions 102/103, menus 75/76.
    txn.exec(
        "INSERT INTO ir_act_window (id, name, res_model, view_mode, path) VALUES "
        "(102, 'Product Templates', 'product.template', 'list,form', 'product-templates') "
        "ON CONFLICT (id) DO UPDATE SET name='Product Templates', "
        "res_model='product.template', view_mode='list,form', path='product-templates'");
    txn.exec(
        "INSERT INTO ir_act_window (id, name, res_model, view_mode, path) VALUES "
        "(103, 'Attributes', 'product.attribute', 'list,form', 'product-attributes') "
        "ON CONFLICT (id) DO UPDATE SET name='Attributes', "
        "res_model='product.attribute', view_mode='list,form', path='product-attributes'");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
    txn.exec(
        "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
        "(75, 'Product Templates', 50, 12, 102) "
        "ON CONFLICT (id) DO UPDATE SET name='Product Templates', parent_id=50, "
        "sequence=12, action_id=102");
    txn.exec(
        "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
        "(76, 'Attributes', 52, 25, 103) "
        "ON CONFLICT (id) DO UPDATE SET name='Attributes', parent_id=52, "
        "sequence=25, action_id=103");
    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");

    // docs/096 — pricelists under Products ▸ Configuration (52).
    txn.exec(
        "INSERT INTO ir_act_window (id, name, res_model, view_mode, path) VALUES "
        "(104, 'Pricelists', 'product.pricelist', 'list,form', 'pricelists') "
        "ON CONFLICT (id) DO UPDATE SET name='Pricelists', res_model='product.pricelist', "
        "view_mode='list,form', path='pricelists'");
    txn.exec(
        "INSERT INTO ir_act_window (id, name, res_model, view_mode, path) VALUES "
        "(105, 'Price Rules', 'product.pricelist.item', 'list', 'price-rules') "
        "ON CONFLICT (id) DO UPDATE SET name='Price Rules', res_model='product.pricelist.item', "
        "view_mode='list', path='price-rules'");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
    txn.exec(
        "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
        "(77, 'Pricelists', 52, 26, 104) "
        "ON CONFLICT (id) DO UPDATE SET name='Pricelists', parent_id=52, sequence=26, action_id=104");
    txn.exec(
        "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
        "(78, 'Price Rules', 52, 27, 105) "
        "ON CONFLICT (id) DO UPDATE SET name='Price Rules', parent_id=52, sequence=27, action_id=105");

    // docs/097 — the lookup review desk, under Products (50) next to the
    // parametric search it feeds.
    txn.exec(
        "INSERT INTO ir_act_window (id, name, res_model, view_mode, path) VALUES "
        "(106, 'Part Lookup', 'part.lookup', 'list', 'part-lookup') "
        "ON CONFLICT (id) DO UPDATE SET name='Part Lookup', res_model='part.lookup', "
        "view_mode='list', path='part-lookup'");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
    txn.exec(
        "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
        "(79, 'Part Lookup', 50, 16, 106) "
        "ON CONFLICT (id) DO UPDATE SET name='Part Lookup', parent_id=50, sequence=16, action_id=106");

    // docs/098 — the faceted catalogue browser, first entry under Products
    // because it is the screen you arrive at to find a part.
    txn.exec(
        "INSERT INTO ir_act_window (id, name, res_model, view_mode, path) VALUES "
        "(107, 'Parts Catalogue', 'part.catalog', 'list', 'part-catalog') "
        "ON CONFLICT (id) DO UPDATE SET name='Parts Catalogue', res_model='part.catalog', "
        "view_mode='list', path='part-catalog'");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
    txn.exec(
        "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
        "(86, 'Parts Catalogue', 50, 15, 107) "
        "ON CONFLICT (id) DO UPDATE SET name='Parts Catalogue', parent_id=50, sequence=15, action_id=107");
    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");

    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");

    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");


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

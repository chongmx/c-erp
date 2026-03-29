// =============================================================
// modules/product/ProductModule.cpp
// =============================================================
#include "ProductModule.hpp"
#include "BaseModel.hpp"
#include "BaseView.hpp"
#include "GenericViewModel.hpp"
#include "BaseViewModel.hpp"
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

    void registerFields() {
        fieldRegistry_.add({"name",      FieldType::Char,    "Name",    true});
        fieldRegistry_.add({"parent_id", FieldType::Many2one,"Parent Category",
                            false, false, true, true, "product.category"});
        fieldRegistry_.add({"active",    FieldType::Boolean, "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]   = name;
        j["active"] = active;
        j["parent_id"] = parentId > 0
            ? nlohmann::json::array({parentId, ""})
            : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")   && j["name"].is_string())   name   = j["name"].get<std::string>();
        if (j.contains("active") && j["active"].is_boolean()) active = j["active"].get<bool>();
        if (j.contains("parent_id")) {
            const auto& v = j["parent_id"];
            if (v.is_number_integer())           parentId = v.get<int>();
            else if (v.is_array() && v.size() >= 1 && v[0].is_number_integer())
                                                 parentId = v[0].get<int>();
            else                                 parentId = 0;
        }
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
    int         uomId           = 1;
    int         uomPoId         = 1;
    int         companyId       = 0;
    int         incomeAccountId = 0;
    int         expenseAccountId= 0;
    double      listPrice     = 0.0;
    double      standardPrice = 0.0;
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
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]           = name;
        j["default_code"]   = defaultCode;
        j["barcode"]        = barcode.empty()       ? nlohmann::json(false) : nlohmann::json(barcode);
        j["description"]    = description.empty()   ? nlohmann::json(false) : nlohmann::json(description);
        j["type"]           = type.empty() ? "consu" : type;
        j["categ_id"]       = categId  > 0 ? nlohmann::json::array({categId,  ""}) : nlohmann::json(false);
        j["uom_id"]         = uomId    > 0 ? nlohmann::json::array({uomId,    ""}) : nlohmann::json(false);
        j["uom_po_id"]      = uomPoId  > 0 ? nlohmann::json::array({uomPoId,  ""}) : nlohmann::json(false);
        j["company_id"]     = companyId> 0 ? nlohmann::json::array({companyId,""}) : nlohmann::json(false);
        j["list_price"]     = listPrice;
        j["standard_price"] = standardPrice;
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
        if (j.contains("categ_id"))   categId   = m2o(j["categ_id"]);
        if (j.contains("uom_id"))     uomId     = m2o(j["uom_id"]);
        if (j.contains("uom_po_id"))  uomPoId   = m2o(j["uom_po_id"]);
        if (j.contains("company_id")) companyId = m2o(j["company_id"]);
        if (j.contains("list_price")     && j["list_price"].is_number())     listPrice     = j["list_price"].get<double>();
        if (j.contains("standard_price") && j["standard_price"].is_number()) standardPrice = j["standard_price"].get<double>();
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
        REGISTER_METHOD("create",          handleCreate)
        REGISTER_METHOD("write",           handleWrite)
        REGISTER_METHOD("unlink",          handleUnlink)
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
                if (parentId > 0)
                    txn.exec("UPDATE product_category SET parent_id=$1, write_date=now() WHERE id=$2",
                             pqxx::params{parentId, id});
                else
                    txn.exec("UPDATE product_category SET parent_id=NULL, write_date=now() WHERE id=$1",
                             pqxx::params{id});
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
        for (int id : ids)
            txn.exec("DELETE FROM product_category WHERE id=$1", pqxx::params{id});
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
}

void ProductModule::registerViews() {
    views_.registerCreator("product.category.list", []{ return std::make_shared<ProductCategoryListView>(); });
    views_.registerCreator("product.category.form", []{ return std::make_shared<ProductCategoryFormView>(); });
    views_.registerCreator("product.product.list",  []{ return std::make_shared<ProductProductListView>(); });
    views_.registerCreator("product.product.form",  []{ return std::make_shared<ProductProductFormView>(); });
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
            (9,  'Products',           'product.product',  'list,form', 'products',            '{}'),
            (10, 'Product Categories', 'product.category', 'list,form', 'product-categories',  '{}')
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

    // Level 2: Categories under Configuration section (id=52, created by UomModule)
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (54, 'Categories', 52, 20, 10)
        ON CONFLICT (id) DO UPDATE
            SET action_id=EXCLUDED.action_id
    )");

    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");
    txn.commit();
}

} // namespace odoo::modules::product

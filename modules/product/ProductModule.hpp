#pragma once
// =============================================================
// modules/product/ProductModule.hpp
//
// Phase 8 — Product Catalog
//
// Models:  product.category  (table: product_category)
//          product.product   (table: product_product)
//
// Simplification: single-model — no template/variant split.
// Seeds:  3 product categories (All, Goods, Services)
// Menus:  Adds Products direct link (id=51) under the Products
//         app tile (id=50, created by UomModule) and Categories
//         leaf (id=54) under Configuration section (id=52).
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
        fieldRegistry_.add({"description_sale",    FieldType::Text,    "Sales Description"});
        fieldRegistry_.add({"description_purchase",FieldType::Text,    "Purchase Description"});
        fieldRegistry_.add({"income_account_id",   FieldType::Many2one,"Income Account",
                            false, false, true, true, "account.account"});
        fieldRegistry_.add({"expense_account_id",  FieldType::Many2one,"Expense Account",
                            false, false, true, true, "account.account"});
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
// 3. MODULE
// ================================================================

class ProductModule : public core::IModule {
public:
    explicit ProductModule(core::ModelFactory&     models,
                           core::ServiceFactory&   services,
                           core::ViewModelFactory& viewModels,
                           core::ViewFactory&      views)
        : models_(models), services_(services),
          viewModels_(viewModels), views_(views)
    {}

    static constexpr const char* staticModuleName() { return "product"; }
    std::string moduleName() const override { return "product"; }

    void registerModels() override {
        auto db = services_.db();
        models_.registerCreator("product.category", [db]{ return std::make_shared<ProductCategory>(db); });
        models_.registerCreator("product.product",  [db]{ return std::make_shared<ProductProduct>(db); });
    }

    void registerServices() override {}

    void registerViewModels() override {
        auto db = services_.db();
        viewModels_.registerCreator("product.category", [db]{
            return std::make_shared<GenericViewModel<ProductCategory>>(db);
        });
        viewModels_.registerCreator("product.product", [db]{
            return std::make_shared<GenericViewModel<ProductProduct>>(db);
        });
    }

    void registerViews() override {
        views_.registerCreator("product.category.list", []{ return std::make_shared<ProductCategoryListView>(); });
        views_.registerCreator("product.category.form", []{ return std::make_shared<ProductCategoryFormView>(); });
        views_.registerCreator("product.product.list",  []{ return std::make_shared<ProductProductListView>(); });
        views_.registerCreator("product.product.form",  []{ return std::make_shared<ProductProductFormView>(); });
    }

    void registerRoutes() override {}

    void initialize() override {
        ensureSchema_();
        seedCategories_();
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

        txn.commit();
    }

    // ----------------------------------------------------------
    // Seeds — 3 default product categories
    // ----------------------------------------------------------
    void seedCategories_() {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};

        txn.exec(R"(
            INSERT INTO product_category (id, name, parent_id) VALUES
                (1, 'All',      NULL),
                (2, 'Goods',    1),
                (3, 'Services', 1)
            ON CONFLICT (id) DO NOTHING
        )");
        txn.exec("SELECT setval('product_category_id_seq', (SELECT MAX(id) FROM product_category), true)");

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

    // ----------------------------------------------------------
    // Menus — adds products items under Products app tile (id=50)
    // ----------------------------------------------------------
    void seedMenus_() {
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
};

} // namespace odoo::modules::product

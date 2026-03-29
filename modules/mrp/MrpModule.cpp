// =============================================================
// modules/mrp/MrpModule.cpp
// =============================================================
#include "MrpModule.hpp"
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

    int         productId    = 0;
    std::string code;          // reference / internal reference
    std::string bomType      = "normal";  // normal | phantom
    double      productQty   = 1.0;
    int         productUomId = 0;
    int         companyId    = 0;
    bool        active       = true;

    explicit MrpBom(std::shared_ptr<DbConnection> db)
        : BaseModel<MrpBom>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"product_id",     FieldType::Many2one, "Product",       true,  false, true, false, "product.product"});
        fieldRegistry_.add({"code",           FieldType::Char,    "Reference"});
        fieldRegistry_.add({"bom_type",       FieldType::Char,    "BOM Type"});
        fieldRegistry_.add({"product_qty",    FieldType::Float,   "Quantity"});
        fieldRegistry_.add({"product_uom_id", FieldType::Many2one,"Unit of Measure",false, false, true, false, "uom.uom"});
        fieldRegistry_.add({"company_id",     FieldType::Many2one,"Company",         false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",         FieldType::Boolean, "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["product_id"]     = productId    > 0 ? nlohmann::json(productId)    : nlohmann::json(false);
        j["code"]           = code;
        j["bom_type"]       = bomType;
        j["product_qty"]    = productQty;
        j["product_uom_id"] = productUomId > 0 ? nlohmann::json(productUomId) : nlohmann::json(false);
        j["company_id"]     = companyId    > 0 ? nlohmann::json(companyId)    : nlohmann::json(false);
        j["active"]         = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("product_id"))     productId    = mrpM2oId(j["product_id"]);
        if (j.contains("code")           && j["code"].is_string())           code        = j["code"].get<std::string>();
        if (j.contains("bom_type")       && j["bom_type"].is_string())       bomType     = j["bom_type"].get<std::string>();
        if (j.contains("product_qty")    && j["product_qty"].is_number())    productQty  = j["product_qty"].get<double>();
        if (j.contains("product_uom_id")) productUomId = mrpM2oId(j["product_uom_id"]);
        if (j.contains("company_id"))     companyId    = mrpM2oId(j["company_id"]);
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
        REGISTER_METHOD("create",       handleCreate)
        REGISTER_METHOD("write",        handleWrite)
        REGISTER_METHOD("unlink",       handleUnlink)
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
            j["product_qty"]    = row["product_qty"].as<double>(1.0);
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
            j["product_qty"]    = row["product_qty"].as<double>(1.0);
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
        return proto.create(call.arg(0));
    }

    nlohmann::json handleWrite(const CallKwArgs& call) {
        MrpBom proto(db_);
        return proto.write(call.ids(), call.arg(1));
    }

    nlohmann::json handleUnlink(const CallKwArgs& call) {
        // Also remove lines
        const auto ids = call.ids();
        if (ids.empty()) return true;
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
        return proto.fieldsGet(call.fields());
    }

    nlohmann::json handleSearchCount(const CallKwArgs& call) {
        MrpBom proto(db_);
        return proto.searchCount(call.domain());
    }

    nlohmann::json handleSearch(const CallKwArgs& call) {
        MrpBom proto(db_);
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
        REGISTER_METHOD("create",       handleCreate)
        REGISTER_METHOD("write",        handleWrite)
        REGISTER_METHOD("unlink",       handleUnlink)
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
            j["product_qty"]    = row["product_qty"].as<double>(1.0);
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
        return proto.read(call.ids());
    }

    nlohmann::json handleCreate(const CallKwArgs& call) {
        MrpBomLine proto(db_);
        return proto.create(call.arg(0));
    }

    nlohmann::json handleWrite(const CallKwArgs& call) {
        MrpBomLine proto(db_);
        return proto.write(call.ids(), call.arg(1));
    }

    nlohmann::json handleUnlink(const CallKwArgs& call) {
        MrpBomLine proto(db_);
        return proto.unlink(call.ids());
    }

    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        MrpBomLine proto(db_);
        return proto.fieldsGet(call.fields());
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

std::string MrpModule::moduleName() const { return "mrp"; }

void MrpModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("mrp.bom",      [db]{ return std::make_shared<MrpBom>(db); });
    models_.registerCreator("mrp.bom.line", [db]{ return std::make_shared<MrpBomLine>(db); });
}

void MrpModule::registerServices() {}

void MrpModule::registerViewModels() {
    auto db = services_.db();
    viewModels_.registerCreator("mrp.bom",      [db]{ return std::make_shared<MrpBomViewModel>(db); });
    viewModels_.registerCreator("mrp.bom.line", [db]{ return std::make_shared<MrpBomLineViewModel>(db); });
}

void MrpModule::registerViews() {
    views_.registerCreator("mrp.bom.list", []{ return std::make_shared<MrpBomListView>(); });
    views_.registerCreator("mrp.bom.form", []{ return std::make_shared<MrpBomFormView>(); });
}

void MrpModule::registerRoutes() {}

void MrpModule::initialize() {
    ensureSchema_();
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

    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");
    txn.commit();
}

} // namespace odoo::modules::mrp

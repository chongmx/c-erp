#include "PurchaseModule.hpp"
// =============================================================
// modules/purchase/PurchaseModule.hpp
//
// Phase 10 — Purchase module
//
// Models:  purchase.order      (table: purchase_order)
//          purchase.order.line (table: purchase_order_line)
//
// State machine: draft → purchase → cancel
// ViewModels:
//   PurchaseOrderViewModel  — action_confirm (PO/YYYY/NNNN),
//                             action_cancel, action_create_bills
//   PurchaseOrderLineViewModel — recomputes line amounts on
//                                create/write; updates parent order
// Seeds: ir_act_window id=12, ir_ui_menu id=70 (Purchase app),
//        id=71 (Orders section), id=72 (Purchase Orders leaf)
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include "BaseModel.hpp"
#include "BaseView.hpp"
#include "BaseViewModel.hpp"
#include "DbConnection.hpp"
#include "MailHelpers.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cmath>

namespace odoo::modules::purchase {

using namespace odoo::infrastructure;
using namespace odoo::core;

// ----------------------------------------------------------------
// helpers
// ----------------------------------------------------------------
namespace {

inline int purM2oId(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer())
        return v[0].get<int>();
    return 0;
}

inline std::string purCurrentDate() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::gmtime(&t));
    return std::string(buf);
}

inline std::string purIdsArray(const std::vector<int>& ids) {
    std::string s = "{";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(ids[i]);
    }
    return s + "}";
}

} // anonymous namespace


// ================================================================
// 1. MODELS
// ================================================================

// ----------------------------------------------------------------
// PurchaseOrder — purchase.order
// ----------------------------------------------------------------
class PurchaseOrder : public BaseModel<PurchaseOrder> {
public:
    static constexpr const char* MODEL_NAME = "purchase.order";
    static constexpr const char* TABLE_NAME = "purchase_order";

    explicit PurchaseOrder(std::shared_ptr<DbConnection> db)
        : BaseModel(std::move(db)) {}

    std::string name          = "New";
    std::string state         = "draft";
    int         partnerId     = 0;
    std::string dateOrder;
    std::string datePlanned;
    int         paymentTermId = 0;
    std::string note;
    int         currencyId    = 0;
    int         companyId     = 1;
    int         userId        = 0;
    std::string origin;
    std::string invoiceStatus = "nothing";
    double      amountUntaxed = 0.0;
    double      amountTax     = 0.0;
    double      amountTotal   = 0.0;

    void registerFields() {
        fieldRegistry_.add({"name",            FieldType::Char,      "Order Reference"});
        fieldRegistry_.add({"state",           FieldType::Selection, "Status",             false, true});
        fieldRegistry_.add({"partner_id",      FieldType::Many2one,  "Vendor",             true,  false, true, false, "res.partner"});
        fieldRegistry_.add({"date_order",      FieldType::Date,      "Order Date",         true});
        fieldRegistry_.add({"date_planned",    FieldType::Date,      "Expected Arrival"});
        fieldRegistry_.add({"payment_term_id", FieldType::Many2one,  "Payment Terms",      false, false, true, false, "account.payment.term"});
        fieldRegistry_.add({"note",            FieldType::Text,      "Terms and Conditions"});
        fieldRegistry_.add({"currency_id",     FieldType::Many2one,  "Currency",           false, false, true, false, "res.currency"});
        fieldRegistry_.add({"company_id",      FieldType::Many2one,  "Company",            true,  false, true, false, "res.company"});
        fieldRegistry_.add({"user_id",         FieldType::Many2one,  "Purchase Rep.",      false, false, true, false, "res.users"});
        fieldRegistry_.add({"origin",          FieldType::Char,      "Source"});
        fieldRegistry_.add({"invoice_status",  FieldType::Selection, "Billing Status",     false, true});
        fieldRegistry_.add({"amount_untaxed",  FieldType::Monetary,  "Untaxed Amount",     false, true});
        fieldRegistry_.add({"amount_tax",      FieldType::Monetary,  "Taxes",              false, true});
        fieldRegistry_.add({"amount_total",    FieldType::Monetary,  "Total",              false, true});
        fieldRegistry_.add({"order_line", FieldType::One2many, "Order Lines", false, false, false, false, "purchase.order.line", "order_id"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]            = name;
        j["state"]           = state;
        j["partner_id"]      = partnerId     > 0 ? nlohmann::json(partnerId)     : nlohmann::json(false);
        j["date_order"]      = dateOrder.empty()   ? nlohmann::json(purCurrentDate()) : nlohmann::json(dateOrder);
        j["date_planned"]    = datePlanned.empty() ? nlohmann::json(false)            : nlohmann::json(datePlanned);
        j["payment_term_id"] = paymentTermId > 0 ? nlohmann::json(paymentTermId) : nlohmann::json(false);
        j["note"]            = note.empty()        ? nlohmann::json(false)            : nlohmann::json(note);
        j["currency_id"]     = currencyId    > 0 ? nlohmann::json(currencyId)    : nlohmann::json(false);
        j["company_id"]      = companyId     > 0 ? nlohmann::json(companyId)     : nlohmann::json(false);
        j["user_id"]         = userId        > 0 ? nlohmann::json(userId)        : nlohmann::json(false);
        j["origin"]          = origin.empty()      ? nlohmann::json(false)            : nlohmann::json(origin);
        j["invoice_status"]  = invoiceStatus;
        j["amount_untaxed"]  = amountUntaxed;
        j["amount_tax"]      = amountTax;
        j["amount_total"]    = amountTotal;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")           && j["name"].is_string())           name          = j["name"].get<std::string>();
        if (j.contains("state")          && j["state"].is_string())          state         = j["state"].get<std::string>();
        if (j.contains("partner_id"))     partnerId     = purM2oId(j["partner_id"]);
        if (j.contains("date_order")     && j["date_order"].is_string())     dateOrder     = j["date_order"].get<std::string>();
        if (j.contains("date_planned")   && j["date_planned"].is_string())   datePlanned   = j["date_planned"].get<std::string>();
        if (j.contains("payment_term_id")) paymentTermId = purM2oId(j["payment_term_id"]);
        if (j.contains("note")           && j["note"].is_string())           note          = j["note"].get<std::string>();
        if (j.contains("currency_id"))    currencyId    = purM2oId(j["currency_id"]);
        if (j.contains("company_id"))     companyId     = purM2oId(j["company_id"]);
        if (j.contains("user_id"))        userId        = purM2oId(j["user_id"]);
        if (j.contains("origin")         && j["origin"].is_string())         origin        = j["origin"].get<std::string>();
        if (j.contains("invoice_status") && j["invoice_status"].is_string()) invoiceStatus = j["invoice_status"].get<std::string>();
        if (j.contains("amount_untaxed") && j["amount_untaxed"].is_number()) amountUntaxed = j["amount_untaxed"].get<double>();
        if (j.contains("amount_tax")     && j["amount_tax"].is_number())     amountTax     = j["amount_tax"].get<double>();
        if (j.contains("amount_total")   && j["amount_total"].is_number())   amountTotal   = j["amount_total"].get<double>();
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
        if (partnerId <= 0) e.push_back("Vendor is required");
        return e;
    }
};

// ----------------------------------------------------------------
// PurchaseOrderLine — purchase.order.line
// ----------------------------------------------------------------
class PurchaseOrderLine : public BaseModel<PurchaseOrderLine> {
public:
    static constexpr const char* MODEL_NAME = "purchase.order.line";
    static constexpr const char* TABLE_NAME = "purchase_order_line";

    explicit PurchaseOrderLine(std::shared_ptr<DbConnection> db)
        : BaseModel(std::move(db)) {}

    int         orderId       = 0;
    int         sequence      = 10;
    int         productId     = 0;
    std::string name;
    double      productQty    = 1.0;
    int         productUomId  = 1;
    double      priceUnit     = 0.0;
    double      discount      = 0.0;
    std::string taxIdsJson    = "[]";
    double      priceSubtotal = 0.0;
    double      priceTax      = 0.0;
    double      priceTotal    = 0.0;
    std::string datePlanned;
    double      qtyInvoiced   = 0.0;
    double      qtyReceived   = 0.0;
    int         companyId     = 0;
    int         currencyId    = 0;

    void registerFields() {
        fieldRegistry_.add({"order_id",       FieldType::Many2one,  "Order",           true,  false, true, false, "purchase.order"});
        fieldRegistry_.add({"sequence",       FieldType::Integer,   "Sequence"});
        fieldRegistry_.add({"product_id",     FieldType::Many2one,  "Product",         false, false, true, false, "product.product"});
        fieldRegistry_.add({"name",           FieldType::Text,      "Description",     true});
        fieldRegistry_.add({"product_qty",    FieldType::Float,     "Quantity"});
        fieldRegistry_.add({"product_uom_id", FieldType::Many2one,  "Unit of Measure", false, false, true, false, "uom.uom"});
        fieldRegistry_.add({"price_unit",     FieldType::Monetary,  "Unit Price"});
        fieldRegistry_.add({"discount",       FieldType::Float,     "Disc.%"});
        fieldRegistry_.add({"tax_ids_json",   FieldType::Text,      "Taxes (JSON)"});
        fieldRegistry_.add({"price_subtotal", FieldType::Monetary,  "Subtotal",        false, true});
        fieldRegistry_.add({"price_tax",      FieldType::Monetary,  "Tax",             false, true});
        fieldRegistry_.add({"price_total",    FieldType::Monetary,  "Total",           false, true});
        fieldRegistry_.add({"date_planned",   FieldType::Date,      "Expected Arrival"});
        fieldRegistry_.add({"qty_invoiced",   FieldType::Float,     "Billed Qty",      false, true});
        fieldRegistry_.add({"qty_received",   FieldType::Float,     "Received Qty",    false, true});
        fieldRegistry_.add({"company_id",     FieldType::Many2one,  "Company",         false, false, true, false, "res.company"});
        fieldRegistry_.add({"currency_id",    FieldType::Many2one,  "Currency",        false, false, true, false, "res.currency"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["order_id"]       = orderId      > 0 ? nlohmann::json(orderId)      : nlohmann::json(false);
        j["sequence"]       = sequence;
        j["product_id"]     = productId    > 0 ? nlohmann::json(productId)    : nlohmann::json(false);
        j["name"]           = name;
        j["product_qty"]    = productQty;
        j["product_uom_id"] = productUomId > 0 ? nlohmann::json(productUomId) : nlohmann::json(false);
        j["price_unit"]     = priceUnit;
        j["discount"]       = discount;
        j["tax_ids_json"]   = taxIdsJson;
        j["price_subtotal"] = priceSubtotal;
        j["price_tax"]      = priceTax;
        j["price_total"]    = priceTotal;
        j["date_planned"]   = datePlanned.empty() ? nlohmann::json(false) : nlohmann::json(datePlanned);
        j["qty_invoiced"]   = qtyInvoiced;
        j["qty_received"]   = qtyReceived;
        j["company_id"]     = companyId    > 0 ? nlohmann::json(companyId)    : nlohmann::json(false);
        j["currency_id"]    = currencyId   > 0 ? nlohmann::json(currencyId)   : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("order_id"))        orderId      = purM2oId(j["order_id"]);
        if (j.contains("sequence")      && j["sequence"].is_number())      sequence     = j["sequence"].get<int>();
        if (j.contains("product_id"))      productId    = purM2oId(j["product_id"]);
        if (j.contains("name")          && j["name"].is_string())          name         = j["name"].get<std::string>();
        if (j.contains("product_qty")   && j["product_qty"].is_number())   productQty   = j["product_qty"].get<double>();
        if (j.contains("product_uom_id")) productUomId = purM2oId(j["product_uom_id"]);
        if (j.contains("price_unit")    && j["price_unit"].is_number())    priceUnit    = j["price_unit"].get<double>();
        if (j.contains("discount")      && j["discount"].is_number())      discount     = j["discount"].get<double>();
        if (j.contains("tax_ids_json")  && j["tax_ids_json"].is_string())  taxIdsJson   = j["tax_ids_json"].get<std::string>();
        if (j.contains("price_subtotal")&& j["price_subtotal"].is_number())priceSubtotal= j["price_subtotal"].get<double>();
        if (j.contains("price_tax")     && j["price_tax"].is_number())     priceTax     = j["price_tax"].get<double>();
        if (j.contains("price_total")   && j["price_total"].is_number())   priceTotal   = j["price_total"].get<double>();
        if (j.contains("date_planned")  && j["date_planned"].is_string())  datePlanned  = j["date_planned"].get<std::string>();
        if (j.contains("qty_invoiced")  && j["qty_invoiced"].is_number())  qtyInvoiced  = j["qty_invoiced"].get<double>();
        if (j.contains("qty_received")  && j["qty_received"].is_number())  qtyReceived  = j["qty_received"].get<double>();
        if (j.contains("company_id"))      companyId    = purM2oId(j["company_id"]);
        if (j.contains("currency_id"))     currencyId   = purM2oId(j["currency_id"]);
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
        if (orderId <= 0) e.push_back("Order is required");
        if (name.empty())  e.push_back("Description is required");
        return e;
    }
};


// ================================================================
// 2. VIEWS
// ================================================================

class PurchaseOrderListView : public core::BaseView {
public:
    std::string viewName()  const override { return "purchase.order.list"; }
    std::string modelName() const override { return "purchase.order"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Purchase Orders\">"
               "<field name=\"name\"/>"
               "<field name=\"partner_id\"/>"
               "<field name=\"date_order\"/>"
               "<field name=\"date_planned\"/>"
               "<field name=\"state\"/>"
               "<field name=\"invoice_status\"/>"
               "<field name=\"amount_untaxed\"/>"
               "<field name=\"amount_total\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",           {{"type","char"},      {"string","Order Reference"}}},
            {"partner_id",     {{"type","many2one"},  {"string","Vendor"},         {"relation","res.partner"}}},
            {"date_order",     {{"type","date"},      {"string","Order Date"}}},
            {"date_planned",   {{"type","date"},      {"string","Expected Arrival"}}},
            {"state",          {{"type","selection"}, {"string","Status"}}},
            {"invoice_status", {{"type","selection"}, {"string","Billing Status"}}},
            {"amount_untaxed", {{"type","monetary"},  {"string","Untaxed Amount"}}},
            {"amount_total",   {{"type","monetary"},  {"string","Total"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class PurchaseOrderFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "purchase.order.form"; }
    std::string modelName() const override { return "purchase.order"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Purchase Order\">"
               "<field name=\"name\"/>"
               "<field name=\"state\"/>"
               "<field name=\"partner_id\"/>"
               "<field name=\"date_order\"/>"
               "<field name=\"date_planned\"/>"
               "<field name=\"payment_term_id\"/>"
               "<field name=\"currency_id\"/>"
               "<field name=\"user_id\"/>"
               "<field name=\"origin\"/>"
               "<field name=\"invoice_status\"/>"
               "<field name=\"amount_untaxed\"/>"
               "<field name=\"amount_tax\"/>"
               "<field name=\"amount_total\"/>"
               "<field name=\"note\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",            {{"type","char"},      {"string","Order Reference"}}},
            {"state",           {{"type","selection"}, {"string","Status"}}},
            {"partner_id",      {{"type","many2one"},  {"string","Vendor"},          {"relation","res.partner"}}},
            {"date_order",      {{"type","date"},      {"string","Order Date"}}},
            {"date_planned",    {{"type","date"},      {"string","Expected Arrival"}}},
            {"payment_term_id", {{"type","many2one"},  {"string","Payment Terms"},   {"relation","account.payment.term"}}},
            {"currency_id",     {{"type","many2one"},  {"string","Currency"},        {"relation","res.currency"}}},
            {"user_id",         {{"type","many2one"},  {"string","Purchase Rep."},   {"relation","res.users"}}},
            {"origin",          {{"type","char"},      {"string","Source"}}},
            {"invoice_status",  {{"type","selection"}, {"string","Billing Status"}}},
            {"amount_untaxed",  {{"type","monetary"},  {"string","Untaxed Amount"}}},
            {"amount_tax",      {{"type","monetary"},  {"string","Taxes"}}},
            {"amount_total",    {{"type","monetary"},  {"string","Total"}}},
            {"note",            {{"type","text"},      {"string","Terms and Conditions"}}},
            {"order_line",      {{"type","one2many"},  {"string","Order Lines"},
                                 {"relation","purchase.order.line"}, {"relation_field","order_id"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class PurchaseOrderLineListView : public core::BaseView {
public:
    std::string viewName()  const override { return "purchase.order.line.list"; }
    std::string modelName() const override { return "purchase.order.line"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Order Lines\">"
               "<field name=\"order_id\"/>"
               "<field name=\"product_id\"/>"
               "<field name=\"name\"/>"
               "<field name=\"product_qty\"/>"
               "<field name=\"price_unit\"/>"
               "<field name=\"discount\"/>"
               "<field name=\"price_subtotal\"/>"
               "<field name=\"price_total\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"order_id",       {{"type","many2one"},  {"string","Order"},       {"relation","purchase.order"}}},
            {"product_id",     {{"type","many2one"},  {"string","Product"},     {"relation","product.product"}}},
            {"name",           {{"type","text"},      {"string","Description"}}},
            {"product_qty",    {{"type","float"},     {"string","Quantity"}}},
            {"price_unit",     {{"type","monetary"},  {"string","Unit Price"}}},
            {"discount",       {{"type","float"},     {"string","Disc.%"}}},
            {"price_subtotal", {{"type","monetary"},  {"string","Subtotal"}}},
            {"price_total",    {{"type","monetary"},  {"string","Total"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class PurchaseOrderLineFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "purchase.order.line.form"; }
    std::string modelName() const override { return "purchase.order.line"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Order Line\">"
               "<field name=\"order_id\"/>"
               "<field name=\"sequence\"/>"
               "<field name=\"product_id\"/>"
               "<field name=\"name\"/>"
               "<field name=\"product_qty\"/>"
               "<field name=\"product_uom_id\"/>"
               "<field name=\"price_unit\"/>"
               "<field name=\"discount\"/>"
               "<field name=\"tax_ids_json\"/>"
               "<field name=\"date_planned\"/>"
               "<field name=\"price_subtotal\"/>"
               "<field name=\"price_tax\"/>"
               "<field name=\"price_total\"/>"
               "<field name=\"qty_invoiced\"/>"
               "<field name=\"qty_received\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"order_id",       {{"type","many2one"},  {"string","Order"},           {"relation","purchase.order"}}},
            {"sequence",       {{"type","integer"},   {"string","Sequence"}}},
            {"product_id",     {{"type","many2one"},  {"string","Product"},         {"relation","product.product"}}},
            {"name",           {{"type","text"},      {"string","Description"}}},
            {"product_qty",    {{"type","float"},     {"string","Quantity"}}},
            {"product_uom_id", {{"type","many2one"},  {"string","Unit of Measure"}, {"relation","uom.uom"}}},
            {"price_unit",     {{"type","monetary"},  {"string","Unit Price"}}},
            {"discount",       {{"type","float"},     {"string","Disc.%"}}},
            {"tax_ids_json",   {{"type","text"},      {"string","Taxes (JSON)"}}},
            {"date_planned",   {{"type","date"},      {"string","Expected Arrival"}}},
            {"price_subtotal", {{"type","monetary"},  {"string","Subtotal"}}},
            {"price_tax",      {{"type","monetary"},  {"string","Tax"}}},
            {"price_total",    {{"type","monetary"},  {"string","Total"}}},
            {"qty_invoiced",   {{"type","float"},     {"string","Billed Qty"}}},
            {"qty_received",   {{"type","float"},     {"string","Received Qty"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};


// ================================================================
// 3. VIEWMODELS
// ================================================================

// ----------------------------------------------------------------
// PurchaseViewModel<T> — generic CRUD base
// ----------------------------------------------------------------
template<typename TModel>
class PurchaseViewModel : public BaseViewModel {
public:
    explicit PurchaseViewModel(std::shared_ptr<DbConnection> db)
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
    }

    std::string modelName() const override { return TModel::MODEL_NAME; }

protected:
    std::shared_ptr<DbConnection> db_;

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        TModel proto(db_);
        return proto.searchRead(call.domain(), call.fields(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "id ASC");
    }
    nlohmann::json handleRead(const CallKwArgs& call) {
        TModel proto(db_);
        return proto.read(call.ids(), call.fields());
    }
    nlohmann::json handleCreate(const CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("create: args[0] must be a dict");
        TModel proto(db_);
        return proto.create(v);
    }
    nlohmann::json handleWrite(const CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        TModel proto(db_);
        return proto.write(call.ids(), v);
    }
    nlohmann::json handleUnlink(const CallKwArgs& call) {
        TModel proto(db_);
        return proto.unlink(call.ids());
    }
    nlohmann::json handleFieldsGet(const CallKwArgs& call) {
        TModel proto(db_);
        return proto.fieldsGet(call.fields());
    }
    nlohmann::json handleSearchCount(const CallKwArgs& call) {
        TModel proto(db_);
        return proto.searchCount(call.domain());
    }
    nlohmann::json handleSearch(const CallKwArgs& call) {
        TModel proto(db_);
        auto ids = proto.search(call.domain(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "id ASC");
        nlohmann::json arr = nlohmann::json::array();
        for (int id : ids) arr.push_back(id);
        return arr;
    }
};


// ----------------------------------------------------------------
// PurchaseOrderViewModel — action_confirm, action_cancel,
//                          action_create_bills
// ----------------------------------------------------------------
class PurchaseOrderViewModel : public PurchaseViewModel<PurchaseOrder> {
public:
    explicit PurchaseOrderViewModel(std::shared_ptr<DbConnection> db)
        : PurchaseViewModel<PurchaseOrder>(std::move(db))
    {
        REGISTER_METHOD("action_confirm",             handleActionConfirm)
        REGISTER_METHOD("action_cancel",              handleActionCancel)
        REGISTER_METHOD("action_create_bills",        handleActionCreateBills)
        REGISTER_METHOD("action_create_down_payment", handleActionCreateDownPayment)
    }

    std::string modelName() const override { return "purchase.order"; }

private:
    // ----------------------------------------------------------
    // action_confirm — draft → purchase, assign PO/YYYY/NNNN
    // ----------------------------------------------------------
    nlohmann::json handleActionConfirm(const CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int id : ids) {
            auto r = txn.exec(
                "SELECT state FROM purchase_order WHERE id = $1",
                pqxx::params{id});
            if (r.empty()) throw std::runtime_error("Purchase order not found: " + std::to_string(id));
            std::string st = r[0][0].c_str();
            if (st != "draft")
                throw std::runtime_error("Only draft orders can be confirmed");

            std::string year = []{
                std::time_t t = std::time(nullptr);
                char buf[8];
                std::strftime(buf, sizeof(buf), "%Y", std::gmtime(&t));
                return std::string(buf);
            }();

            auto cnt = txn.exec("SELECT nextval('purchase_order_seq')");
            long long seq = cnt[0][0].as<long long>();

            std::ostringstream ss;
            ss << "PO/" << year << "/" << std::setfill('0') << std::setw(4) << seq;

            txn.exec(
                "UPDATE purchase_order "
                "SET state = 'purchase', name = $1, write_date = now() "
                "WHERE id = $2",
                pqxx::params{ss.str(), id});

            // --------------------------------------------------
            // Phase 16: auto-create receipt picking
            // --------------------------------------------------
            auto ptRow = txn.exec(
                "SELECT id, default_location_src_id, default_location_dest_id "
                "FROM stock_picking_type WHERE code='incoming' LIMIT 1");
            if (!ptRow.empty()) {
                int ptId   = ptRow[0]["id"].as<int>();
                int locSrc = ptRow[0]["default_location_src_id"].is_null()  ? 5 : ptRow[0]["default_location_src_id"].as<int>();
                int locDst = ptRow[0]["default_location_dest_id"].is_null() ? 4 : ptRow[0]["default_location_dest_id"].as<int>();

                auto poRow = txn.exec(
                    "SELECT partner_id, company_id FROM purchase_order WHERE id=$1",
                    pqxx::params{id});
                int partnerId = poRow[0]["partner_id"].is_null() ? 0 : poRow[0]["partner_id"].as<int>();
                int companyId = poRow[0]["company_id"].is_null() ? 0 : poRow[0]["company_id"].as<int>();

                auto seqRow  = txn.exec("SELECT nextval('stock_in_seq')");
                long long ps = seqRow[0][0].as<long long>();
                std::string pickName = "WH/IN/" + year + "/" +
                    std::string(4 - std::min(4, (int)std::to_string(ps).size()), '0') + std::to_string(ps);

                auto pickRow = txn.exec(
                    "INSERT INTO stock_picking "
                    "(name,picking_type_id,state,partner_id,location_id,location_dest_id,origin,company_id,purchase_id) "
                    "VALUES($1,$2,'confirmed',NULLIF($3,0),$4,$5,$6,NULLIF($7,0),$8) RETURNING id",
                    pqxx::params{pickName, ptId, partnerId, locSrc, locDst, ss.str(), companyId, id});
                int pickId = pickRow[0]["id"].as<int>();

                txn.exec(
                    "INSERT INTO stock_move "
                    "(picking_id,product_id,name,product_uom_qty,quantity,state,location_id,location_dest_id,company_id,origin) "
                    "SELECT $1,pol.product_id,pp.name,pol.product_qty,0,'confirmed',$2,$3,NULLIF($4,0),$5 "
                    "FROM purchase_order_line pol "
                    "JOIN product_product pp ON pp.id=pol.product_id "
                    "WHERE pol.order_id=$6",
                    pqxx::params{pickId, locSrc, locDst, companyId, ss.str(), id});
            }
        }

        for (int id : ids)
            odoo::modules::mail::postLog(txn, "purchase.order", id, 0,
                "Purchase order confirmed.", "log_note");
        txn.commit();
        return true;
    }

    // ----------------------------------------------------------
    // action_cancel — draft → cancel
    // ----------------------------------------------------------
    nlohmann::json handleActionCancel(const CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(
            "UPDATE purchase_order SET state = 'cancel', write_date = now() "
            "WHERE id = ANY($1::int[]) AND state = 'draft'",
            pqxx::params{purIdsArray(ids)});
        for (int id : ids)
            odoo::modules::mail::postLog(txn, "purchase.order", id, 0,
                "Purchase order cancelled.", "log_note");
        txn.commit();
        return true;
    }

    // ----------------------------------------------------------
    // action_create_bills — confirmed → account.move in_invoice
    // ----------------------------------------------------------
    nlohmann::json handleActionCreateBills(const CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int ordId : ids) {
            // Load order
            auto r = txn.exec(
                "SELECT state, partner_id, payment_term_id, company_id, "
                "       currency_id, amount_untaxed, amount_tax, amount_total, name "
                "FROM purchase_order WHERE id = $1",
                pqxx::params{ordId});
            if (r.empty()) throw std::runtime_error("Purchase order not found");

            std::string state      = r[0][0].c_str();
            int         partnerId  = r[0][1].is_null() ? 0 : r[0][1].as<int>();
            int         payTermId  = r[0][2].is_null() ? 0 : r[0][2].as<int>();
            int         companyId  = r[0][3].as<int>();
            int         currencyId = r[0][4].is_null() ? 0 : r[0][4].as<int>();
            double      amtUntaxed = r[0][5].as<double>();
            double      amtTax     = r[0][6].as<double>();
            double      amtTotal   = r[0][7].as<double>();
            std::string orderName  = r[0][8].c_str();

            if (state != "purchase")
                throw std::runtime_error("Only confirmed orders can be billed");

            // Find PUR journal
            auto jrow = txn.exec(
                "SELECT id FROM account_journal "
                "WHERE type = 'purchase' AND company_id = $1 AND active = TRUE "
                "ORDER BY id LIMIT 1",
                pqxx::params{companyId});
            if (jrow.empty()) throw std::runtime_error("No purchase journal found");
            int journalId = jrow[0][0].as<int>();

            // Find expense account (account_type = 'expense' or 'expense_direct_cost')
            auto expRow = txn.exec(
                "SELECT id FROM account_account "
                "WHERE account_type IN ('expense','expense_direct_cost') "
                "  AND company_id = $1 AND active = TRUE "
                "ORDER BY code LIMIT 1",
                pqxx::params{companyId});
            if (expRow.empty()) throw std::runtime_error("No expense account found");
            int expAccId = expRow[0][0].as<int>();

            // Find AP account (liability_payable)
            auto apRow = txn.exec(
                "SELECT id FROM account_account "
                "WHERE account_type = 'liability_payable' "
                "  AND company_id = $1 AND active = TRUE "
                "ORDER BY code LIMIT 1",
                pqxx::params{companyId});
            if (apRow.empty()) throw std::runtime_error("No payable account found");
            int apAccId = apRow[0][0].as<int>();

            std::string today = purCurrentDate();

            // Create account.move (in_invoice, draft)
            pqxx::params mp;
            mp.append(journalId);
            if (partnerId  > 0) mp.append(partnerId);  else mp.append(nullptr);
            if (payTermId  > 0) mp.append(payTermId);  else mp.append(nullptr);
            mp.append(companyId);
            if (currencyId > 0) mp.append(currencyId); else mp.append(nullptr);
            mp.append(today);
            mp.append(amtUntaxed);
            mp.append(amtTax);
            mp.append(amtTotal);
            mp.append(amtTotal);  // amount_residual
            mp.append(orderName); // ref
            mp.append(ordId);     // purchase_id

            auto mvRow = txn.exec(
                "INSERT INTO account_move "
                "(move_type, state, date, journal_id, partner_id, "
                " payment_term_id, company_id, currency_id, invoice_date, "
                " amount_untaxed, amount_tax, amount_total, amount_residual, ref, purchase_id) "
                "VALUES ('in_invoice','draft',$6,$1,$2,$3,$4,$5,$6,"
                "        $7,$8,$9,$10,$11,$12) "
                "RETURNING id",
                mp);
            int moveId = mvRow[0][0].as<int>();

            // Create move lines from purchase order lines
            auto lines = txn.exec(
                "SELECT name, product_qty, price_unit, "
                "       price_subtotal, price_tax "
                "FROM purchase_order_line WHERE order_id = $1",
                pqxx::params{ordId});

            for (const auto& ln : lines) {
                std::string lname = ln[0].c_str();
                double      qty   = ln[1].as<double>();
                double      unit  = ln[2].as<double>();
                double      sub   = ln[3].as<double>();
                double      tax   = ln[4].as<double>();

                // Expense (debit) line
                pqxx::params el;
                el.append(moveId);
                el.append(expAccId);
                el.append(journalId);
                el.append(companyId);
                el.append(today);
                el.append(lname);
                if (partnerId > 0) el.append(partnerId); else el.append(nullptr);
                el.append(qty);
                el.append(unit);  // price_unit
                el.append(sub);   // debit
                el.append(0.0);   // credit
                txn.exec(
                    "INSERT INTO account_move_line "
                    "(move_id, account_id, journal_id, company_id, date, "
                    " name, partner_id, quantity, price_unit, debit, credit) "
                    "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)",
                    el);

                // Tax line if any
                if (tax > 0.001) {
                    pqxx::params tl;
                    tl.append(moveId);
                    tl.append(expAccId);  // simplified: use same expense account
                    tl.append(journalId);
                    tl.append(companyId);
                    tl.append(today);
                    tl.append("Tax: " + lname);
                    if (partnerId > 0) tl.append(partnerId); else tl.append(nullptr);
                    tl.append(1.0);
                    tl.append(tax);   // debit
                    tl.append(0.0);
                    txn.exec(
                        "INSERT INTO account_move_line "
                        "(move_id, account_id, journal_id, company_id, date, "
                        " name, partner_id, quantity, debit, credit) "
                        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
                        tl);
                }
            }

            // Payable (credit) line — total amount
            pqxx::params pl;
            pl.append(moveId);
            pl.append(apAccId);
            pl.append(journalId);
            pl.append(companyId);
            pl.append(today);
            pl.append("Payable: " + orderName);
            if (partnerId > 0) pl.append(partnerId); else pl.append(nullptr);
            pl.append(1.0);
            pl.append(0.0);       // debit
            pl.append(amtTotal);  // credit
            txn.exec(
                "INSERT INTO account_move_line "
                "(move_id, account_id, journal_id, company_id, date, "
                " name, partner_id, quantity, debit, credit) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
                pl);

            // Mark order as billed
            txn.exec(
                "UPDATE purchase_order SET invoice_status = 'billed', write_date = now() "
                "WHERE id = $1",
                pqxx::params{ordId});
        }

        txn.commit();
        return true;
    }

    // ----------------------------------------------------------
    // action_create_down_payment — create a partial advance bill
    // kwargs: amount (numeric), note (optional string)
    // ----------------------------------------------------------
    nlohmann::json handleActionCreateDownPayment(const CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        double dpAmount = 0.0;
        if (call.kwargs.contains("amount") && call.kwargs["amount"].is_number())
            dpAmount = call.kwargs["amount"].get<double>();
        if (dpAmount <= 0.0)
            throw std::runtime_error("Down payment amount must be greater than zero");

        std::string dpNote = "Down Payment";
        if (call.kwargs.contains("note") && call.kwargs["note"].is_string())
            dpNote = call.kwargs["note"].get<std::string>();

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int ordId : ids) {
            auto r = txn.exec(
                "SELECT state, partner_id, payment_term_id, company_id, "
                "       currency_id, amount_total, name "
                "FROM purchase_order WHERE id = $1",
                pqxx::params{ordId});
            if (r.empty()) throw std::runtime_error("Purchase order not found");

            std::string state      = r[0][0].c_str();
            int         partnerId  = r[0][1].is_null() ? 0 : r[0][1].as<int>();
            int         payTermId  = r[0][2].is_null() ? 0 : r[0][2].as<int>();
            int         companyId  = r[0][3].as<int>();
            int         currencyId = r[0][4].is_null() ? 0 : r[0][4].as<int>();
            std::string orderName  = r[0][6].c_str();

            if (state != "purchase")
                throw std::runtime_error("Only confirmed orders can have a down payment");

            // Find journals/accounts
            auto jrow = txn.exec(
                "SELECT id FROM account_journal "
                "WHERE type = 'purchase' AND company_id = $1 AND active = TRUE "
                "ORDER BY id LIMIT 1", pqxx::params{companyId});
            if (jrow.empty()) throw std::runtime_error("No purchase journal found");
            int journalId = jrow[0][0].as<int>();

            auto expRow = txn.exec(
                "SELECT id FROM account_account "
                "WHERE account_type IN ('expense','expense_direct_cost') "
                "  AND company_id = $1 AND active = TRUE ORDER BY code LIMIT 1",
                pqxx::params{companyId});
            if (expRow.empty()) throw std::runtime_error("No expense account found");
            int expAccId = expRow[0][0].as<int>();

            auto apRow = txn.exec(
                "SELECT id FROM account_account "
                "WHERE account_type = 'liability_payable' "
                "  AND company_id = $1 AND active = TRUE ORDER BY code LIMIT 1",
                pqxx::params{companyId});
            if (apRow.empty()) throw std::runtime_error("No payable account found");
            int apAccId = apRow[0][0].as<int>();

            std::string today = purCurrentDate();
            std::string ref   = dpNote + " - " + orderName;

            // Create the advance bill header
            pqxx::params mp;
            mp.append(journalId);
            if (partnerId  > 0) mp.append(partnerId);  else mp.append(nullptr);
            if (payTermId  > 0) mp.append(payTermId);  else mp.append(nullptr);
            mp.append(companyId);
            if (currencyId > 0) mp.append(currencyId); else mp.append(nullptr);
            mp.append(today);      // $6 — date and invoice_date
            mp.append(dpAmount);   // $7 — amount_untaxed
            mp.append(0.0);        // $8 — amount_tax
            mp.append(dpAmount);   // $9 — amount_total
            mp.append(dpAmount);   // $10 — amount_residual
            mp.append(ref);        // $11 — ref
            mp.append(ordId);      // $12 — purchase_id

            auto mvRow = txn.exec(
                "INSERT INTO account_move "
                "(move_type, state, date, journal_id, partner_id, "
                " payment_term_id, company_id, currency_id, invoice_date, "
                " amount_untaxed, amount_tax, amount_total, amount_residual, ref, purchase_id) "
                "VALUES ('in_invoice','draft',$6,$1,$2,$3,$4,$5,$6,"
                "        $7,$8,$9,$10,$11,$12) "
                "RETURNING id",
                mp);
            int moveId = mvRow[0][0].as<int>();

            // Single expense line for the down payment amount
            pqxx::params el;
            el.append(moveId);
            el.append(expAccId);
            el.append(journalId);
            el.append(companyId);
            el.append(today);
            el.append(ref);
            if (partnerId > 0) el.append(partnerId); else el.append(nullptr);
            el.append(1.0);         // qty
            el.append(dpAmount);    // price_unit
            el.append(dpAmount);    // debit
            el.append(0.0);         // credit
            txn.exec(
                "INSERT INTO account_move_line "
                "(move_id, account_id, journal_id, company_id, date, "
                " name, partner_id, quantity, price_unit, debit, credit) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)",
                el);

            // Payable (credit) line
            pqxx::params pl;
            pl.append(moveId);
            pl.append(apAccId);
            pl.append(journalId);
            pl.append(companyId);
            pl.append(today);
            pl.append(ref);
            if (partnerId > 0) pl.append(partnerId); else pl.append(nullptr);
            pl.append(1.0);
            pl.append(0.0);
            pl.append(dpAmount);
            txn.exec(
                "INSERT INTO account_move_line "
                "(move_id, account_id, journal_id, company_id, date, "
                " name, partner_id, quantity, debit, credit) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
                pl);

            // Mark PO as partially billed
            txn.exec(
                "UPDATE purchase_order SET invoice_status = 'to invoice', write_date = now() "
                "WHERE id = $1 AND invoice_status = 'nothing'",
                pqxx::params{ordId});

            odoo::modules::mail::postLog(txn, "purchase.order", ordId, 0,
                "Down payment bill created: " + ref, "log_note");
        }

        txn.commit();
        return true;
    }
};


// ----------------------------------------------------------------
// PurchaseOrderLineViewModel — CRUD + amount recomputation
// ----------------------------------------------------------------
class PurchaseOrderLineViewModel : public PurchaseViewModel<PurchaseOrderLine> {
public:
    explicit PurchaseOrderLineViewModel(std::shared_ptr<DbConnection> db)
        : PurchaseViewModel<PurchaseOrderLine>(std::move(db))
    {
        // Override base CRUD registrations with amount-aware versions
        REGISTER_METHOD("create", handleCreate)
        REGISTER_METHOD("write",  handleWrite)
    }

    std::string modelName() const override { return "purchase.order.line"; }

    nlohmann::json handleCreate(const CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("create: args[0] must be a dict");

        nlohmann::json vals = v;
        recomputeAmounts_(vals);

        PurchaseOrderLine proto(db_);
        nlohmann::json result = proto.create(vals);

        int orderId = 0;
        if (vals.contains("order_id")) orderId = purM2oId(vals["order_id"]);
        if (orderId > 0) updateOrderTotals_(orderId);

        return result;
    }

    nlohmann::json handleWrite(const CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");

        const auto ids = call.ids();

        for (int id : ids) {
            // 1. Read current values then release connection
            int    orderId = 0;
            double qty     = 1.0;
            double unit    = 0.0;
            double disc    = 0.0;
            std::string taxJ = "[]";
            {
                auto conn = db_->acquire();
                pqxx::work txn{conn.get()};
                auto r = txn.exec(
                    "SELECT order_id, product_qty, price_unit, discount, tax_ids_json "
                    "FROM purchase_order_line WHERE id = $1",
                    pqxx::params{id});
                txn.commit();
                if (r.empty()) continue;
                orderId = r[0][0].is_null() ? 0 : r[0][0].as<int>();
                qty     = r[0][1].as<double>();
                unit    = r[0][2].as<double>();
                disc    = r[0][3].as<double>();
                taxJ    = r[0][4].c_str();
            }

            // 2. Merge with incoming values
            nlohmann::json merged;
            merged["order_id"]      = orderId;
            merged["product_qty"]   = qty;
            merged["price_unit"]    = unit;
            merged["discount"]      = disc;
            merged["tax_ids_json"]  = taxJ;
            for (auto& [k, val] : v.items()) merged[k] = val;

            // 3. Recompute amounts
            recomputeAmounts_(merged);

            // 4. Write all fields including computed amounts
            PurchaseOrderLine proto(db_);
            proto.write({id}, merged);

            // 5. Update parent order totals
            if (orderId > 0) updateOrderTotals_(orderId);
        }

        return true;
    }

private:
    void recomputeAmounts_(nlohmann::json& vals) {
        double qty  = vals.value("product_qty", 1.0);
        double unit = vals.value("price_unit",  0.0);
        double disc = vals.value("discount",    0.0);

        double subtotal = qty * unit * (1.0 - disc / 100.0);

        double taxAmt = 0.0;
        std::string taxJson = vals.value("tax_ids_json", std::string("[]"));
        try {
            auto taxIds = nlohmann::json::parse(taxJson);
            if (taxIds.is_array() && !taxIds.empty()) {
                auto conn = db_->acquire();
                pqxx::work txn{conn.get()};
                for (const auto& tid : taxIds) {
                    if (!tid.is_number_integer()) continue;
                    int taxId = tid.get<int>();
                    auto tr = txn.exec(
                        "SELECT amount, amount_type, price_include "
                        "FROM account_tax WHERE id = $1 AND active = TRUE",
                        pqxx::params{taxId});
                    if (tr.empty()) continue;
                    double taxRate = tr[0][0].as<double>();
                    std::string atype = tr[0][1].c_str();
                    bool priceIncl    = tr[0][2].as<bool>();
                    if (atype == "percent" && !priceIncl)
                        taxAmt += subtotal * taxRate / 100.0;
                }
            }
        } catch (...) {}

        vals["price_subtotal"] = std::round(subtotal * 100.0) / 100.0;
        vals["price_tax"]      = std::round(taxAmt   * 100.0) / 100.0;
        vals["price_total"]    = std::round((subtotal + taxAmt) * 100.0) / 100.0;
    }

    void updateOrderTotals_(int orderId) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(
            "UPDATE purchase_order SET "
            "  amount_untaxed = COALESCE((SELECT SUM(price_subtotal) FROM purchase_order_line WHERE order_id = $1), 0), "
            "  amount_tax     = COALESCE((SELECT SUM(price_tax)      FROM purchase_order_line WHERE order_id = $1), 0), "
            "  amount_total   = COALESCE((SELECT SUM(price_total)    FROM purchase_order_line WHERE order_id = $1), 0), "
            "  write_date     = now() "
            "WHERE id = $1",
            pqxx::params{orderId});
        txn.commit();
    }
};


// ================================================================
// 4. MODULE
// ================================================================

// ================================================================
// MODULE IMPLEMENTATIONS
// ================================================================
PurchaseModule::PurchaseModule(core::ModelFactory&     modelFactory,
                               core::ServiceFactory&   serviceFactory,
                               core::ViewModelFactory& viewModelFactory,
                               core::ViewFactory&      viewFactory)
    : models_    (modelFactory)
    , services_  (serviceFactory)
    , viewModels_(viewModelFactory)
    , views_     (viewFactory)
{}

std::string              PurchaseModule::moduleName()   const { return "purchase"; }
std::string              PurchaseModule::version()      const { return "19.0.1.0.0"; }
std::vector<std::string> PurchaseModule::dependencies() const { return {"product", "account"}; }

void PurchaseModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("purchase.order",      [db]{ return std::make_shared<PurchaseOrder>(db); });
    models_.registerCreator("purchase.order.line", [db]{ return std::make_shared<PurchaseOrderLine>(db); });
}

void PurchaseModule::registerServices() {}
void PurchaseModule::registerRoutes()   {}

void PurchaseModule::registerViews() {
    views_.registerView<PurchaseOrderListView>     ("purchase.order.list");
    views_.registerView<PurchaseOrderFormView>     ("purchase.order.form");
    views_.registerView<PurchaseOrderLineListView> ("purchase.order.line.list");
    views_.registerView<PurchaseOrderLineFormView> ("purchase.order.line.form");
}

void PurchaseModule::registerViewModels() {
    auto db = services_.db();
    viewModels_.registerCreator("purchase.order", [db]{
        return std::make_shared<PurchaseOrderViewModel>(db);
    });
    viewModels_.registerCreator("purchase.order.line", [db]{
        return std::make_shared<PurchaseOrderLineViewModel>(db);
    });
}

void PurchaseModule::initialize() {
    ensureSchema_();
    seedMenus_();
}

void PurchaseModule::ensureSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec("CREATE SEQUENCE IF NOT EXISTS purchase_order_seq START 1");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS purchase_order (
            id               SERIAL PRIMARY KEY,
            name             VARCHAR NOT NULL DEFAULT 'New',
            state            VARCHAR NOT NULL DEFAULT 'draft',
            partner_id       INTEGER NOT NULL REFERENCES res_partner(id),
            date_order       TIMESTAMP NOT NULL DEFAULT now(),
            date_planned     DATE,
            payment_term_id  INTEGER REFERENCES account_payment_term(id),
            note             TEXT,
            currency_id      INTEGER REFERENCES res_currency(id),
            company_id       INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            user_id          INTEGER REFERENCES res_users(id),
            origin           VARCHAR,
            invoice_status   VARCHAR NOT NULL DEFAULT 'nothing',
            amount_untaxed   NUMERIC(16,2) NOT NULL DEFAULT 0,
            amount_tax       NUMERIC(16,2) NOT NULL DEFAULT 0,
            amount_total     NUMERIC(16,2) NOT NULL DEFAULT 0,
            create_date      TIMESTAMP DEFAULT now(),
            write_date       TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(
        "ALTER TABLE account_move "
        "ADD COLUMN IF NOT EXISTS purchase_id INTEGER REFERENCES purchase_order(id)");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS purchase_order_line (
            id               SERIAL PRIMARY KEY,
            order_id         INTEGER NOT NULL REFERENCES purchase_order(id) ON DELETE CASCADE,
            sequence         INTEGER NOT NULL DEFAULT 10,
            product_id       INTEGER REFERENCES product_product(id),
            name             TEXT NOT NULL,
            product_qty      NUMERIC(16,4) NOT NULL DEFAULT 1,
            product_uom_id   INTEGER REFERENCES uom_uom(id),
            price_unit       NUMERIC(16,4) NOT NULL DEFAULT 0,
            discount         NUMERIC(8,4)  NOT NULL DEFAULT 0,
            tax_ids_json     TEXT NOT NULL DEFAULT '[]',
            price_subtotal   NUMERIC(16,2) NOT NULL DEFAULT 0,
            price_tax        NUMERIC(16,2) NOT NULL DEFAULT 0,
            price_total      NUMERIC(16,2) NOT NULL DEFAULT 0,
            date_planned     DATE,
            qty_invoiced     NUMERIC(16,4) NOT NULL DEFAULT 0,
            qty_received     NUMERIC(16,4) NOT NULL DEFAULT 0,
            company_id       INTEGER REFERENCES res_company(id),
            currency_id      INTEGER REFERENCES res_currency(id),
            create_date      TIMESTAMP DEFAULT now(),
            write_date       TIMESTAMP DEFAULT now()
        )
    )");

    txn.commit();
}

void PurchaseModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (12, 'Purchase Orders', 'purchase.order', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO NOTHING
    )");

    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id, web_icon)
        VALUES (70, 'Purchase', NULL, 50, NULL, 'purchase')
        ON CONFLICT (id) DO NOTHING
    )");

    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (71, 'Orders', 70, 10, NULL)
        ON CONFLICT (id) DO NOTHING
    )");

    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (72, 'Purchase Orders', 71, 10, 12)
        ON CONFLICT (id) DO NOTHING
    )");

    txn.commit();
}

} // namespace odoo::modules::purchase

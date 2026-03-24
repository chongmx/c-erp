#pragma once
// =============================================================
// modules/report/ReportModule.hpp
//
// Phase 30 — Document Report & Settings
//
// Provides:
//   ir.report.template  (table: ir_report_template)
//     - name, model, template_html, paper_format, orientation, active
//
//   ReportTemplateViewModel — search_read, read, write, fields_get
//   TemplateRenderer        — static render helpers
//
// Routes:
//   GET /report/html/{model}/{id}  → rendered HTML document
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include "BaseModel.hpp"
#include "BaseView.hpp"
#include "BaseViewModel.hpp"
#include "DbConnection.hpp"
#include "SessionManager.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <drogon/drogon.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <fstream>
#include <cstdlib>
#include <cstdio>

namespace odoo::modules::report {

using namespace odoo::infrastructure;
using namespace odoo::core;

// ================================================================
// TemplateRenderer — static mustache-like template renderer
// ================================================================
class TemplateRenderer {
public:
    static std::string replaceAll(std::string str,
                                  const std::string& from,
                                  const std::string& to)
    {
        if (from.empty()) return str;
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.size(), to);
            pos += to.size();
        }
        return str;
    }

    static std::string render(
        std::string tmpl,
        const std::map<std::string, std::string>& vars,
        const std::vector<std::map<std::string, std::string>>& lines = {})
    {
        // Handle {{#each lines}}...{{/each}} loops
        const std::string eachStart = "{{#each lines}}";
        const std::string eachEnd   = "{{/each}}";
        size_t sPos = tmpl.find(eachStart);
        size_t ePos = tmpl.find(eachEnd);

        if (sPos != std::string::npos && ePos != std::string::npos) {
            std::string before  = tmpl.substr(0, sPos);
            std::string loopTpl = tmpl.substr(sPos + eachStart.size(),
                                              ePos - sPos - eachStart.size());
            std::string after   = tmpl.substr(ePos + eachEnd.size());

            std::string expanded;
            for (const auto& line : lines) {
                std::string row = loopTpl;
                for (const auto& [k, v] : line)
                    row = replaceAll(row, "{{" + k + "}}", v);
                expanded += row;
            }
            tmpl = before + expanded + after;
        }

        // Replace scalar vars
        for (const auto& [k, v] : vars)
            tmpl = replaceAll(tmpl, "{{" + k + "}}", v);

        // Blank out any remaining unresolved {{placeholders}}
        {
            std::string out;
            out.reserve(tmpl.size());
            std::size_t i = 0;
            while (i < tmpl.size()) {
                if (tmpl[i] == '{' && i + 1 < tmpl.size() && tmpl[i+1] == '{') {
                    std::size_t end = tmpl.find("}}", i + 2);
                    if (end != std::string::npos) { i = end + 2; continue; }
                }
                out += tmpl[i++];
            }
            tmpl = std::move(out);
        }

        return tmpl;
    }
};

// ================================================================
// Number formatter helpers
// ================================================================

// Legacy — kept for backward compat (no comma separator)
static std::string fmtNum(const pqxx::field& f) {
    if (f.is_null()) return "0.00";
    try {
        double v = f.as<double>();
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    } catch (...) { return f.c_str(); }
}

static std::string safeStr(const pqxx::field& f) {
    return f.is_null() ? "" : f.c_str();
}

// Format a double as "13,600.00" with comma thousands separator
static std::string fmtMoney(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << std::abs(v);
    std::string s = oss.str();
    // Insert commas
    size_t dot = s.find('.');
    size_t start = dot == std::string::npos ? s.size() : dot;
    int ins = (int)start - 3;
    while (ins > 0) { s.insert(ins, ","); ins -= 3; }
    if (v < 0) s = "-" + s;
    return s;
}

static std::string fmtMoneyField(const pqxx::field& f) {
    if (f.is_null()) return "0.00";
    try { return fmtMoney(f.as<double>()); } catch (...) { return f.c_str(); }
}

// Precision-aware format (comma-thousands, variable decimals)
static std::string fmtPrec(double v, int prec) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(prec) << std::abs(v);
    std::string s = oss.str();
    size_t dot = s.find('.');
    size_t start = dot == std::string::npos ? s.size() : dot;
    int ins = (int)start - 3;
    while (ins > 0) { s.insert(ins, ","); ins -= 3; }
    if (v < 0) s = "-" + s;
    return s;
}
static std::string fmtPrecF(const pqxx::field& f, int prec) {
    if (f.is_null()) return "0." + std::string(std::max(0, prec), '0');
    try { return fmtPrec(f.as<double>(), prec); } catch (...) { return f.c_str(); }
}

// Convert YYYY-MM-DD to DD/MM/YYYY
static std::string ymdToDisplay(const std::string& ymd) {
    if (ymd.size() >= 10 && ymd[4] == '-' && ymd[7] == '-')
        return ymd.substr(8, 2) + "/" + ymd.substr(5, 2) + "/" + ymd.substr(0, 4);
    return ymd;
}

// ================================================================
// HTML error page helper
// ================================================================
static drogon::HttpResponsePtr htmlError(int status, const std::string& msg) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    resp->setContentTypeCode(drogon::CT_TEXT_HTML);
    resp->setBody("<html><body><h2>Error: " + msg + "</h2></body></html>");
    return resp;
}

// ================================================================
// Shared CSS block (wkhtmltopdf-compatible, float-based)
// ================================================================
static const std::string SHARED_CSS = R"CSS(
<style>
@page { size: A4; margin: 0; }
* { box-sizing: border-box; }
body { font-family: Arial, Helvetica, sans-serif; font-size: 10pt; color: #333333; line-height: 1.5; margin: 0; padding: 15mm 18mm 20mm 18mm; display: flex; flex-direction: column; min-height: 257mm; }
.page-fill-spacer { flex: 1; }
.clearfix { overflow: hidden; }
.hdr-left { float: left; width: 40%; }
.hdr-right { float: right; width: 58%; }
.company-name { font-weight: bold; font-size: 11pt; }
.company-detail { font-size: 10pt; }
.info-row { overflow: hidden; margin-top: 10mm; margin-bottom: 6mm; }
.buyer-col { float: left; width: 55%; }
.meta-col { float: right; width: 42%; }
.buyer-name { font-weight: bold; font-size: 11pt; }
.doc-title { font-weight: bold; font-size: 14pt; margin-bottom: 2mm; }
.meta-line { font-size: 10pt; }
.meta-lbl { font-weight: bold; }
.attn { margin-bottom: 3mm; font-size: 10pt; }
.currency-note { margin-bottom: 3mm; font-size: 10pt; }
.lines-table { width: 100%; border-collapse: collapse; margin-bottom: 4mm; border: 0.5pt solid #cccccc; }
.lines-table thead th { background-color: #4a4a4a; color: #ffffff; font-weight: bold; text-transform: uppercase; padding: 6px 8px; font-size: 10pt; text-align: left; }
.lines-table thead th.r { text-align: right; }
.lines-table thead th.c { text-align: center; }
.lines-table tbody td { padding: 6px 8px; border-bottom: 0.5pt solid #cccccc; font-size: 10pt; vertical-align: top; }
.lines-table tbody td.r { text-align: right; }
.lines-table tbody td.c { text-align: center; }
.col-desc { width: 55%; }
.col-qty { width: 12%; }
.col-uom { width: 10%; }
.col-price { width: 16%; }
.col-amount { width: 17%; }
.row-line_section td { font-weight: bold; background-color: #f5f5f5; }
.row-line_note td { font-style: italic; color: #666666; }
.totals-wrap { overflow: hidden; margin-bottom: 8mm; }
.totals-table { float: right; width: 45%; border-collapse: collapse; }
.totals-table td { padding: 5px 8px; font-size: 10pt; }
.totals-table .t-lbl { text-align: right; font-weight: bold; padding-right: 12px; }
.totals-table .t-val { text-align: right; white-space: nowrap; }
.totals-table .row-total td { background-color: #4a4a4a; color: #ffffff; font-weight: bold; }
.payment-terms { margin-bottom: 5mm; font-size: 11pt; }
.bank-details { font-size: 9.5pt; line-height: 1.7; }
.page-footer { position: fixed; bottom: 0; left: 0; right: 0; background-color: #4a4a4a; color: #ffffff; text-align: center; padding: 6px 0; font-size: 9pt; }
.print-btn { display: inline-block; margin-top: 10mm; padding: 8px 20px; background: #4a4a4a; color: #fff; border: none; font-size: 10pt; cursor: pointer; }
@media print { .print-btn { display: none; } }
</style>
)CSS";

// ================================================================
// Default HTML Templates
// ================================================================

static const std::string INVOICE_TEMPLATE = R"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8">
<title>{{document_title}} - {{doc_number}}</title>
)HTML" + SHARED_CSS + R"HTML(
</head><body>

<div class="clearfix">
  <div class="hdr-left">
    <div class="company-name">{{company_name}} ({{company_reg}})</div>
    <div class="company-detail">{{company_addr1}}</div>
    <div class="company-detail">{{company_addr2}}</div>
    <div class="company-detail">{{company_addr3}}</div>
    <div class="company-detail">{{company_city_country}}</div>
  </div>
  <div class="hdr-right"></div>
</div>

<div class="info-row">
  <div class="buyer-col">
    <div class="buyer-name">{{partner_name}}</div>
    <div>{{partner_street}}</div>
    <div>{{partner_city}}</div>
    <div>{{partner_phone}}</div>
    <div class="attn">Attn: {{attn_name}}</div>
  </div>
  <div class="meta-col">
    <div class="doc-title">{{document_title}}</div>
    <div class="meta-line"><span class="meta-lbl">Invoice No. :</span> {{doc_number}}</div>
    <div class="meta-line"><span class="meta-lbl">Invoice Date :</span> {{doc_date}}</div>
    <div class="meta-line"><span class="meta-lbl">Due Date :</span> {{doc_date_due}}</div>
  </div>
</div>

<div class="currency-note">All Amount Stated in - {{currency_code}}</div>

<table class="lines-table">
  <thead><tr>
    <th class="col-desc">DESCRIPTION</th>
    <th class="col-qty c">QUANTITY</th>
    <th class="col-price r">UNIT PRICE</th>
    <th class="col-amount r">AMOUNT</th>
  </tr></thead>
  <tbody>
    {{#each lines}}
    <tr class="row-{{line_type}}">
      <td>{{product_name}}</td>
      <td class="c">{{qty}}</td>
      <td class="r">{{price_unit}}</td>
      <td class="r">{{subtotal}}</td>
    </tr>
    {{/each}}
  </tbody>
</table>

<div class="totals-wrap">
  <table class="totals-table">
    <tr><td class="t-lbl">Subtotal</td><td class="t-val">{{currency_code}} {{amount_untaxed}}</td></tr>
    <tr><td class="t-lbl">Tax</td><td class="t-val">{{currency_code}} {{amount_tax}}</td></tr>
    <tr class="row-total"><td class="t-lbl">Total</td><td class="t-val">{{currency_code}} {{amount_total}}</td></tr>
  </table>
</div>

<div class="payment-terms"><strong>Payment terms:</strong> {{payment_term_days}} Days</div>

<div class="bank-details">
  <div>TT Transfer Payable to</div>
  <div>Account Name : {{bank_account_name}}</div>
  <div>Account No. : {{bank_account_no}}</div>
  <div>Bank Name : {{bank_name}}</div>
  <div>Bank Address : {{bank_address}}</div>
  <div>Bank SWIFT Code : {{bank_swift}}</div>
</div>

<div class="page-footer">{{company_website}}</div>
<button class="print-btn" onclick="window.print()">Print / Save as PDF</button>
</body></html>)HTML";

static const std::string SALE_ORDER_TEMPLATE = R"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8">
<title>{{document_title}} - {{doc_number}}</title>
)HTML" + SHARED_CSS + R"HTML(
</head><body>

<div class="clearfix">
  <div class="hdr-left">
    <div class="company-name">{{company_name}} ({{company_reg}})</div>
    <div class="company-detail">{{company_addr1}}</div>
    <div class="company-detail">{{company_addr2}}</div>
    <div class="company-detail">{{company_addr3}}</div>
    <div class="company-detail">{{company_city_country}}</div>
  </div>
  <div class="hdr-right"></div>
</div>

<div class="info-row">
  <div class="buyer-col">
    <div class="buyer-name">{{partner_name}}</div>
    <div>{{partner_street}}</div>
    <div>{{partner_city}}</div>
    <div>{{partner_phone}}</div>
    <div class="attn">Attn: {{attn_name}}</div>
  </div>
  <div class="meta-col">
    <div class="doc-title">{{document_title}}</div>
    <div class="meta-line"><span class="meta-lbl">Order No. :</span> {{doc_number}}</div>
    <div class="meta-line"><span class="meta-lbl">Order Date :</span> {{doc_date}}</div>
    <div class="meta-line"><span class="meta-lbl">Valid Until :</span> {{validity_date}}</div>
  </div>
</div>

<div class="currency-note">All Amount Stated in - {{currency_code}}</div>

<table class="lines-table">
  <thead><tr>
    <th class="col-desc">DESCRIPTION</th>
    <th class="col-qty c">QUANTITY</th>
    <th class="col-uom">UOM</th>
    <th class="col-price r">UNIT PRICE</th>
    <th class="col-amount r">AMOUNT</th>
  </tr></thead>
  <tbody>
    {{#each lines}}
    <tr>
      <td>{{product_name}}</td>
      <td class="c">{{qty}}</td>
      <td>{{uom}}</td>
      <td class="r">{{price_unit}}</td>
      <td class="r">{{subtotal}}</td>
    </tr>
    {{/each}}
  </tbody>
</table>

<div class="totals-wrap">
  <table class="totals-table">
    <tr><td class="t-lbl">Subtotal</td><td class="t-val">{{currency_code}} {{amount_untaxed}}</td></tr>
    <tr><td class="t-lbl">Tax</td><td class="t-val">{{currency_code}} {{amount_tax}}</td></tr>
    <tr class="row-total"><td class="t-lbl">Total</td><td class="t-val">{{currency_code}} {{amount_total}}</td></tr>
  </table>
</div>

<div class="payment-terms"><strong>Payment terms:</strong> {{payment_term_days}} Days</div>

<div class="bank-details">
  <div>TT Transfer Payable to</div>
  <div>Account Name : {{bank_account_name}}</div>
  <div>Account No. : {{bank_account_no}}</div>
  <div>Bank Name : {{bank_name}}</div>
  <div>Bank Address : {{bank_address}}</div>
  <div>Bank SWIFT Code : {{bank_swift}}</div>
</div>

<div class="page-footer">{{company_website}}</div>
<button class="print-btn" onclick="window.print()">Print / Save as PDF</button>
</body></html>)HTML";

static const std::string PURCHASE_ORDER_TEMPLATE = R"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8">
<title>{{document_title}} - {{doc_number}}</title>
)HTML" + SHARED_CSS + R"HTML(
</head><body>

<div class="clearfix">
  <div class="hdr-left">
    <div class="company-name">{{company_name}} ({{company_reg}})</div>
    <div class="company-detail">{{company_addr1}}</div>
    <div class="company-detail">{{company_addr2}}</div>
    <div class="company-detail">{{company_addr3}}</div>
    <div class="company-detail">{{company_city_country}}</div>
  </div>
  <div class="hdr-right"></div>
</div>

<div class="info-row">
  <div class="buyer-col">
    <div class="buyer-name">{{partner_name}}</div>
    <div>{{partner_street}}</div>
    <div>{{partner_city}}</div>
    <div>{{partner_phone}}</div>
    <div class="attn">Attn: {{attn_name}}</div>
  </div>
  <div class="meta-col">
    <div class="doc-title">{{document_title}}</div>
    <div class="meta-line"><span class="meta-lbl">PO No. :</span> {{doc_number}}</div>
    <div class="meta-line"><span class="meta-lbl">Order Date :</span> {{doc_date}}</div>
    <div class="meta-line"><span class="meta-lbl">Expected :</span> {{date_planned}}</div>
  </div>
</div>

<div class="currency-note">All Amount Stated in - {{currency_code}}</div>

<table class="lines-table">
  <thead><tr>
    <th class="col-desc">DESCRIPTION</th>
    <th class="col-qty c">QUANTITY</th>
    <th class="col-uom">UOM</th>
    <th class="col-price r">UNIT PRICE</th>
    <th class="col-amount r">AMOUNT</th>
  </tr></thead>
  <tbody>
    {{#each lines}}
    <tr>
      <td>{{product_name}}</td>
      <td class="c">{{qty}}</td>
      <td>{{uom}}</td>
      <td class="r">{{price_unit}}</td>
      <td class="r">{{subtotal}}</td>
    </tr>
    {{/each}}
  </tbody>
</table>

<div class="totals-wrap">
  <table class="totals-table">
    <tr><td class="t-lbl">Subtotal</td><td class="t-val">{{currency_code}} {{amount_untaxed}}</td></tr>
    <tr><td class="t-lbl">Tax</td><td class="t-val">{{currency_code}} {{amount_tax}}</td></tr>
    <tr class="row-total"><td class="t-lbl">Total</td><td class="t-val">{{currency_code}} {{amount_total}}</td></tr>
  </table>
</div>

<div class="payment-terms"><strong>Payment terms:</strong> {{payment_term_days}} Days</div>

<div class="bank-details">
  <div>TT Transfer Payable to</div>
  <div>Account Name : {{bank_account_name}}</div>
  <div>Account No. : {{bank_account_no}}</div>
  <div>Bank Name : {{bank_name}}</div>
  <div>Bank Address : {{bank_address}}</div>
  <div>Bank SWIFT Code : {{bank_swift}}</div>
</div>

<div class="page-footer">{{company_website}}</div>
<button class="print-btn" onclick="window.print()">Print / Save as PDF</button>
</body></html>)HTML";

static const std::string STOCK_PICKING_TEMPLATE = R"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8">
<title>{{document_title}} - {{doc_number}}</title>
)HTML" + SHARED_CSS + R"HTML(
</head><body>

<div class="clearfix">
  <div class="hdr-left">
    <div class="company-name">{{company_name}} ({{company_reg}})</div>
    <div class="company-detail">{{company_addr1}}</div>
    <div class="company-detail">{{company_addr2}}</div>
    <div class="company-detail">{{company_addr3}}</div>
    <div class="company-detail">{{company_city_country}}</div>
  </div>
  <div class="hdr-right"></div>
</div>

<div class="info-row">
  <div class="buyer-col">
    <div class="buyer-name">{{partner_name}}</div>
    <div>{{partner_street}}</div>
    <div>{{partner_city}}</div>
    <div>{{partner_phone}}</div>
    <div class="attn">Attn: {{attn_name}}</div>
  </div>
  <div class="meta-col">
    <div class="doc-title">{{document_title}}</div>
    <div class="meta-line"><span class="meta-lbl">Ref No. :</span> {{doc_number}}</div>
    <div class="meta-line"><span class="meta-lbl">Date :</span> {{doc_date}}</div>
    <div class="meta-line"><span class="meta-lbl">Origin :</span> {{origin}}</div>
  </div>
</div>

<div class="info-row">
  <div class="buyer-col">
    <div class="meta-line"><span class="meta-lbl">From :</span> {{source_location}}</div>
    <div class="meta-line"><span class="meta-lbl">To :</span> {{dest_location}}</div>
  </div>
</div>

<table class="lines-table">
  <thead><tr>
    <th class="col-desc">PRODUCT</th>
    <th class="col-qty c">DEMAND</th>
    <th class="col-qty c">DONE</th>
    <th class="col-uom">UOM</th>
  </tr></thead>
  <tbody>
    {{#each lines}}
    <tr>
      <td>{{product_name}}</td>
      <td class="c">{{demand}}</td>
      <td class="c">{{done}}</td>
      <td>{{uom}}</td>
    </tr>
    {{/each}}
  </tbody>
</table>

<div class="page-footer">{{company_website}}</div>
<button class="print-btn" onclick="window.print()">Print / Save as PDF</button>
</body></html>)HTML";

// Escape single quotes for SQL
static std::string sqlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out;
}

// ================================================================
// ReportTemplateViewModel — ir.report.template
// ================================================================
class ReportTemplateViewModel : public BaseViewModel {
public:
    explicit ReportTemplateViewModel(std::shared_ptr<DbConnection> db)
        : db_(std::move(db))
    {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_METHOD("write",           handleWrite)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
    }

    std::string modelName() const override { return "ir.report.template"; }

private:
    std::shared_ptr<DbConnection> db_;

    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        // Extract model filter from domain e.g. [["model","=","account.move"]]
        std::string modelFilter;
        const auto& dom = call.domain();
        if (dom.is_array()) {
            for (const auto& leaf : dom) {
                if (leaf.is_array() && leaf.size() == 3 &&
                    leaf[0].is_string() && leaf[0].get<std::string>() == "model" &&
                    leaf[1].is_string() && leaf[1].get<std::string>() == "=" &&
                    leaf[2].is_string()) {
                    modelFilter = leaf[2].get<std::string>();
                    break;
                }
            }
        }

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        pqxx::result rows;
        if (modelFilter.empty()) {
            rows = txn.exec(
                "SELECT id, name, model, paper_format, orientation, active, "
                "decimal_qty, decimal_price, decimal_subtotal, "
                "COALESCE(margin_top,15)::float AS margin_top, COALESCE(margin_right,18)::float AS margin_right, "
                "COALESCE(margin_bottom,18)::float AS margin_bottom, COALESCE(margin_left,18)::float AS margin_left, "
                "COALESCE(font_size,10) AS font_size, COALESCE(font_color,'#333333') AS font_color, "
                "COALESCE(line_height,1.5)::float AS line_height, COALESCE(footer_text,'') AS footer_text "
                "FROM ir_report_template WHERE active=true ORDER BY id");
        } else {
            rows = txn.exec(
                "SELECT id, name, model, paper_format, orientation, active, "
                "decimal_qty, decimal_price, decimal_subtotal, "
                "COALESCE(margin_top,15)::float AS margin_top, COALESCE(margin_right,18)::float AS margin_right, "
                "COALESCE(margin_bottom,18)::float AS margin_bottom, COALESCE(margin_left,18)::float AS margin_left, "
                "COALESCE(font_size,10) AS font_size, COALESCE(font_color,'#333333') AS font_color, "
                "COALESCE(line_height,1.5)::float AS line_height, COALESCE(footer_text,'') AS footer_text "
                "FROM ir_report_template WHERE active=true AND model=$1 ORDER BY id LIMIT 1",
                pqxx::params{modelFilter});
        }

        nlohmann::json result = nlohmann::json::array();
        for (const auto& row : rows) {
            nlohmann::json rec;
            rec["id"]             = row["id"].as<int>();
            rec["name"]           = safeStr(row["name"]);
            rec["model"]          = safeStr(row["model"]);
            rec["paper_format"]   = safeStr(row["paper_format"]);
            rec["orientation"]    = safeStr(row["orientation"]);
            rec["active"]         = row["active"].is_null() ? true : row["active"].as<bool>();
            rec["decimal_qty"]      = row["decimal_qty"].is_null()      ? 2 : row["decimal_qty"].as<int>();
            rec["decimal_price"]    = row["decimal_price"].is_null()    ? 2 : row["decimal_price"].as<int>();
            rec["decimal_subtotal"] = row["decimal_subtotal"].is_null() ? 2 : row["decimal_subtotal"].as<int>();
            rec["margin_top"]       = row["margin_top"].as<double>(15);
            rec["margin_right"]     = row["margin_right"].as<double>(18);
            rec["margin_bottom"]    = row["margin_bottom"].as<double>(18);
            rec["margin_left"]      = row["margin_left"].as<double>(18);
            rec["font_size"]        = row["font_size"].as<int>(10);
            rec["font_color"]       = safeStr(row["font_color"]);
            rec["line_height"]      = row["line_height"].as<double>(1.5);
            rec["footer_text"]      = safeStr(row["footer_text"]);
            result.push_back(rec);
        }
        return result;
    }

    nlohmann::json handleRead(const CallKwArgs& call) {
        const auto& idArg = call.arg(0);
        std::vector<int> ids;
        if (idArg.is_array()) {
            for (const auto& v : idArg) {
                if (v.is_number_integer())
                    ids.push_back(v.get<int>());
                else if (v.is_array() && !v.empty() && v[0].is_number_integer())
                    ids.push_back(v[0].get<int>());
            }
        } else if (idArg.is_number_integer()) {
            ids.push_back(idArg.get<int>());
        }
        if (ids.empty()) return nlohmann::json::array();

        // Build IN clause
        std::string inClause;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i > 0) inClause += ",";
            inClause += std::to_string(ids[i]);
        }

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto rows = txn.exec(
            "SELECT id, name, model, template_html, paper_format, orientation, active, "
            "decimal_qty, decimal_price, decimal_subtotal, "
            "COALESCE(margin_top,15)::float AS margin_top, COALESCE(margin_right,18)::float AS margin_right, "
            "COALESCE(margin_bottom,18)::float AS margin_bottom, COALESCE(margin_left,18)::float AS margin_left, "
            "COALESCE(font_size,10) AS font_size, COALESCE(font_color,'#333333') AS font_color, "
            "COALESCE(line_height,1.5)::float AS line_height, COALESCE(footer_text,'') AS footer_text "
            "FROM ir_report_template WHERE id IN (" + inClause + ") ORDER BY id");

        nlohmann::json result = nlohmann::json::array();
        for (const auto& row : rows) {
            nlohmann::json rec;
            rec["id"]               = row["id"].as<int>();
            rec["name"]             = safeStr(row["name"]);
            rec["model"]            = safeStr(row["model"]);
            rec["template_html"]    = safeStr(row["template_html"]);
            rec["paper_format"]     = safeStr(row["paper_format"]);
            rec["orientation"]      = safeStr(row["orientation"]);
            rec["active"]           = row["active"].is_null() ? true : row["active"].as<bool>();
            rec["decimal_qty"]      = row["decimal_qty"].is_null()      ? 2 : row["decimal_qty"].as<int>();
            rec["decimal_price"]    = row["decimal_price"].is_null()    ? 2 : row["decimal_price"].as<int>();
            rec["decimal_subtotal"] = row["decimal_subtotal"].is_null() ? 2 : row["decimal_subtotal"].as<int>();
            rec["margin_top"]       = row["margin_top"].as<double>(15);
            rec["margin_right"]     = row["margin_right"].as<double>(18);
            rec["margin_bottom"]    = row["margin_bottom"].as<double>(18);
            rec["margin_left"]      = row["margin_left"].as<double>(18);
            rec["font_size"]        = row["font_size"].as<int>(10);
            rec["font_color"]       = safeStr(row["font_color"]);
            rec["line_height"]      = row["line_height"].as<double>(1.5);
            rec["footer_text"]      = safeStr(row["footer_text"]);
            result.push_back(rec);
        }
        return result;
    }

    nlohmann::json handleWrite(const CallKwArgs& call) {
        // args: [[id1,...], {vals}]  or  [[id], {vals}]
        const auto& idArg  = call.arg(0);
        const auto& vals   = call.arg(1);

        std::vector<int> ids;
        if (idArg.is_array()) {
            for (const auto& v : idArg)
                if (v.is_number_integer()) ids.push_back(v.get<int>());
        } else if (idArg.is_number_integer()) {
            ids.push_back(idArg.get<int>());
        }
        if (ids.empty() || !vals.is_object()) return false;

        std::string templateHtml = vals.value("template_html", "");
        std::string paperFormat  = vals.value("paper_format",  "A4");
        std::string orientation  = vals.value("orientation",   "portrait");
        std::string name         = vals.value("name",          "");
        int decimalQty      = vals.contains("decimal_qty")      && vals["decimal_qty"].is_number_integer()
                               ? vals["decimal_qty"].get<int>() : -1;
        int decimalPrice    = vals.contains("decimal_price")    && vals["decimal_price"].is_number_integer()
                               ? vals["decimal_price"].get<int>() : -1;
        int decimalSubtotal = vals.contains("decimal_subtotal") && vals["decimal_subtotal"].is_number_integer()
                               ? vals["decimal_subtotal"].get<int>() : -1;
        int decQty  = std::max(0, decimalQty      < 0 ? 2 : decimalQty);
        int decPrc  = std::max(0, decimalPrice    < 0 ? 2 : decimalPrice);
        int decSub  = std::max(0, decimalSubtotal < 0 ? 2 : decimalSubtotal);
        double marginTop    = vals.contains("margin_top")    && vals["margin_top"].is_number()    ? vals["margin_top"].get<double>()    : 15.0;
        double marginRight  = vals.contains("margin_right")  && vals["margin_right"].is_number()  ? vals["margin_right"].get<double>()  : 18.0;
        double marginBottom = vals.contains("margin_bottom") && vals["margin_bottom"].is_number() ? vals["margin_bottom"].get<double>() : 18.0;
        double marginLeft   = vals.contains("margin_left")   && vals["margin_left"].is_number()   ? vals["margin_left"].get<double>()   : 18.0;
        int    fontSize     = vals.contains("font_size")     && vals["font_size"].is_number_integer() ? vals["font_size"].get<int>()    : 10;
        std::string fontColor  = vals.value("font_color",   "#333333");
        double lineHeight   = vals.contains("line_height")   && vals["line_height"].is_number()   ? vals["line_height"].get<double>()   : 1.5;
        std::string footerText = vals.value("footer_text",  "");

        std::string inClause;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i > 0) inClause += ",";
            inClause += std::to_string(ids[i]);
        }

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        if (!name.empty()) {
            txn.exec(
                "UPDATE ir_report_template SET "
                "template_html=$1, paper_format=$2, orientation=$3, name=$4, "
                "decimal_qty=$5, decimal_price=$6, decimal_subtotal=$7, "
                "margin_top=$8, margin_right=$9, margin_bottom=$10, margin_left=$11, "
                "font_size=$12, font_color=$13, line_height=$14, footer_text=$15 "
                "WHERE id IN (" + inClause + ")",
                pqxx::params{templateHtml, paperFormat, orientation, name, decQty, decPrc, decSub,
                             marginTop, marginRight, marginBottom, marginLeft,
                             fontSize, fontColor, lineHeight, footerText});
        } else {
            txn.exec(
                "UPDATE ir_report_template SET "
                "template_html=$1, paper_format=$2, orientation=$3, "
                "decimal_qty=$4, decimal_price=$5, decimal_subtotal=$6, "
                "margin_top=$7, margin_right=$8, margin_bottom=$9, margin_left=$10, "
                "font_size=$11, font_color=$12, line_height=$13, footer_text=$14 "
                "WHERE id IN (" + inClause + ")",
                pqxx::params{templateHtml, paperFormat, orientation, decQty, decPrc, decSub,
                             marginTop, marginRight, marginBottom, marginLeft,
                             fontSize, fontColor, lineHeight, footerText});
        }
        txn.commit();
        return true;
    }

    nlohmann::json handleFieldsGet(const CallKwArgs&) {
        return {
            {"id",            {{"type","integer"}, {"string","ID"}}},
            {"name",          {{"type","char"},    {"string","Template Name"}}},
            {"model",         {{"type","char"},    {"string","Model"}}},
            {"template_html", {{"type","text"},    {"string","Template HTML"}}},
            {"paper_format",  {{"type","char"},    {"string","Paper Format"}}},
            {"orientation",   {{"type","char"},    {"string","Orientation"}}},
            {"active",        {{"type","boolean"}, {"string","Active"}}},
        };
    }
};

// ================================================================
// ReportModule — IModule implementation
// ================================================================
class ReportModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "report"; }

    explicit ReportModule(core::ModelFactory&     modelFactory,
                          core::ServiceFactory&   serviceFactory,
                          core::ViewModelFactory& viewModelFactory,
                          core::ViewFactory&      viewFactory)
        : models_    (modelFactory)
        , services_  (serviceFactory)
        , viewModels_(viewModelFactory)
        , views_     (viewFactory)
    {
        db_ = serviceFactory.db();
    }

    std::string              moduleName()   const override { return "report"; }
    std::string              version()      const override { return "17.0.1.0.0"; }
    std::vector<std::string> dependencies() const override { return {"base", "sale", "purchase", "stock", "account"}; }

    void registerModels()   override {}
    void registerServices() override {}
    void registerViews()    override {}

    void registerViewModels() override {
        auto db = db_;
        viewModels_.registerCreator("ir.report.template", [db]{
            return std::make_shared<ReportTemplateViewModel>(db);
        });
    }

    // ---------------------------------------------------------------
    // renderDoc_ — renders a document record to HTML.
    // Called by both /report/html/ and /report/pdf/ routes.
    // Throws std::runtime_error on record-not-found or bad model.
    // ---------------------------------------------------------------
    static std::string renderDoc_(
        pqxx::work& txn,
        const std::string& model,
        int recordId)
    {
        // Load template
        auto tplRows = txn.exec(
            "SELECT template_html, paper_format, orientation, "
            "COALESCE(decimal_qty, 2) AS decimal_qty, "
            "COALESCE(decimal_price, 2) AS decimal_price, "
            "COALESCE(decimal_subtotal, 2) AS decimal_subtotal "
            "FROM ir_report_template "
            "WHERE model=$1 AND active=true ORDER BY id LIMIT 1",
            pqxx::params{model});

        if (tplRows.empty())
            throw std::runtime_error("No template found for model: " + model);

        std::string tplHtml     = safeStr(tplRows[0]["template_html"]);
        std::string paperFormat = safeStr(tplRows[0]["paper_format"]);
        std::string orientation = safeStr(tplRows[0]["orientation"]);
        if (paperFormat.empty()) paperFormat = "A4";
        if (orientation.empty()) orientation = "portrait";
        const int qtyPrec = tplRows[0]["decimal_qty"].as<int>(2);
        const int prcPrec = tplRows[0]["decimal_price"].as<int>(2);
        const int subPrec = tplRows[0]["decimal_subtotal"].as<int>(2);

        std::map<std::string, std::string> vars;
        std::vector<std::map<std::string, std::string>> lines;

        vars["paper_format"] = paperFormat;
        vars["orientation"]  = orientation;

        auto loadCfg = [&](const std::string& key, const std::string& def = "") -> std::string {
            try {
                auto r = txn.exec(
                    "SELECT value FROM ir_config_parameter WHERE key=$1",
                    pqxx::params{key});
                if (!r.empty() && !r[0]["value"].is_null())
                    return r[0]["value"].c_str();
            } catch (...) {}
            return def;
        };

        int companyId = 1;
        int partnerId = 0;

        if (model == "sale.order") {
                        auto rows = txn.exec(
                            "SELECT so.name, so.state, "
                            "to_char(so.date_order AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS date_order, "
                            "to_char(so.validity_date, 'YYYY-MM-DD') AS validity_date, "
                            "COALESCE(so.amount_untaxed::TEXT,'0') AS amount_untaxed, "
                            "COALESCE(so.amount_tax::TEXT,'0') AS amount_tax, "
                            "COALESCE(so.amount_total::TEXT,'0') AS amount_total, "
                            "COALESCE(so.note,'') AS note, "
                            "so.partner_id, so.company_id "
                            "FROM sale_order so WHERE so.id=$1",
                            pqxx::params{recordId});
                        if (rows.empty()) throw std::runtime_error("Sale order not found: " + std::to_string(recordId));
                        const auto& r = rows[0];
                        companyId = r["company_id"].is_null() ? 1 : r["company_id"].as<int>();
                        partnerId = r["partner_id"].is_null() ? 0 : r["partner_id"].as<int>();

                        std::string soState = safeStr(r["state"]);
                        vars["document_title"] = (soState == "sale" || soState == "done") ? "Sales Order" : "Quotation";
                        vars["doc_number"]     = safeStr(r["name"]);
                        vars["doc_date"]       = ymdToDisplay(safeStr(r["date_order"]));
                        vars["validity_date"]  = ymdToDisplay(safeStr(r["validity_date"]));
                        vars["amount_untaxed"] = fmtMoneyField(r["amount_untaxed"]);
                        vars["amount_tax"]     = fmtMoneyField(r["amount_tax"]);
                        vars["amount_total"]   = fmtMoneyField(r["amount_total"]);

                        // Lines
                        auto lrows = txn.exec(
                            "SELECT COALESCE(sol.name, pp.name, '') AS product_name, "
                            "sol.product_uom_qty AS qty, "
                            "sol.price_unit, "
                            "sol.price_subtotal AS subtotal, "
                            "COALESCE(uu.name,'') AS uom "
                            "FROM sale_order_line sol "
                            "LEFT JOIN product_product pp ON pp.id = sol.product_id "
                            "LEFT JOIN uom_uom uu ON uu.id = sol.product_uom_id "
                            "WHERE sol.order_id = $1 ORDER BY sol.id",
                            pqxx::params{recordId});
                        for (const auto& lr : lrows) {
                            std::map<std::string, std::string> line;
                            line["product_name"] = safeStr(lr["product_name"]);
                            line["qty"]          = fmtPrecF(lr["qty"],      qtyPrec);
                            line["uom"]          = safeStr(lr["uom"]);
                            line["price_unit"]   = fmtPrecF(lr["price_unit"], prcPrec);
                            line["subtotal"]     = fmtPrecF(lr["subtotal"],  subPrec);
                            lines.push_back(line);
                        }

                    } else if (model == "account.move") {
                        auto rows = txn.exec(
                            "SELECT am.name, am.move_type, am.state, "
                            "to_char(am.invoice_date, 'YYYY-MM-DD') AS invoice_date, "
                            "to_char(am.due_date, 'YYYY-MM-DD') AS invoice_date_due, "
                            "COALESCE(am.amount_untaxed::TEXT,'0') AS amount_untaxed, "
                            "COALESCE(am.amount_tax::TEXT,'0') AS amount_tax, "
                            "COALESCE(am.amount_total::TEXT,'0') AS amount_total, "
                            "am.partner_id, am.company_id "
                            "FROM account_move am WHERE am.id=$1",
                            pqxx::params{recordId});
                        if (rows.empty()) throw std::runtime_error("Invoice not found: " + std::to_string(recordId));
                        const auto& r = rows[0];
                        companyId = r["company_id"].is_null() ? 1 : r["company_id"].as<int>();
                        partnerId = r["partner_id"].is_null() ? 0 : r["partner_id"].as<int>();

                        std::string moveType = safeStr(r["move_type"]);
                        vars["document_title"]   = (moveType == "in_invoice") ? "Vendor Bill" :
                                                   (moveType == "out_refund")  ? "Credit Note" :
                                                   (moveType == "in_refund")   ? "Vendor Credit Note" : "Sales Invoice";
                        vars["doc_number"]       = safeStr(r["name"]);
                        vars["doc_date"]         = ymdToDisplay(safeStr(r["invoice_date"]));
                        vars["doc_date_due"]     = ymdToDisplay(safeStr(r["invoice_date_due"]));
                        vars["amount_untaxed"]   = fmtMoneyField(r["amount_untaxed"]);
                        vars["amount_tax"]       = fmtMoneyField(r["amount_tax"]);
                        vars["amount_total"]     = fmtMoneyField(r["amount_total"]);

                        // Lines — include display_type='' (product lines), exclude AR/AP accounting lines
                        auto lrows = txn.exec(
                            "SELECT COALESCE(aml.name,'') AS product_name, "
                            "COALESCE(aml.quantity, 0) AS qty, "
                            "COALESCE(aml.price_unit, 0) AS price_unit, "
                            "COALESCE(NULLIF(aml.price_unit,0) * aml.quantity, aml.debit, 0) AS subtotal, "
                            "COALESCE(NULLIF(aml.display_type,''),'product') AS line_type "
                            "FROM account_move_line aml "
                            "JOIN account_account aa ON aa.id = aml.account_id "
                            "WHERE aml.move_id = $1 "
                            "AND (aml.display_type IS NULL OR aml.display_type IN ('', 'product','line_section','line_note')) "
                            "AND aa.account_type NOT IN ('liability_payable', 'asset_receivable') "
                            "ORDER BY aml.id",
                            pqxx::params{recordId});
                        for (const auto& lr : lrows) {
                            std::map<std::string, std::string> line;
                            line["product_name"] = safeStr(lr["product_name"]);
                            line["qty"]          = fmtPrecF(lr["qty"],       qtyPrec);
                            line["price_unit"]   = fmtPrecF(lr["price_unit"], prcPrec);
                            line["subtotal"]     = fmtPrecF(lr["subtotal"],  subPrec);
                            line["line_type"]    = safeStr(lr["line_type"]);
                            line["uom"]          = "Unit";
                            lines.push_back(line);
                        }

                    } else if (model == "purchase.order") {
                        auto rows = txn.exec(
                            "SELECT po.name, po.state, "
                            "to_char(po.date_order AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS date_order, "
                            "to_char(po.date_planned AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS date_planned, "
                            "COALESCE(po.amount_untaxed::TEXT,'0') AS amount_untaxed, "
                            "COALESCE(po.amount_tax::TEXT,'0') AS amount_tax, "
                            "COALESCE(po.amount_total::TEXT,'0') AS amount_total, "
                            "po.partner_id, po.company_id "
                            "FROM purchase_order po WHERE po.id=$1",
                            pqxx::params{recordId});
                        if (rows.empty()) throw std::runtime_error("Purchase order not found: " + std::to_string(recordId));
                        const auto& r = rows[0];
                        companyId = r["company_id"].is_null() ? 1 : r["company_id"].as<int>();
                        partnerId = r["partner_id"].is_null() ? 0 : r["partner_id"].as<int>();

                        std::string poState = safeStr(r["state"]);
                        vars["document_title"] = (poState == "purchase" || poState == "done") ? "Purchase Order" : "Request for Quotation";
                        vars["doc_number"]     = safeStr(r["name"]);
                        vars["doc_date"]       = ymdToDisplay(safeStr(r["date_order"]));
                        vars["date_planned"]   = ymdToDisplay(safeStr(r["date_planned"]));
                        vars["amount_untaxed"] = fmtMoneyField(r["amount_untaxed"]);
                        vars["amount_tax"]     = fmtMoneyField(r["amount_tax"]);
                        vars["amount_total"]   = fmtMoneyField(r["amount_total"]);

                        // Lines
                        auto lrows = txn.exec(
                            "SELECT COALESCE(pol.name, pp.name, '') AS product_name, "
                            "pol.product_qty AS qty, "
                            "pol.price_unit, "
                            "pol.price_subtotal AS subtotal, "
                            "COALESCE(uu.name,'') AS uom "
                            "FROM purchase_order_line pol "
                            "LEFT JOIN product_product pp ON pp.id = pol.product_id "
                            "LEFT JOIN uom_uom uu ON uu.id = pol.product_uom_id "
                            "WHERE pol.order_id = $1 ORDER BY pol.id",
                            pqxx::params{recordId});
                        for (const auto& lr : lrows) {
                            std::map<std::string, std::string> line;
                            line["product_name"] = safeStr(lr["product_name"]);
                            line["qty"]          = fmtPrecF(lr["qty"],      qtyPrec);
                            line["uom"]          = safeStr(lr["uom"]);
                            line["price_unit"]   = fmtPrecF(lr["price_unit"], prcPrec);
                            line["subtotal"]     = fmtPrecF(lr["subtotal"],  subPrec);
                            lines.push_back(line);
                        }

                    } else if (model == "stock.picking") {
                        auto rows = txn.exec(
                            "SELECT sp.name, sp.origin, sp.state, "
                            "to_char(sp.scheduled_date AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS scheduled_date, "
                            "sp.partner_id, sp.location_id, sp.location_dest_id, sp.company_id, "
                            "COALESCE(spt.code,'') AS picking_type_code "
                            "FROM stock_picking sp "
                            "LEFT JOIN stock_picking_type spt ON spt.id = sp.picking_type_id "
                            "WHERE sp.id=$1",
                            pqxx::params{recordId});
                        if (rows.empty()) throw std::runtime_error("Transfer not found: " + std::to_string(recordId));
                        const auto& r = rows[0];
                        companyId = r["company_id"].is_null() ? 1 : r["company_id"].as<int>();
                        partnerId = r["partner_id"].is_null() ? 0 : r["partner_id"].as<int>();

                        std::string code = safeStr(r["picking_type_code"]);

                        // Location names
                        std::string srcLoc, dstLoc;
                        if (!r["location_id"].is_null()) {
                            auto lrow = txn.exec(
                                "SELECT complete_name FROM stock_location WHERE id=$1",
                                pqxx::params{r["location_id"].as<int>()});
                            if (!lrow.empty()) srcLoc = safeStr(lrow[0]["complete_name"]);
                        }
                        if (!r["location_dest_id"].is_null()) {
                            auto lrow = txn.exec(
                                "SELECT complete_name FROM stock_location WHERE id=$1",
                                pqxx::params{r["location_dest_id"].as<int>()});
                            if (!lrow.empty()) dstLoc = safeStr(lrow[0]["complete_name"]);
                        }

                        vars["document_title"]  = (code == "incoming") ? "Receipt" :
                                                  (code == "outgoing") ? "Delivery Order" : "Internal Transfer";
                        vars["doc_number"]      = safeStr(r["name"]);
                        vars["doc_date"]        = ymdToDisplay(safeStr(r["scheduled_date"]));
                        vars["origin"]          = safeStr(r["origin"]);
                        vars["source_location"] = srcLoc;
                        vars["dest_location"]   = dstLoc;

                        // Lines
                        auto lrows = txn.exec(
                            "SELECT COALESCE(pp.name, sm.name, '') AS product_name, "
                            "sm.product_uom_qty AS demand, "
                            "sm.quantity AS done, "
                            "COALESCE(uu.name,'') AS uom "
                            "FROM stock_move sm "
                            "LEFT JOIN product_product pp ON pp.id = sm.product_id "
                            "LEFT JOIN uom_uom uu ON uu.id = sm.product_uom_id "
                            "WHERE sm.picking_id = $1 ORDER BY sm.id",
                            pqxx::params{recordId});
                        for (const auto& lr : lrows) {
                            std::map<std::string, std::string> line;
                            line["product_name"] = safeStr(lr["product_name"]);
                            line["demand"]       = fmtPrecF(lr["demand"], qtyPrec);
                            line["done"]         = fmtPrecF(lr["done"],   qtyPrec);
                            line["uom"]          = safeStr(lr["uom"]);
                            lines.push_back(line);
                        }
                    } else {
                        throw std::runtime_error("Unsupported model: " + model);
                    }

                    // ---- Company info ----
                    auto crows = txn.exec(
                        "SELECT rc.name, COALESCE(rc.phone,'') AS phone, COALESCE(rc.email,'') AS email, "
                        "COALESCE(rc.website,'') AS website, "
                        "COALESCE(rp.street,'') AS street, COALESCE(rp.city,'') AS city "
                        "FROM res_company rc "
                        "LEFT JOIN res_partner rp ON rp.id = rc.partner_id "
                        "WHERE rc.id = $1",
                        pqxx::params{companyId});

                    std::string companyStreet, companyCity;
                    if (!crows.empty()) {
                        vars["company_name"]    = safeStr(crows[0]["name"]);
                        vars["company_phone"]   = safeStr(crows[0]["phone"]);
                        vars["company_email"]   = safeStr(crows[0]["email"]);
                        vars["company_website"] = safeStr(crows[0]["website"]);
                        companyStreet           = safeStr(crows[0]["street"]);
                        companyCity             = safeStr(crows[0]["city"]);
                    } else {
                        vars["company_name"]    = "";
                        vars["company_phone"]   = "";
                        vars["company_email"]   = "";
                        vars["company_website"] = "";
                    }

                    // ---- Load report config params ----
                    std::string regNumber   = loadCfg("report.reg_number");
                    std::string addr1       = loadCfg("report.addr1");
                    std::string addr2       = loadCfg("report.addr2");
                    std::string addr3       = loadCfg("report.addr3");
                    std::string cityCountry = loadCfg("report.city_country");
                    std::string currCode    = loadCfg("report.currency_code", "MYR");
                    std::string ptDays      = loadCfg("report.payment_term_days", "30");
                    std::string bankAccName = loadCfg("report.bank.account_name");
                    std::string bankAccNo   = loadCfg("report.bank.account_no");
                    std::string bankName    = loadCfg("report.bank.bank_name");
                    std::string bankAddr    = loadCfg("report.bank.bank_address");
                    std::string bankSwift   = loadCfg("report.bank.swift_code");

                    vars["company_reg"]          = regNumber;
                    vars["company_addr1"]        = addr1.empty() ? companyStreet : addr1;
                    vars["company_addr2"]        = addr2;
                    vars["company_addr3"]        = addr3;
                    vars["company_city_country"] = cityCountry.empty() ? companyCity : cityCountry;
                    vars["currency_code"]        = currCode;
                    vars["payment_term_days"]    = ptDays;
                    vars["bank_account_name"]    = bankAccName;
                    vars["bank_account_no"]      = bankAccNo;
                    vars["bank_name"]            = bankName;
                    vars["bank_address"]         = bankAddr;
                    vars["bank_swift"]           = bankSwift;

                    // ---- Partner info ----
                    if (partnerId > 0) {
                        auto prows = txn.exec(
                            "SELECT COALESCE(name,'') AS name, COALESCE(street,'') AS street, "
                            "COALESCE(city,'') AS city, COALESCE(phone,'') AS phone, "
                            "COALESCE(company_name,'') AS company_name, "
                            "COALESCE(is_company,false) AS is_company "
                            "FROM res_partner WHERE id=$1",
                            pqxx::params{partnerId});
                        if (!prows.empty()) {
                            std::string pName    = safeStr(prows[0]["name"]);
                            std::string compName = safeStr(prows[0]["company_name"]);
                            bool isCompany = prows[0]["is_company"].as<bool>(false);

                            vars["partner_street"] = safeStr(prows[0]["street"]);
                            vars["partner_city"]   = safeStr(prows[0]["city"]);
                            vars["partner_phone"]  = safeStr(prows[0]["phone"]);

                            if (isCompany) {
                                // Company contact: header = company name, attn = same
                                vars["partner_name"] = pName;
                                vars["attn_name"]    = pName;
                            } else {
                                // Individual: header = their company/org name,
                                // attn = personal name
                                vars["partner_name"] = compName.empty() ? pName : compName;
                                vars["attn_name"]    = pName;
                            }
                        }
                    } else {
                        vars["partner_name"]   = "";
                        vars["partner_street"] = "";
                        vars["partner_city"]   = "";
                        vars["partner_phone"]  = "";
                        vars["attn_name"]      = "";
                    }

                    return TemplateRenderer::render(tplHtml, vars, lines);
    }

    // ---------------------------------------------------------------
    // registerRoutes — HTTP route registration
    // ---------------------------------------------------------------
    void registerRoutes() override {
        auto db       = db_;
        auto sessions = services_.sessions();

        auto checkAuth = [sessions](const drogon::HttpRequestPtr& req) -> bool {
            if (!sessions) return false;
            const std::string sid = req->getCookie(SessionManager::cookieName());
            if (sid.empty()) return false;
            auto s = sessions->get(sid);
            return s.has_value() && s->isAuthenticated();
        };

        auto authRedirect = []() -> drogon::HttpResponsePtr {
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setStatusCode(drogon::k302Found);
            r->addHeader("Location", "/#/login");
            return r;
        };

        // HTML route
        drogon::app().registerHandler(
            "/report/html/{1}/{2}",
            [db, checkAuth, authRedirect](const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                 const std::string& model,
                 const std::string& idStr)
            {
                if (!checkAuth(req)) { cb(authRedirect()); return; }
                int recordId = 0;
                try { recordId = std::stoi(idStr); } catch (...) { cb(htmlError(400, "Invalid record id")); return; }
                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    std::string html = renderDoc_(txn, model, recordId);
                    txn.commit();
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k200OK);
                    resp->setContentTypeCode(drogon::CT_TEXT_HTML);
                    resp->setBody(html);
                    cb(resp);
                } catch (const std::runtime_error& ex) {
                    cb(htmlError(404, ex.what()));
                } catch (const std::exception& ex) {
                    cb(htmlError(500, std::string("Internal error: ") + ex.what()));
                }
            },
            {drogon::Get}
        );

        // PDF route
        drogon::app().registerHandler(
            "/report/pdf/{1}/{2}",
            [db, checkAuth, authRedirect](const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                 const std::string& model,
                 const std::string& idStr)
            {
                if (!checkAuth(req)) { cb(authRedirect()); return; }
                int recordId = 0;
                try { recordId = std::stoi(idStr); } catch (...) { cb(htmlError(400, "Invalid record id")); return; }
                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    std::string html = renderDoc_(txn, model, recordId);

                    // Read template layout settings for PDF generation
                    double pdfMarginTop = 15, pdfMarginRight = 18, pdfMarginBottom = 18, pdfMarginLeft = 18;
                    int    pdfFontSize  = 10;
                    std::string pdfFontColor = "#333333";
                    double pdfLineHeight = 1.5;
                    std::string pdfPaperFormat = "A4";
                    std::string pdfFooterOverride;
                    try {
                        auto srows = txn.exec(
                            "SELECT COALESCE(margin_top,15)::float AS mt, COALESCE(margin_right,18)::float AS mr, "
                            "COALESCE(margin_bottom,18)::float AS mb, COALESCE(margin_left,18)::float AS ml, "
                            "COALESCE(font_size,10) AS fs, COALESCE(font_color,'#333333') AS fc, "
                            "COALESCE(line_height,1.5)::float AS lh, COALESCE(paper_format,'A4') AS pf, "
                            "COALESCE(footer_text,'') AS ft "
                            "FROM ir_report_template WHERE model=$1 AND active=true ORDER BY id LIMIT 1",
                            pqxx::params{model});
                        if (!srows.empty()) {
                            pdfMarginTop    = srows[0]["mt"].as<double>(15);
                            pdfMarginRight  = srows[0]["mr"].as<double>(18);
                            pdfMarginBottom = srows[0]["mb"].as<double>(18);
                            pdfMarginLeft   = srows[0]["ml"].as<double>(18);
                            pdfFontSize     = srows[0]["fs"].as<int>(10);
                            pdfFontColor    = safeStr(srows[0]["fc"]);
                            pdfLineHeight   = srows[0]["lh"].as<double>(1.5);
                            pdfPaperFormat  = safeStr(srows[0]["pf"]);
                            pdfFooterOverride = safeStr(srows[0]["ft"]);
                        }
                    } catch (...) {}
                    txn.commit();

                    std::string tmpBase   = "/tmp/erp_report_" + model + "_" + idStr;
                    std::string tmpHtml   = tmpBase + ".html";
                    std::string tmpPdf    = tmpBase + ".pdf";

                    // --- If template settings include a footer_text override, inject a
                    //     page-footer div into the HTML if none is already present.
                    //     wkhtmltopdf 0.12.6 (unpatched Qt) silently ignores --footer-html,
                    //     so the footer lives in the main HTML as position:fixed;bottom:0.
                    {
                        const std::string marker = "class=\"page-footer\"";
                        if (!pdfFooterOverride.empty() && html.find(marker) == std::string::npos) {
                            // No footer block in template — inject one from template settings
                            const std::string inject =
                                "<div class=\"page-footer\">" + pdfFooterOverride + "</div>\n";
                            size_t bodyEnd = html.find("</body>");
                            if (bodyEnd != std::string::npos)
                                html.insert(bodyEnd, inject);
                        }
                    }

                    // --- Inject PDF-specific CSS overrides before </head> ---
                    // Remove body padding (wkhtmltopdf --margin-* handle margins externally).
                    // Do NOT override min-height: the template CSS sets min-height:257mm which
                    // keeps the wkhtmltopdf viewport at full page height, so position:fixed;bottom:0
                    // anchors to the page bottom rather than the content bottom.
                    // padding-bottom reserves space so content doesn't slide under the fixed footer.
                    std::ostringstream cssOss;
                    cssOss << "<style>"
                           << "body{padding:0!important;padding-bottom:12mm!important;"
                           << "font-size:" << pdfFontSize << "pt!important;"
                           << "color:" << pdfFontColor << "!important;"
                           << "line-height:" << std::fixed << std::setprecision(2) << pdfLineHeight << "!important;}"
                           << ".page-footer{position:fixed!important;bottom:0!important;"
                           << "left:0!important;right:0!important;width:100%!important;"
                           << "display:block!important;}"
                           << ".print-btn{display:none!important;}"
                           << "</style>";
                    const std::string pdfCss = cssOss.str();
                    {
                        size_t hEnd = html.find("</head>");
                        if (hEnd != std::string::npos) html.insert(hEnd, pdfCss);
                    }

                    // --- Write main HTML ---
                    { std::ofstream f(tmpHtml); f << html; }

                    // --- Run wkhtmltopdf ---
                    // --margin-* controls the page margins (printable area).
                    // Footer is embedded in HTML as position:fixed;bottom:0 because
                    // wkhtmltopdf 0.12.6 (unpatched Qt) does not support --footer-html.
                    auto mmStr = [](double v) {
                        std::ostringstream s;
                        s << std::fixed << std::setprecision(1) << v << "mm";
                        return s.str();
                    };
                    std::string cmd = std::string("wkhtmltopdf --quiet")
                        + " --page-size "    + (pdfPaperFormat.empty() ? "A4" : pdfPaperFormat)
                        + " --margin-top "   + mmStr(pdfMarginTop)
                        + " --margin-right " + mmStr(pdfMarginRight)
                        + " --margin-bottom "+ mmStr(pdfMarginBottom)
                        + " --margin-left "  + mmStr(pdfMarginLeft)
                        + " --enable-local-file-access"
                        + " \"" + tmpHtml + "\""
                        + " \"" + tmpPdf  + "\""
                        + " 2>/dev/null";
                    int ret = std::system(cmd.c_str());
                    std::remove(tmpHtml.c_str());

                    if (ret != 0) {
                        std::remove(tmpPdf.c_str());
                        cb(htmlError(503, "PDF generation failed. Ensure wkhtmltopdf is installed on the server."));
                        return;
                    }

                    // --- Read and return PDF ---
                    std::ifstream pdfFile(tmpPdf, std::ios::binary);
                    std::string pdfData((std::istreambuf_iterator<char>(pdfFile)),
                                        std::istreambuf_iterator<char>());
                    pdfFile.close();
                    std::remove(tmpPdf.c_str());

                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k200OK);
                    resp->setContentTypeString("application/pdf");
                    resp->addHeader("Content-Disposition",
                        "inline; filename=\"" + model + "_" + idStr + ".pdf\"");
                    resp->setBody(pdfData);
                    cb(resp);

                } catch (const std::runtime_error& ex) {
                    cb(htmlError(404, ex.what()));
                } catch (const std::exception& ex) {
                    cb(htmlError(500, std::string("Internal error: ") + ex.what()));
                }
            },
            {drogon::Get}
        );

        // Preview route — renders template with dummy data (no real record needed)
        // NOTE: uses /report/preview/ (not /report/html/) to avoid collision with /report/html/{model}/{id}
        drogon::app().registerHandler(
            "/report/preview/{1}",
            [db](const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                 const std::string& model)
            {
                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};

                    // ---- Load template ----
                    auto tplRows = txn.exec(
                        "SELECT template_html, paper_format, orientation "
                        "FROM ir_report_template "
                        "WHERE model=$1 AND active=true ORDER BY id LIMIT 1",
                        pqxx::params{model});

                    if (tplRows.empty()) {
                        cb(htmlError(404, "No template found for model: " + model));
                        return;
                    }

                    std::string tplHtml     = safeStr(tplRows[0]["template_html"]);
                    std::string paperFormat = safeStr(tplRows[0]["paper_format"]);
                    std::string orientation = safeStr(tplRows[0]["orientation"]);
                    if (paperFormat.empty()) paperFormat = "A4";
                    if (orientation.empty()) orientation = "portrait";

                    std::map<std::string, std::string> vars;
                    std::vector<std::map<std::string, std::string>> lines;

                    vars["paper_format"] = paperFormat;
                    vars["orientation"]  = orientation;

                    // Helper: load a config param from ir_config_parameter
                    auto loadCfg = [&](const std::string& key, const std::string& def = "") -> std::string {
                        try {
                            auto r = txn.exec(
                                "SELECT value FROM ir_config_parameter WHERE key=$1",
                                pqxx::params{key});
                            if (!r.empty() && !r[0]["value"].is_null()) {
                                std::string v = r[0]["value"].c_str();
                                if (!v.empty()) return v;
                            }
                        } catch (...) {}
                        return def;
                    };

                    // ---- Company info from ir_config_parameter ----
                    vars["company_name"]         = loadCfg("company.name", "Demo Company Sdn. Bhd.");
                    vars["company_phone"]        = loadCfg("company.phone", "+603-2181 8000");
                    vars["company_email"]        = loadCfg("company.email", "info@democompany.com");
                    vars["company_website"]      = loadCfg("company.website", "www.democompany.com");
                    vars["company_reg"]          = loadCfg("report.reg_number", "123456-A");
                    vars["company_addr1"]        = loadCfg("report.addr1", "Level 10, Menara Demo");
                    vars["company_addr2"]        = loadCfg("report.addr2", "Jalan Ampang");
                    vars["company_addr3"]        = loadCfg("report.addr3", "");
                    vars["company_city_country"] = loadCfg("report.city_country", "50450 Kuala Lumpur, Malaysia");
                    vars["currency_code"]        = loadCfg("report.currency_code", "MYR");
                    vars["payment_term_days"]    = loadCfg("report.payment_term_days", "30");
                    vars["bank_account_name"]    = loadCfg("report.bank.account_name", "Demo Company Sdn. Bhd.");
                    vars["bank_account_no"]      = loadCfg("report.bank.account_no", "1234567890");
                    vars["bank_name"]            = loadCfg("report.bank.bank_name", "Maybank Berhad");
                    vars["bank_address"]         = loadCfg("report.bank.bank_address", "Jalan Tun Perak, Kuala Lumpur");
                    vars["bank_swift"]           = loadCfg("report.bank.swift_code", "MBBEMYKL");

                    // ---- Dummy partner info ----
                    vars["partner_name"]   = "ABC Technology Sdn. Bhd.";
                    vars["partner_street"] = "Level 3, Menara KL";
                    vars["partner_city"]   = "50088 Kuala Lumpur, Malaysia";
                    vars["partner_phone"]  = "+603-2181 9000";
                    vars["attn_name"]      = "Mr. John Doe";

                    // ---- Model-specific dummy data ----
                    if (model == "account.move") {
                        vars["document_title"] = "Sales Invoice";
                        vars["doc_number"]     = "INV/2025/0001";
                        vars["doc_date"]       = "01/03/2025";
                        vars["doc_date_due"]   = "31/03/2025";
                        vars["amount_untaxed"] = "10,000.00";
                        vars["amount_tax"]     = "600.00";
                        vars["amount_total"]   = "10,600.00";
                    } else if (model == "sale.order") {
                        vars["document_title"] = "Sales Order";
                        vars["doc_number"]     = "SO/2025/0001";
                        vars["doc_date"]       = "01/03/2025";
                        vars["validity_date"]  = "31/03/2025";
                        vars["amount_untaxed"] = "10,000.00";
                        vars["amount_tax"]     = "600.00";
                        vars["amount_total"]   = "10,600.00";
                    } else if (model == "purchase.order") {
                        vars["document_title"] = "Purchase Order";
                        vars["doc_number"]     = "PO/2025/0001";
                        vars["doc_date"]       = "01/03/2025";
                        vars["date_planned"]   = "15/03/2025";
                        vars["amount_untaxed"] = "8,000.00";
                        vars["amount_tax"]     = "480.00";
                        vars["amount_total"]   = "8,480.00";
                    } else if (model == "stock.picking") {
                        vars["document_title"]  = "Delivery Order";
                        vars["doc_number"]      = "WH/OUT/2025/0001";
                        vars["doc_date"]        = "01/03/2025";
                        vars["origin"]          = "SO/2025/0001";
                        vars["source_location"] = "WH/Stock";
                        vars["dest_location"]   = "Customers";
                    } else {
                        vars["document_title"] = "Document";
                        vars["doc_number"]     = "DOC/2025/0001";
                        vars["doc_date"]       = "01/03/2025";
                        vars["amount_untaxed"] = "10,000.00";
                        vars["amount_tax"]     = "600.00";
                        vars["amount_total"]   = "10,600.00";
                    }

                    // ---- Dummy lines ----
                    if (model == "stock.picking") {
                        lines.push_back({{"product_name","Industrial Motor 5kW"},{"demand","2.00"},{"done","2.00"},{"uom","Unit"}});
                        lines.push_back({{"product_name","Control Panel Assembly"},{"demand","1.00"},{"done","1.00"},{"uom","Unit"}});
                        lines.push_back({{"product_name","Installation Service"},{"demand","1.00"},{"done","1.00"},{"uom","Job"}});
                    } else {
                        lines.push_back({{"product_name","Industrial Motor 5kW"},{"qty","2.00"},{"uom","Unit"},{"price_unit","2,500.00"},{"subtotal","5,000.00"},{"line_type","product"}});
                        lines.push_back({{"product_name","Control Panel Assembly"},{"qty","1.00"},{"uom","Unit"},{"price_unit","3,000.00"},{"subtotal","3,000.00"},{"line_type","product"}});
                        lines.push_back({{"product_name","Installation Service"},{"qty","1.00"},{"uom","Job"},{"price_unit","2,000.00"},{"subtotal","2,000.00"},{"line_type","product"}});
                    }

                    std::string rendered = TemplateRenderer::render(tplHtml, vars, lines);

                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k200OK);
                    resp->setContentTypeCode(drogon::CT_TEXT_HTML);
                    resp->setBody(rendered);
                    cb(resp);

                } catch (const std::exception& ex) {
                    cb(htmlError(500, std::string("Internal error: ") + ex.what()));
                }
            },
            {drogon::Get}
        );
    }

    void initialize() override {
        ensureSchema_();
        seedTemplates_();
        seedConfigParams_();
        seedMenuEntries_();
        seedConfigParams_extra_();
    }

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;
    std::shared_ptr<DbConnection> db_;

    void ensureSchema_() {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(
            "CREATE TABLE IF NOT EXISTS ir_report_template ("
            "  id            SERIAL PRIMARY KEY, "
            "  name          TEXT NOT NULL, "
            "  model         TEXT NOT NULL, "
            "  template_html TEXT NOT NULL DEFAULT '', "
            "  paper_format  TEXT NOT NULL DEFAULT 'A4', "
            "  orientation   TEXT NOT NULL DEFAULT 'portrait', "
            "  active        BOOLEAN NOT NULL DEFAULT true "
            ")");
        // Add decimal precision columns if not yet present
        txn.exec(
            "ALTER TABLE ir_report_template "
            "ADD COLUMN IF NOT EXISTS decimal_qty INTEGER NOT NULL DEFAULT 2");
        txn.exec(
            "ALTER TABLE ir_report_template "
            "ADD COLUMN IF NOT EXISTS decimal_price INTEGER NOT NULL DEFAULT 2");
        txn.exec(
            "ALTER TABLE ir_report_template "
            "ADD COLUMN IF NOT EXISTS decimal_subtotal INTEGER NOT NULL DEFAULT 2");
        // Page layout settings
        txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS margin_top     NUMERIC(6,2) NOT NULL DEFAULT 15");
        txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS margin_right   NUMERIC(6,2) NOT NULL DEFAULT 18");
        txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS margin_bottom  NUMERIC(6,2) NOT NULL DEFAULT 18");
        txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS margin_left    NUMERIC(6,2) NOT NULL DEFAULT 18");
        txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS font_size      INTEGER      NOT NULL DEFAULT 10");
        txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS font_color     TEXT         NOT NULL DEFAULT '#333333'");
        txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS line_height    NUMERIC(4,2) NOT NULL DEFAULT 1.5");
        txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS footer_text    TEXT         NOT NULL DEFAULT ''");
        txn.commit();
    }

    void seedTemplates_() {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        // Use pqxx params to safely insert large HTML templates
        auto seed = [&](int id, const std::string& name, const std::string& model,
                        const std::string& html, const std::string& paper, const std::string& orient)
        {
            txn.exec(
                "INSERT INTO ir_report_template (id, name, model, template_html, paper_format, orientation) "
                "VALUES ($1,$2,$3,$4,$5,$6) "
                "ON CONFLICT (id) DO NOTHING",
                pqxx::params{id, name, model, html, paper, orient});
        };

        seed(1, "Sales Order",    "sale.order",     SALE_ORDER_TEMPLATE,    "A4", "portrait");
        seed(2, "Invoice",        "account.move",   INVOICE_TEMPLATE,       "A4", "portrait");
        seed(3, "Purchase Order", "purchase.order", PURCHASE_ORDER_TEMPLATE,"A4", "portrait");
        seed(4, "Delivery Order", "stock.picking",  STOCK_PICKING_TEMPLATE, "A4", "portrait");

        txn.exec("SELECT setval('ir_report_template_id_seq', 4, true)");
        txn.commit();
    }

    void seedConfigParams_() {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        txn.exec(R"(
            INSERT INTO ir_config_parameter (key, value) VALUES
                ('report.reg_number',        ''),
                ('report.addr1',             ''),
                ('report.addr2',             ''),
                ('report.addr3',             ''),
                ('report.city_country',      ''),
                ('report.currency_code',     'MYR'),
                ('report.payment_term_days', '30'),
                ('report.bank.account_name', ''),
                ('report.bank.account_no',   ''),
                ('report.bank.bank_name',    ''),
                ('report.bank.bank_address', ''),
                ('report.bank.swift_code',   '')
            ON CONFLICT (key) DO NOTHING
        )");
        txn.commit();
    }

    void seedMenuEntries_() {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        // Action id=30: Document Templates
        txn.exec(
            "INSERT INTO ir_act_window (id, name, res_model, view_mode, path) VALUES "
            "(30, 'Document Templates', 'ir.report.template', 'list,form', 'report-templates') "
            "ON CONFLICT (id) DO UPDATE SET "
            "name=EXCLUDED.name, res_model=EXCLUDED.res_model, view_mode=EXCLUDED.view_mode");

        // Remove the duplicate Settings app tile (id=100) — we reuse id=30 from IrModule
        txn.exec("UPDATE ir_ui_menu SET parent_id=30 WHERE id IN (101,103) AND parent_id=100");
        txn.exec("DELETE FROM ir_ui_menu WHERE id=100");

        // Section under unified Settings (id=101): Technical — parent_id=30
        txn.exec(
            "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
            "(101, 'Technical', 30, 30, NULL) "
            "ON CONFLICT (id) DO UPDATE SET parent_id=30, sequence=30");

        // Document Templates item (id=102) under Technical (id=101)
        txn.exec(
            "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
            "(102, 'Document Templates', 101, 10, 30) "
            "ON CONFLICT (id) DO UPDATE SET parent_id=101");

        // Action id=36: Groups (res.groups)
        txn.exec(
            "INSERT INTO ir_act_window (id, name, res_model, view_mode, path) VALUES "
            "(36, 'Groups', 'res.groups', 'list', 'groups') "
            "ON CONFLICT (id) DO NOTHING");

        // Menu id=105: Groups under Technical (id=101), after Document Templates (seq=10)
        // (id=104 is owned by MrpModule for Bills of Materials under Inventory)
        txn.exec(
            "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
            "(105, 'Groups', 101, 20, 36) "
            "ON CONFLICT (id) DO UPDATE SET name='Groups', parent_id=101, sequence=20, action_id=36");

        // Action id=31: ERP Settings
        txn.exec(
            "INSERT INTO ir_act_window (id, name, res_model, view_mode) VALUES "
            "(31, 'ERP Settings', 'ir.erp.settings', 'list,form') "
            "ON CONFLICT (id) DO NOTHING");

        // Menu id=103: ERP Settings directly under unified Settings (id=30)
        txn.exec(
            "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
            "(103, 'ERP Settings', 30, 25, 31) "
            "ON CONFLICT (id) DO UPDATE SET parent_id=30, sequence=25");

        txn.commit();
    }

    void seedConfigParams_extra_() {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        txn.exec(R"(
            INSERT INTO ir_config_parameter (key, value) VALUES
                ('company.name',               ''),
                ('company.phone',              ''),
                ('company.email',              ''),
                ('company.website',            ''),
                ('report.design.accent_color', '#4a4a4a'),
                ('report.design.font_family',  'Arial, sans-serif')
            ON CONFLICT (key) DO NOTHING
        )");
        txn.commit();
    }
};

} // namespace odoo::modules::report

#include "ReportModule.hpp"
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
#include "ProcessRunner.hpp"
#include "Money.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <drogon/drogon.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <array>
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <set>

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

// P2: every money/price/quantity column the reports read is BIGINT
// micro-units (migrations 901–970). These two helpers are the single funnel
// for all report number formatting, so converting here covers every
// document template at once — nothing downstream needs to know.
//
// Note some queries select `amount_total::TEXT`, which arrives as a string
// rather than an int; reportMicros handles both.
static double reportMicros(const pqxx::field& f) {
    if (f.is_null()) return 0.0;
    try {
        return odoo::core::Money::fromMicros(f.as<long long>(0)).toJson();
    } catch (...) {
        // ::TEXT-cast columns come through as a decimal string of micro-units
        try { return odoo::core::Money::parse(f.c_str()).toJson(); }
        catch (...) { return 0.0; }
    }
}

static std::string fmtMoneyField(const pqxx::field& f) {
    if (f.is_null()) return "0.00";
    return fmtMoney(reportMicros(f));
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
    return fmtPrec(reportMicros(f), prec);   // P2: micro-units → major units
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
    <tr class="row-{{line_type}}">
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
        REGISTER_MUTATOR("write",           handleWrite)
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
            "COALESCE(line_height,1.5)::float AS line_height, COALESCE(footer_text,'') AS footer_text, "
            "COALESCE(footer_show_page_num,true) AS footer_show_page_num, "
            "COALESCE(footer_page_num_fmt,'Page {p} of {t}') AS footer_page_num_fmt, "
            "COALESCE(footer_text_source,'custom') AS footer_text_source, "
            "COALESCE(footer_line_color,'#cccccc') AS footer_line_color, "
            "COALESCE(footer_line_width,0.5)::float AS footer_line_width "
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
            rec["footer_text"]          = safeStr(row["footer_text"]);
            rec["footer_show_page_num"] = row["footer_show_page_num"].is_null() ? true : row["footer_show_page_num"].as<bool>();
            rec["footer_page_num_fmt"]  = safeStr(row["footer_page_num_fmt"]);
            rec["footer_text_source"]   = safeStr(row["footer_text_source"]);
            rec["footer_line_color"]    = safeStr(row["footer_line_color"]);
            rec["footer_line_width"]    = row["footer_line_width"].as<double>(0.5);
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
        std::string footerText      = vals.value("footer_text", "");
        bool footerShowPageNum = vals.contains("footer_show_page_num") && vals["footer_show_page_num"].is_boolean()
                                 ? vals["footer_show_page_num"].get<bool>() : true;
        std::string footerPageNumFmt = vals.value("footer_page_num_fmt", "Page {p} of {t}");
        std::string footerTextSource = vals.value("footer_text_source",  "custom");
        std::string footerLineColor  = vals.value("footer_line_color",   "#cccccc");
        double footerLineWidth = vals.contains("footer_line_width") && vals["footer_line_width"].is_number()
                                 ? vals["footer_line_width"].get<double>() : 0.5;

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
                "font_size=$12, font_color=$13, line_height=$14, footer_text=$15, "
                "footer_show_page_num=$16, footer_page_num_fmt=$17, footer_text_source=$18, "
                "footer_line_color=$19, footer_line_width=$20 "
                "WHERE id IN (" + inClause + ")",
                pqxx::params{templateHtml, paperFormat, orientation, name, decQty, decPrc, decSub,
                             marginTop, marginRight, marginBottom, marginLeft,
                             fontSize, fontColor, lineHeight, footerText,
                             footerShowPageNum, footerPageNumFmt, footerTextSource,
                             footerLineColor, footerLineWidth});
        } else {
            txn.exec(
                "UPDATE ir_report_template SET "
                "template_html=$1, paper_format=$2, orientation=$3, "
                "decimal_qty=$4, decimal_price=$5, decimal_subtotal=$6, "
                "margin_top=$7, margin_right=$8, margin_bottom=$9, margin_left=$10, "
                "font_size=$11, font_color=$12, line_height=$13, footer_text=$14, "
                "footer_show_page_num=$15, footer_page_num_fmt=$16, footer_text_source=$17, "
                "footer_line_color=$18, footer_line_width=$19 "
                "WHERE id IN (" + inClause + ")",
                pqxx::params{templateHtml, paperFormat, orientation, decQty, decPrc, decSub,
                             marginTop, marginRight, marginBottom, marginLeft,
                             fontSize, fontColor, lineHeight, footerText,
                             footerShowPageNum, footerPageNumFmt, footerTextSource,
                             footerLineColor, footerLineWidth});
        }
        txn.commit();
        // S-47: document templates are the injection surface behind S-44 —
        // whoever edits template_html controls what the PDF renderer parses.
        // Template edits must be attributable.
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

// ================================================================
// MODULE IMPLEMENTATIONS
// ================================================================


ReportModule::ReportModule(core::ModelFactory&     modelFactory,
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

std::string ReportModule::moduleName()   const { return "report"; }
std::string ReportModule::version()      const { return "17.0.1.0.0"; }
std::vector<std::string> ReportModule::dependencies() const { return {"base", "sale", "purchase", "stock", "account"}; }

void ReportModule::registerModels()   {}
void ReportModule::registerServices() {}
void ReportModule::registerViews()    {}

void ReportModule::registerViewModels() {
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
    int recordId,
    bool proforma = false)
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

  if(model == "sale.order") {
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
                    vars["document_title"] = proforma
                        ? "Pro-Forma Invoice"
                        : ((soState == "sale" || soState == "done") ? "Sales Order" : "Quotation");
                    vars["doc_number"]     = safeStr(r["name"]);
                    vars["doc_date"]       = ymdToDisplay(safeStr(r["date_order"]));
                    vars["validity_date"]  = ymdToDisplay(safeStr(r["validity_date"]));
                    vars["amount_untaxed"] = fmtMoneyField(r["amount_untaxed"]);
                    vars["amount_tax"]     = fmtMoneyField(r["amount_tax"]);
                    vars["amount_total"]   = fmtMoneyField(r["amount_total"]);

                    // Lines — sections/notes (display_type) render as full-width
                    // annotation rows (styled .row-line_section / .row-line_note).
                    auto lrows = txn.exec(
                        "SELECT COALESCE(sol.name, pp.name, '') AS product_name, "
                        "COALESCE(sol.product_uom_qty, 0) AS qty, "
                        "COALESCE(sol.price_unit, 0) AS price_unit, "
                        "COALESCE(sol.price_subtotal, 0) AS subtotal, "
                        "COALESCE(uu.name,'') AS uom, "
                        "COALESCE(NULLIF(sol.display_type,''),'product') AS line_type "
                        "FROM sale_order_line sol "
                        "LEFT JOIN product_product pp ON pp.id = sol.product_id "
                        "LEFT JOIN uom_uom uu ON uu.id = sol.product_uom_id "
                        "WHERE sol.order_id = $1 ORDER BY sol.id",
                        pqxx::params{recordId});
  for(const auto& lr : lrows) {
                        std::map<std::string, std::string> line;
                        const std::string ltype = safeStr(lr["line_type"]);
                        line["line_type"]    = ltype;
                        line["product_name"] = safeStr(lr["product_name"]);
                        line["uom"]          = safeStr(lr["uom"]);
                        if (ltype == "product") {
                            line["qty"]        = fmtPrecF(lr["qty"],       qtyPrec);
                            line["price_unit"] = fmtPrecF(lr["price_unit"], prcPrec);
                            line["subtotal"]   = fmtPrecF(lr["subtotal"],  subPrec);
                        } else {
                            line["qty"] = ""; line["price_unit"] = ""; line["subtotal"] = "";
                        }
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
                        // P2: price_unit x quantity is micros x micros = scale 12;
                        // divide by 1e6. aml.debit is already scale 6.
                        "COALESCE(NULLIF(aml.price_unit,0) * aml.quantity / 1000000, aml.debit, 0) AS subtotal, "
                        "COALESCE(NULLIF(aml.display_type,''),'product') AS line_type "
                        "FROM account_move_line aml "
                        "JOIN account_account aa ON aa.id = aml.account_id "
                        "WHERE aml.move_id = $1 "
                        "AND (aml.display_type IS NULL OR aml.display_type IN ('', 'product','line_section','line_note')) "
                        "AND aa.account_type NOT IN ('liability_payable', 'asset_receivable') "
                        "ORDER BY aml.id",
                        pqxx::params{recordId});
  for(const auto& lr : lrows) {
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
                        "COALESCE(pol.product_qty, 0) AS qty, "
                        "COALESCE(pol.price_unit, 0) AS price_unit, "
                        "COALESCE(pol.price_subtotal, 0) AS subtotal, "
                        "COALESCE(uu.name,'') AS uom "
                        "FROM purchase_order_line pol "
                        "LEFT JOIN product_product pp ON pp.id = pol.product_id "
                        "LEFT JOIN uom_uom uu ON uu.id = pol.product_uom_id "
                        "WHERE pol.order_id = $1 ORDER BY pol.id",
                        pqxx::params{recordId});
  for(const auto& lr : lrows) {
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
                        "COALESCE(sm.product_uom_qty, 0) AS demand, "
                        "COALESCE(sm.quantity, 0) AS done, "
                        "COALESCE(uu.name,'') AS uom "
                        "FROM stock_move sm "
                        "LEFT JOIN product_product pp ON pp.id = sm.product_id "
                        "LEFT JOIN uom_uom uu ON uu.id = sm.product_uom_id "
                        "WHERE sm.picking_id = $1 ORDER BY sm.id",
                        pqxx::params{recordId});
  for(const auto& lr : lrows) {
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
  if(partnerId > 0) {
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

  if(isCompany) {
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

// ================================================================
// Financial statement reports (docs/081)
//   trial_balance | balance_sheet | profit_loss | general_ledger | aged_receivable
// Computed from POSTED account_move_line (BIGINT micro-units). Returns a uniform
//   { title, subtitle, columns[], rows[] }  where each row is
//   { type: line|section|subtotal|total, cells:[...] }
// so both the on-screen UI and the print view render it the same way.
// ================================================================
static std::string fmtMicros_(long long micros) { return fmtMoney(micros / 1000000.0); }

static nlohmann::json financialReport_(pqxx::work& txn,
                                       const std::string& report,
                                       const std::string& dateFrom,
                                       const std::string& dateTo) {
    using nlohmann::json;
    json out;
    out["report"]    = report;
    out["date_from"] = dateFrom;
    out["date_to"]   = dateTo;
    json rows = json::array();
    auto R = [](const char* type, std::vector<std::string> cells) {
        return json{{"type", type}, {"cells", std::move(cells)}};
    };
    auto cols = [](std::vector<std::string> labels) {
        json c = json::array();
        for (size_t i = 0; i < labels.size(); ++i)
            c.push_back(json{{"label", labels[i]}, {"align", i == 0 ? "left" : "right"}});
        return c;
    };

    // ---- Trial Balance (as of date_to) ----
    if (report == "trial_balance") {
        out["title"]    = "Trial Balance";
        out["subtitle"] = "As at " + ymdToDisplay(dateTo);
        out["columns"]  = cols({"Account", "Debit", "Credit", "Balance"});
        auto r = txn.exec(
            "SELECT a.code, a.name, COALESCE(SUM(l.debit),0) d, COALESCE(SUM(l.credit),0) c "
            "FROM account_account a "
            "LEFT JOIN account_move_line l ON l.account_id=a.id AND l.date <= $1 "
            "  AND EXISTS (SELECT 1 FROM account_move m WHERE m.id=l.move_id AND m.state='posted') "
            "GROUP BY a.id, a.code, a.name "
            "HAVING COALESCE(SUM(l.debit),0)<>0 OR COALESCE(SUM(l.credit),0)<>0 "
            "ORDER BY a.code", pqxx::params{dateTo});
        long long td = 0, tc = 0;
        for (const auto& x : r) {
            long long d = x["d"].as<long long>(0), c = x["c"].as<long long>(0);
            td += d; tc += c;
            rows.push_back(R("line", { safeStr(x["code"]) + "  " + safeStr(x["name"]),
                                       fmtMicros_(d), fmtMicros_(c), fmtMicros_(d - c) }));
        }
        rows.push_back(R("total", { "TOTAL", fmtMicros_(td), fmtMicros_(tc), fmtMicros_(td - tc) }));
    }
    // ---- Profit & Loss (date_from .. date_to) ----
    else if (report == "profit_loss") {
        out["title"]    = "Profit and Loss";
        out["subtitle"] = ymdToDisplay(dateFrom) + " — " + ymdToDisplay(dateTo);
        out["columns"]  = cols({"", "Amount"});
        auto r = txn.exec(
            "SELECT a.code, a.name, a.account_type, "
            "COALESCE(SUM(l.credit),0)-COALESCE(SUM(l.debit),0) AS bal "
            "FROM account_account a "
            "JOIN account_move_line l ON l.account_id=a.id AND l.date BETWEEN $1 AND $2 "
            "JOIN account_move m ON m.id=l.move_id AND m.state='posted' "
            "WHERE a.account_type IN ('income','income_other','expense','expense_depreciation','expense_direct_cost') "
            "GROUP BY a.id, a.code, a.name, a.account_type ORDER BY a.account_type, a.code",
            pqxx::params{dateFrom, dateTo});
        long long income = 0, expense = 0;
        json incRows = json::array(), expRows = json::array();
        for (const auto& x : r) {
            std::string t = safeStr(x["account_type"]);
            long long bal = x["bal"].as<long long>(0);   // credit-debit
            std::string label = safeStr(x["code"]) + "  " + safeStr(x["name"]);
            if (t == "income" || t == "income_other") {
                income += bal;
                incRows.push_back(R("line", { label, fmtMicros_(bal) }));
            } else {
                expense += -bal;   // expense is debit-normal: debit-credit = -bal
                expRows.push_back(R("line", { label, fmtMicros_(-bal) }));
            }
        }
        rows.push_back(R("section", { "Income", "" }));
        for (auto& x : incRows) rows.push_back(x);
        rows.push_back(R("subtotal", { "Total Income", fmtMicros_(income) }));
        rows.push_back(R("section", { "Expenses", "" }));
        for (auto& x : expRows) rows.push_back(x);
        rows.push_back(R("subtotal", { "Total Expenses", fmtMicros_(expense) }));
        rows.push_back(R("total", { "Net Profit", fmtMicros_(income - expense) }));
    }
    // ---- Balance Sheet (as of date_to) ----
    else if (report == "balance_sheet") {
        out["title"]    = "Balance Sheet";
        out["subtitle"] = "As at " + ymdToDisplay(dateTo);
        out["columns"]  = cols({"", "Amount"});
        // Per-account cumulative balance up to date_to (posted only).
        auto r = txn.exec(
            "SELECT a.code, a.name, a.account_type, "
            "COALESCE(SUM(l.debit),0)-COALESCE(SUM(l.credit),0) AS dr, "
            "COALESCE(SUM(l.credit),0)-COALESCE(SUM(l.debit),0) AS cr "
            "FROM account_account a "
            "JOIN account_move_line l ON l.account_id=a.id AND l.date <= $1 "
            "JOIN account_move m ON m.id=l.move_id AND m.state='posted' "
            "GROUP BY a.id, a.code, a.name, a.account_type ORDER BY a.account_type, a.code",
            pqxx::params{dateTo});
        long long assets = 0, liab = 0, equity = 0, earnings = 0;
        json assetRows = json::array(), liabRows = json::array(), eqRows = json::array();
        for (const auto& x : r) {
            std::string t = safeStr(x["account_type"]);
            long long dr = x["dr"].as<long long>(0);   // debit-credit
            long long cr = x["cr"].as<long long>(0);   // credit-debit
            std::string label = safeStr(x["code"]) + "  " + safeStr(x["name"]);
            if (t.rfind("asset", 0) == 0) {
                assets += dr; assetRows.push_back(R("line", { label, fmtMicros_(dr) }));
            } else if (t.rfind("liability", 0) == 0) {
                liab += cr; liabRows.push_back(R("line", { label, fmtMicros_(cr) }));
            } else if (t.rfind("equity", 0) == 0) {
                equity += cr; eqRows.push_back(R("line", { label, fmtMicros_(cr) }));
            } else if (t.rfind("income", 0) == 0 || t.rfind("expense", 0) == 0) {
                earnings += cr;   // income+expense net (credit-debit) = current-year profit
            }
        }
        rows.push_back(R("section", { "Assets", "" }));
        for (auto& x : assetRows) rows.push_back(x);
        rows.push_back(R("subtotal", { "Total Assets", fmtMicros_(assets) }));
        rows.push_back(R("section", { "Liabilities", "" }));
        for (auto& x : liabRows) rows.push_back(x);
        rows.push_back(R("subtotal", { "Total Liabilities", fmtMicros_(liab) }));
        rows.push_back(R("section", { "Equity", "" }));
        for (auto& x : eqRows) rows.push_back(x);
        rows.push_back(R("line", { "Current Year Earnings", fmtMicros_(earnings) }));
        rows.push_back(R("subtotal", { "Total Equity", fmtMicros_(equity + earnings) }));
        rows.push_back(R("total", { "Total Liabilities + Equity", fmtMicros_(liab + equity + earnings) }));
    }
    // ---- General Ledger (date_from .. date_to) ----
    else if (report == "general_ledger") {
        out["title"]    = "General Ledger";
        out["subtitle"] = ymdToDisplay(dateFrom) + " — " + ymdToDisplay(dateTo);
        out["columns"]  = cols({"Date / Entry", "Debit", "Credit", "Balance"});
        auto accs = txn.exec(
            "SELECT DISTINCT a.id, a.code, a.name FROM account_account a "
            "JOIN account_move_line l ON l.account_id=a.id "
            "JOIN account_move m ON m.id=l.move_id AND m.state='posted' AND l.date<=$1 "
            "ORDER BY a.code", pqxx::params{dateTo});
        for (const auto& a : accs) {
            int aid = a["id"].as<int>();
            long long opening = txn.exec(
                "SELECT COALESCE(SUM(l.debit-l.credit),0) FROM account_move_line l "
                "JOIN account_move m ON m.id=l.move_id AND m.state='posted' "
                "WHERE l.account_id=$1 AND l.date < $2", pqxx::params{aid, dateFrom})[0][0].as<long long>(0);
            auto lns = txn.exec(
                "SELECT to_char(l.date,'YYYY-MM-DD') dt, COALESCE(m.name,'') ref, "
                "COALESCE(l.name,'') lbl, l.debit d, l.credit c "
                "FROM account_move_line l JOIN account_move m ON m.id=l.move_id AND m.state='posted' "
                "WHERE l.account_id=$1 AND l.date BETWEEN $2 AND $3 ORDER BY l.date, l.id",
                pqxx::params{aid, dateFrom, dateTo});
            if (lns.empty() && opening == 0) continue;
            rows.push_back(R("section", { safeStr(a["code"]) + "  " + safeStr(a["name"]), "", "", "" }));
            rows.push_back(R("line", { "Opening balance", "", "", fmtMicros_(opening) }));
            long long bal = opening;
            for (const auto& x : lns) {
                long long d = x["d"].as<long long>(0), c = x["c"].as<long long>(0);
                bal += d - c;
                std::string lbl = ymdToDisplay(safeStr(x["dt"])) + "  " + safeStr(x["ref"]);
                std::string note = safeStr(x["lbl"]);
                if (!note.empty()) lbl += " — " + note;
                rows.push_back(R("line", { lbl, fmtMicros_(d), fmtMicros_(c), fmtMicros_(bal) }));
            }
            rows.push_back(R("subtotal", { "Closing balance", "", "", fmtMicros_(bal) }));
        }
    }
    // ---- Aged Receivable (as of date_to) ----
    else if (report == "aged_receivable") {
        out["title"]    = "Aged Receivable";
        out["subtitle"] = "As at " + ymdToDisplay(dateTo);
        out["columns"]  = cols({"Partner", "Not due", "1-30", "31-60", "61-90", "90+", "Total"});
        auto r = txn.exec(
            "SELECT COALESCE(p.name,'(no partner)') partner, m.amount_residual amt, "
            "GREATEST(0, ($1::date - COALESCE(m.due_date, m.invoice_date))) AS age "
            "FROM account_move m LEFT JOIN res_partner p ON p.id=m.partner_id "
            "WHERE m.move_type='out_invoice' AND m.state='posted' AND m.amount_residual>0 "
            "AND m.invoice_date <= $1 ORDER BY p.name", pqxx::params{dateTo});
        // partner -> [notdue,1-30,31-60,61-90,90+]
        std::vector<std::pair<std::string, std::array<long long,5>>> agg;
        std::array<long long,5> grand{0,0,0,0,0};
        for (const auto& x : r) {
            std::string partner = safeStr(x["partner"]);
            long long amt = x["amt"].as<long long>(0);
            int age = x["age"].as<int>(0);
            int b = age <= 0 ? 0 : age <= 30 ? 1 : age <= 60 ? 2 : age <= 90 ? 3 : 4;
            auto it = std::find_if(agg.begin(), agg.end(), [&](auto& e){ return e.first == partner; });
            if (it == agg.end()) { agg.push_back({partner, {0,0,0,0,0}}); it = agg.end() - 1; }
            it->second[b] += amt; grand[b] += amt;
        }
        for (auto& e : agg) {
            long long tot = 0; for (int i=0;i<5;i++) tot += e.second[i];
            rows.push_back(R("line", { e.first,
                fmtMicros_(e.second[0]), fmtMicros_(e.second[1]), fmtMicros_(e.second[2]),
                fmtMicros_(e.second[3]), fmtMicros_(e.second[4]), fmtMicros_(tot) }));
        }
        long long gtot = 0; for (int i=0;i<5;i++) gtot += grand[i];
        rows.push_back(R("total", { "TOTAL",
            fmtMicros_(grand[0]), fmtMicros_(grand[1]), fmtMicros_(grand[2]),
            fmtMicros_(grand[3]), fmtMicros_(grand[4]), fmtMicros_(gtot) }));
    }
    // ---- Tax Report — Malaysian SST-02 output tax (date_from .. date_to) ----
    else if (report == "tax_report") {
        out["title"]    = "Tax Report (SST-02)";
        out["subtitle"] = "Output tax collected  ·  " + ymdToDisplay(dateFrom) + " — " + ymdToDisplay(dateTo);
        out["columns"]  = cols({"Tax", "Rate", "Taxable amount", "Tax"});
        // Output tax per tax code from the posted tax lines of customer invoices
        // and credit notes. base is derived from tax and rate (SST is single-stage,
        // so tax = base × rate exactly).
        auto r = txn.exec(
            "SELECT t.name, t.amount AS rate, "
            "  CASE WHEN t.tax_group IN ('sales','service') THEN t.tax_group ELSE 'other' END AS grp, "
            "  COALESCE(SUM(aml.credit - aml.debit),0) AS tax_amt "
            "FROM account_tax t "
            "JOIN account_move_line aml ON aml.tax_line_id = t.id "
            "JOIN account_move m ON m.id = aml.move_id AND m.state='posted' "
            "  AND m.date BETWEEN $1 AND $2 "
            "  AND m.move_type IN ('out_invoice','out_refund') "
            "GROUP BY t.id, t.name, t.amount, t.tax_group "
            "HAVING COALESCE(SUM(aml.credit - aml.debit),0) <> 0 "
            "ORDER BY CASE WHEN t.tax_group='sales' THEN 1 WHEN t.tax_group='service' THEN 2 ELSE 3 END, t.amount",
            pqxx::params{dateFrom, dateTo});

        auto grpLabel = [](const std::string& g) -> const char* {
            return g == "sales" ? "Sales Tax" : g == "service" ? "Service Tax" : "Other Output Tax";
        };
        auto rateStr = [](double rate) {
            std::ostringstream o;
            if (rate == static_cast<double>(static_cast<long long>(rate))) o << static_cast<long long>(rate);
            else o << rate;
            return o.str() + "%";
        };
        std::string curGrp;
        long long sb = 0, st = 0, gBase = 0, gTax = 0;
        auto flush = [&]() {
            if (curGrp.empty()) return;
            rows.push_back(R("subtotal", { std::string("Total ") + grpLabel(curGrp), "",
                                           fmtMicros_(sb), fmtMicros_(st) }));
            gBase += sb; gTax += st; sb = 0; st = 0;
        };
        for (const auto& x : r) {
            const std::string grp = safeStr(x["grp"]);
            if (grp != curGrp) { flush(); rows.push_back(R("section", { grpLabel(grp), "", "", "" })); curGrp = grp; }
            const double    rate = x["rate"].as<double>(0.0);
            const long long tax  = x["tax_amt"].as<long long>(0);
            const long long base = rate > 0.0
                ? static_cast<long long>(tax * 100.0 / rate + (tax >= 0 ? 0.5 : -0.5)) : 0;
            rows.push_back(R("line", { safeStr(x["name"]), rateStr(rate), fmtMicros_(base), fmtMicros_(tax) }));
            sb += base; st += tax;
        }
        flush();
        rows.push_back(R("total", { "Total Tax Payable", "", fmtMicros_(gBase), fmtMicros_(gTax) }));
    }
    // ---- Aged Payable (as of date_to) — mirror of the receivable ageing ----
    else if (report == "aged_payable") {
        out["title"]    = "Aged Payable";
        out["subtitle"] = "As at " + ymdToDisplay(dateTo);
        out["columns"]  = cols({"Vendor", "Not due", "1-30", "31-60", "61-90", "90+", "Total"});
        auto r = txn.exec(
            "SELECT COALESCE(p.name,'(no vendor)') partner, m.amount_residual amt, "
            "GREATEST(0, ($1::date - COALESCE(m.due_date, m.invoice_date))) AS age "
            "FROM account_move m LEFT JOIN res_partner p ON p.id=m.partner_id "
            "WHERE m.move_type='in_invoice' AND m.state='posted' AND m.amount_residual>0 "
            "AND m.invoice_date <= $1 ORDER BY p.name", pqxx::params{dateTo});
        std::vector<std::pair<std::string, std::array<long long,5>>> agg;
        std::array<long long,5> grand{0,0,0,0,0};
        for (const auto& x : r) {
            std::string partner = safeStr(x["partner"]);
            long long amt = x["amt"].as<long long>(0);
            int age = x["age"].as<int>(0);
            int b = age <= 0 ? 0 : age <= 30 ? 1 : age <= 60 ? 2 : age <= 90 ? 3 : 4;
            auto it = std::find_if(agg.begin(), agg.end(), [&](auto& e){ return e.first == partner; });
            if (it == agg.end()) { agg.push_back({partner, {0,0,0,0,0}}); it = agg.end() - 1; }
            it->second[b] += amt; grand[b] += amt;
        }
        for (auto& e : agg) {
            long long tot = 0; for (int i=0;i<5;i++) tot += e.second[i];
            rows.push_back(R("line", { e.first, fmtMicros_(e.second[0]), fmtMicros_(e.second[1]),
                fmtMicros_(e.second[2]), fmtMicros_(e.second[3]), fmtMicros_(e.second[4]), fmtMicros_(tot) }));
        }
        long long gtot = 0; for (int i=0;i<5;i++) gtot += grand[i];
        rows.push_back(R("total", { "TOTAL", fmtMicros_(grand[0]), fmtMicros_(grand[1]),
            fmtMicros_(grand[2]), fmtMicros_(grand[3]), fmtMicros_(grand[4]), fmtMicros_(gtot) }));
    }
    // ---- Partner Ledger (date_from .. date_to) ----
    else if (report == "partner_ledger") {
        out["title"]    = "Partner Ledger";
        out["subtitle"] = ymdToDisplay(dateFrom) + " — " + ymdToDisplay(dateTo);
        out["columns"]  = cols({"Date / Entry", "Debit", "Credit", "Balance"});
        auto partners = txn.exec(
            "SELECT DISTINCT p.id, p.name FROM res_partner p "
            "JOIN account_move_line l ON l.partner_id=p.id "
            "JOIN account_move m ON m.id=l.move_id AND m.state='posted' AND l.date<=$1 "
            "JOIN account_account a ON a.id=l.account_id "
            "WHERE a.account_type IN ('asset_receivable','liability_payable') "
            "ORDER BY p.name", pqxx::params{dateTo});
        for (const auto& p : partners) {
            const int pid = p["id"].as<int>();
            long long opening = txn.exec(
                "SELECT COALESCE(SUM(l.debit-l.credit),0) FROM account_move_line l "
                "JOIN account_move m ON m.id=l.move_id AND m.state='posted' "
                "JOIN account_account a ON a.id=l.account_id "
                "WHERE l.partner_id=$1 AND l.date < $2 "
                "AND a.account_type IN ('asset_receivable','liability_payable')",
                pqxx::params{pid, dateFrom})[0][0].as<long long>(0);
            auto lns = txn.exec(
                "SELECT to_char(l.date,'YYYY-MM-DD') dt, COALESCE(m.name,'') ref, "
                "COALESCE(l.name,'') lbl, l.debit d, l.credit c "
                "FROM account_move_line l JOIN account_move m ON m.id=l.move_id AND m.state='posted' "
                "JOIN account_account a ON a.id=l.account_id "
                "WHERE l.partner_id=$1 AND l.date BETWEEN $2 AND $3 "
                "AND a.account_type IN ('asset_receivable','liability_payable') "
                "ORDER BY l.date, l.id", pqxx::params{pid, dateFrom, dateTo});
            if (lns.empty() && opening == 0) continue;
            rows.push_back(R("section", { safeStr(p["name"]), "", "", "" }));
            rows.push_back(R("line", { "Opening balance", "", "", fmtMicros_(opening) }));
            long long bal = opening;
            for (const auto& x : lns) {
                long long d = x["d"].as<long long>(0), c = x["c"].as<long long>(0);
                bal += d - c;
                std::string lbl = ymdToDisplay(safeStr(x["dt"])) + "  " + safeStr(x["ref"]);
                const std::string note = safeStr(x["lbl"]);
                if (!note.empty()) lbl += " — " + note;
                rows.push_back(R("line", { lbl, fmtMicros_(d), fmtMicros_(c), fmtMicros_(bal) }));
            }
            rows.push_back(R("subtotal", { "Closing balance", "", "", fmtMicros_(bal) }));
        }
    }
    // ---- Journals Audit (date_from .. date_to) ----
    else if (report == "journals_audit") {
        out["title"]    = "Journals Audit";
        out["subtitle"] = ymdToDisplay(dateFrom) + " — " + ymdToDisplay(dateTo);
        out["columns"]  = cols({"Journal / Entry", "Entries", "Debit", "Credit"});
        auto js = txn.exec(
            "SELECT j.id, j.name, j.code, COUNT(DISTINCT m.id) n, "
            "COALESCE(SUM(l.debit),0) d, COALESCE(SUM(l.credit),0) c "
            "FROM account_journal j "
            "JOIN account_move m ON m.journal_id=j.id AND m.state='posted' AND m.date BETWEEN $1 AND $2 "
            "JOIN account_move_line l ON l.move_id=m.id "
            "GROUP BY j.id, j.name, j.code ORDER BY j.code",
            pqxx::params{dateFrom, dateTo});
        long long td = 0, tc = 0; long long tn = 0;
        for (const auto& j : js) {
            const long long d = j["d"].as<long long>(0), c = j["c"].as<long long>(0);
            const long long n = j["n"].as<long long>(0);
            td += d; tc += c; tn += n;
            rows.push_back(R("section", { safeStr(j["code"]) + "  " + safeStr(j["name"]),
                                          std::to_string(n), fmtMicros_(d), fmtMicros_(c) }));
            auto ms = txn.exec(
                "SELECT to_char(m.date,'YYYY-MM-DD') dt, COALESCE(m.name,'') nm, "
                "COALESCE(SUM(l.debit),0) d, COALESCE(SUM(l.credit),0) c "
                "FROM account_move m JOIN account_move_line l ON l.move_id=m.id "
                "WHERE m.journal_id=$1 AND m.state='posted' AND m.date BETWEEN $2 AND $3 "
                "GROUP BY m.id, m.date, m.name ORDER BY m.date, m.id LIMIT 500",
                pqxx::params{j["id"].as<int>(), dateFrom, dateTo});
            for (const auto& m : ms)
                rows.push_back(R("line", { ymdToDisplay(safeStr(m["dt"])) + "  " + safeStr(m["nm"]), "",
                                           fmtMicros_(m["d"].as<long long>(0)),
                                           fmtMicros_(m["c"].as<long long>(0)) }));
        }
        rows.push_back(R("total", { "TOTAL", std::to_string(tn), fmtMicros_(td), fmtMicros_(tc) }));
    }
    // ---- Invoice Analysis (date_from .. date_to) ----
    else if (report == "invoice_analysis") {
        out["title"]    = "Invoice Analysis";
        out["subtitle"] = ymdToDisplay(dateFrom) + " — " + ymdToDisplay(dateTo);
        out["columns"]  = cols({"Customer", "Invoices", "Untaxed", "Tax", "Total", "Outstanding"});
        auto r = txn.exec(
            "SELECT COALESCE(p.name,'(no customer)') partner, COUNT(*) n, "
            "COALESCE(SUM(m.amount_untaxed),0) u, COALESCE(SUM(m.amount_tax),0) t, "
            "COALESCE(SUM(m.amount_total),0) g, COALESCE(SUM(m.amount_residual),0) res "
            "FROM account_move m LEFT JOIN res_partner p ON p.id=m.partner_id "
            "WHERE m.move_type IN ('out_invoice','out_refund') AND m.state='posted' "
            "AND m.date BETWEEN $1 AND $2 "
            "GROUP BY p.name ORDER BY SUM(m.amount_total) DESC",
            pqxx::params{dateFrom, dateTo});
        long long n = 0, u = 0, t = 0, g = 0, res = 0;
        for (const auto& x : r) {
            n += x["n"].as<long long>(0); u += x["u"].as<long long>(0);
            t += x["t"].as<long long>(0); g += x["g"].as<long long>(0);
            res += x["res"].as<long long>(0);
            rows.push_back(R("line", { safeStr(x["partner"]), std::to_string(x["n"].as<long long>(0)),
                fmtMicros_(x["u"].as<long long>(0)), fmtMicros_(x["t"].as<long long>(0)),
                fmtMicros_(x["g"].as<long long>(0)), fmtMicros_(x["res"].as<long long>(0)) }));
        }
        rows.push_back(R("total", { "TOTAL", std::to_string(n), fmtMicros_(u), fmtMicros_(t),
                                    fmtMicros_(g), fmtMicros_(res) }));
    }
    // ---- Product Margins (date_from .. date_to) ----
    else if (report == "product_margins") {
        out["title"]    = "Product Margins";
        out["subtitle"] = ymdToDisplay(dateFrom) + " — " + ymdToDisplay(dateTo);
        out["columns"]  = cols({"Product", "Qty sold", "Revenue", "Cost", "Margin", "Margin %"});
        // Revenue from confirmed sale order lines; cost from the product's standard cost.
        auto r = txn.exec(
            "SELECT COALESCE(pp.name, sol.name, '(no product)') AS product, "
            "COALESCE(SUM(sol.product_uom_qty),0) qty, "
            "COALESCE(SUM(sol.price_subtotal),0) rev, "
            "COALESCE(SUM(sol.product_uom_qty * COALESCE(pp.standard_price,0) / 1000000),0) cost "
            "FROM sale_order_line sol "
            "JOIN sale_order so ON so.id=sol.order_id AND so.state IN ('sale','done') "
            "LEFT JOIN product_product pp ON pp.id=sol.product_id "
            "WHERE COALESCE(sol.display_type,'')='' "
            "AND so.date_order::date BETWEEN $1 AND $2 "
            "GROUP BY COALESCE(pp.name, sol.name, '(no product)') "
            "ORDER BY SUM(sol.price_subtotal) DESC LIMIT 200",
            pqxx::params{dateFrom, dateTo});
        long long trev = 0, tcost = 0;
        for (const auto& x : r) {
            const long long rev = x["rev"].as<long long>(0);
            const long long cost = x["cost"].as<long long>(0);
            const long long margin = rev - cost;
            trev += rev; tcost += cost;
            std::ostringstream pct;
            if (rev != 0) pct << std::fixed << std::setprecision(1) << (margin * 100.0 / rev) << "%";
            else pct << "—";
            rows.push_back(R("line", { safeStr(x["product"]),
                fmtMicros_(x["qty"].as<long long>(0)), fmtMicros_(rev), fmtMicros_(cost),
                fmtMicros_(margin), pct.str() }));
        }
        std::ostringstream tpct;
        if (trev != 0) tpct << std::fixed << std::setprecision(1) << ((trev - tcost) * 100.0 / trev) << "%";
        else tpct << "—";
        rows.push_back(R("total", { "TOTAL", "", fmtMicros_(trev), fmtMicros_(tcost),
                                    fmtMicros_(trev - tcost), tpct.str() }));
    }
    else {
        throw std::runtime_error("Unknown report: " + report);
    }

    out["rows"] = rows;
    return out;
}

// Self-contained printable HTML for a financial report (browser → PDF).
static std::string financialReportHtml_(const nlohmann::json& rep,
                                         const std::string& company) {
    std::ostringstream h;
    const size_t ncol = rep["columns"].size();
    h << "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><title>"
      << rep.value("title","Report") << "</title><style>"
      << "body{font-family:Arial,Helvetica,sans-serif;color:#222;margin:24px;font-size:12px;}"
      << "h1{font-size:18px;margin:0 0 2px;} .sub{color:#666;margin:0 0 16px;font-size:12px;}"
      << ".co{font-weight:bold;font-size:13px;margin-bottom:2px;}"
      << "table{width:100%;border-collapse:collapse;} th,td{padding:5px 8px;}"
      << "th{border-bottom:2px solid #333;text-align:right;font-size:11px;text-transform:uppercase;}"
      << "th:first-child{text-align:left;} td{border-bottom:1px solid #eee;text-align:right;}"
      << "td:first-child{text-align:left;} tr.section td{font-weight:bold;background:#f5f5f5;border-top:1px solid #ccc;}"
      << "tr.subtotal td{font-weight:bold;border-top:1px solid #999;} "
      << "tr.total td{font-weight:bold;border-top:2px solid #333;border-bottom:2px solid #333;font-size:13px;}"
      << "@media print{body{margin:0;}}</style></head><body>";
    if (!company.empty()) h << "<div class=\"co\">" << company << "</div>";
    h << "<h1>" << rep.value("title","") << "</h1>";
    h << "<p class=\"sub\">" << rep.value("subtitle","") << "</p>";
    h << "<table><thead><tr>";
    for (const auto& c : rep["columns"]) h << "<th>" << c.value("label","") << "</th>";
    h << "</tr></thead><tbody>";
    for (const auto& row : rep["rows"]) {
        h << "<tr class=\"" << row.value("type","line") << "\">";
        const auto& cells = row["cells"];
        for (size_t i = 0; i < ncol; ++i)
            h << "<td>" << (i < cells.size() ? cells[i].get<std::string>() : std::string()) << "</td>";
        h << "</tr>";
    }
    h << "</tbody></table></body></html>";
    return h.str();
}

// ---------------------------------------------------------------
// registerRoutes — HTTP route registration
// ---------------------------------------------------------------
void ReportModule::registerRoutes() {
    auto db       = db_;
    auto sessions = services_.sessions();
    bool devMode  = services_.devMode();  // SEC-28: gate ex.what() disclosure

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

    // Accounting settings — read/write the config parameters behind the
    // Settings screen. GET returns them all; GET with ?key=&value= saves one.
    // Lock dates are enforced in AccountMoveViewModel::handleActionPost. (docs/088)
    drogon::app().registerHandler(
        "/web/account/settings",
        [db, checkAuth, devMode](const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            if (!checkAuth(req)) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k401Unauthorized); cb(r); return;
            }
            static const std::vector<std::string> kKeys = {
                "account.fiscal_year_last_day", "account.fiscal_year_last_month",
                "account.lock_date", "account.tax_lock_date",
                "account.tax_periodicity", "account.tax_reminder_day",
                "account.default_sale_tax_id", "account.default_purchase_tax_id",
                "account.default_sale_journal_id", "account.default_purchase_journal_id",
                "account.multi_currency", "account.tax_rounding",
            };
            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                const std::string key = req->getParameter("key");
                if (!key.empty()) {
                    if (std::find(kKeys.begin(), kKeys.end(), key) == kKeys.end())
                        throw std::runtime_error("Unknown setting: " + key);   // allowlist (S-49 spirit)
                    txn.exec("INSERT INTO ir_config_parameter (key, value) VALUES ($1,$2) "
                             "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value",
                             pqxx::params{key, req->getParameter("value")});
                }
                nlohmann::json vals = nlohmann::json::object();
                for (const auto& k : kKeys) {
                    auto r = txn.exec("SELECT value FROM ir_config_parameter WHERE key=$1", pqxx::params{k});
                    vals[k] = (r.empty() || r[0][0].is_null()) ? "" : std::string(r[0][0].c_str());
                }
                // Reference lists the Settings screen offers as choices.
                nlohmann::json taxes = nlohmann::json::array(), journals = nlohmann::json::array();
                for (const auto& t : txn.exec("SELECT id, name, type_tax_use FROM account_tax WHERE active ORDER BY id"))
                    taxes.push_back({{"id", t["id"].as<int>()}, {"name", safeStr(t["name"])},
                                     {"scope", safeStr(t["type_tax_use"])}});
                for (const auto& j : txn.exec("SELECT id, name, type FROM account_journal ORDER BY id"))
                    journals.push_back({{"id", j["id"].as<int>()}, {"name", safeStr(j["name"])},
                                        {"type", safeStr(j["type"])}});
                txn.commit();
                nlohmann::json out;
                out["values"] = vals; out["taxes"] = taxes; out["journals"] = journals;
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                resp->setBody(out.dump());
                cb(resp);
            } catch (const PoolExhaustedException& ex) {
                cb(htmlError(503, "The server is temporarily overloaded. Please retry."));
            } catch (const std::exception& ex) {
                LOG_ERROR << "[account/settings] " << ex.what();
                cb(htmlError(500, devMode ? ex.what() : "An internal error occurred"));
            }
        },
        {drogon::Get});

    // Accounting dashboard — the journal cards, plus which ones are enabled.
    // Card visibility is stored in ir_config_parameter ('account.dashboard.cards'),
    // so the dashboard is adjustable and the choice is shared/persisted. (docs/087)
    drogon::app().registerHandler(
        "/web/account/dashboard",
        [db, checkAuth, devMode](const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            if (!checkAuth(req)) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k401Unauthorized); cb(r); return;
            }
            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};

                // Persist a new card selection when one is posted via ?cards=a,b,c
                const std::string setCards = req->getParameter("cards");
                if (!setCards.empty()) {
                    txn.exec("INSERT INTO ir_config_parameter (key, value) VALUES ('account.dashboard.cards', $1) "
                             "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value",
                             pqxx::params{setCards});
                }
                std::string enabled = "invoices,bills,bank,cash,assets,budgets";
                {
                    auto r = txn.exec("SELECT value FROM ir_config_parameter WHERE key='account.dashboard.cards'");
                    if (!r.empty() && !r[0]["value"].is_null() && std::string(r[0]["value"].c_str()).size())
                        enabled = r[0]["value"].c_str();
                }

                auto one = [&](const char* sql) -> long long {
                    try { auto r = txn.exec(sql); return r.empty() || r[0][0].is_null() ? 0 : r[0][0].as<long long>(0); }
                    catch (...) { return 0; }
                };
                nlohmann::json cards = nlohmann::json::array();
                auto card = [&](const char* id, const char* title, const char* sub,
                                long long amount, long long count) {
                    cards.push_back({{"id", id}, {"title", title}, {"subtitle", sub},
                                     {"amount", fmtMicros_(amount)}, {"count", count}});
                };
                card("invoices", "Customer Invoices", "Unpaid",
                     one("SELECT COALESCE(SUM(amount_residual),0) FROM account_move "
                         "WHERE move_type='out_invoice' AND state='posted' AND amount_residual>0"),
                     one("SELECT COUNT(*) FROM account_move "
                         "WHERE move_type='out_invoice' AND state='posted' AND amount_residual>0"));
                card("bills", "Vendor Bills", "To pay",
                     one("SELECT COALESCE(SUM(amount_residual),0) FROM account_move "
                         "WHERE move_type='in_invoice' AND state='posted' AND amount_residual>0"),
                     one("SELECT COUNT(*) FROM account_move "
                         "WHERE move_type='in_invoice' AND state='posted' AND amount_residual>0"));
                card("bank", "Bank", "Balance",
                     one("SELECT COALESCE(SUM(l.debit-l.credit),0) FROM account_move_line l "
                         "JOIN account_move m ON m.id=l.move_id AND m.state='posted' "
                         "JOIN account_account a ON a.id=l.account_id "
                         "WHERE a.account_type='asset_cash'"),
                     one("SELECT COUNT(*) FROM account_bank_account WHERE active"));
                card("cash", "Cash", "Balance",
                     one("SELECT COALESCE(SUM(l.debit-l.credit),0) FROM account_move_line l "
                         "JOIN account_move m ON m.id=l.move_id AND m.state='posted' "
                         "JOIN account_account a ON a.id=l.account_id "
                         "JOIN account_journal j ON j.id=l.journal_id AND j.type='cash'"),
                     one("SELECT COUNT(*) FROM account_journal WHERE type='cash'"));
                card("assets", "Assets", "Book value",
                     one("SELECT COALESCE(SUM(value_residual),0) FROM account_asset WHERE state='open'"),
                     one("SELECT COUNT(*) FROM account_asset WHERE state='open'"));
                card("budgets", "Budgets", "Planned",
                     one("SELECT COALESCE(SUM(planned_amount),0) FROM account_budget_line"),
                     one("SELECT COUNT(*) FROM account_budget"));
                txn.commit();

                nlohmann::json out;
                out["enabled"] = enabled;
                out["cards"]   = cards;
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                resp->setBody(out.dump());
                cb(resp);
            } catch (const PoolExhaustedException& ex) {
                cb(htmlError(503, "The server is temporarily overloaded. Please retry."));
            } catch (const std::exception& ex) {
                LOG_ERROR << "[account/dashboard] " << ex.what();
                cb(htmlError(500, devMode ? ex.what() : "An internal error occurred"));
            }
        },
        {drogon::Get});

    // Financial statement reports — JSON for the on-screen UI. (docs/081)
    // GET /web/account/report?report=<type>&date_from=YYYY-MM-DD&date_to=YYYY-MM-DD
    drogon::app().registerHandler(
        "/web/account/report",
        [db, checkAuth, devMode](const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            if (!checkAuth(req)) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k401Unauthorized);
                cb(r); return;
            }
            const std::string report   = req->getParameter("report").empty()
                                         ? "trial_balance" : req->getParameter("report");
            const std::string dateTo   = req->getParameter("date_to").empty()
                                         ? "2999-12-31" : req->getParameter("date_to");
            const std::string dateFrom = req->getParameter("date_from").empty()
                                         ? "1900-01-01" : req->getParameter("date_from");
            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                nlohmann::json out = financialReport_(txn, report, dateFrom, dateTo);
                txn.commit();
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                resp->setBody(out.dump());
                cb(resp);
            } catch (const PoolExhaustedException& ex) {
                cb(htmlError(503, "The server is temporarily overloaded. Please retry."));
            } catch (const std::exception& ex) {
                LOG_ERROR << "[account/report] " << ex.what();
                cb(htmlError(500, devMode ? ex.what() : "An internal error occurred"));
            }
        },
        {drogon::Get});

    // Financial statement reports — printable HTML (browser → PDF). Same params.
    drogon::app().registerHandler(
        "/web/account/report/print",
        [db, checkAuth, authRedirect, devMode](const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            if (!checkAuth(req)) { cb(authRedirect()); return; }
            const std::string report   = req->getParameter("report").empty()
                                         ? "trial_balance" : req->getParameter("report");
            const std::string dateTo   = req->getParameter("date_to").empty()
                                         ? "2999-12-31" : req->getParameter("date_to");
            const std::string dateFrom = req->getParameter("date_from").empty()
                                         ? "1900-01-01" : req->getParameter("date_from");
            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                nlohmann::json rep = financialReport_(txn, report, dateFrom, dateTo);
                std::string company;
                try {
                    auto c = txn.exec("SELECT value FROM ir_config_parameter WHERE key='report.company_name'");
                    if (!c.empty()) company = safeStr(c[0]["value"]);
                } catch (...) {}
                if (company.empty()) {
                    try {
                        auto c = txn.exec("SELECT name FROM res_company ORDER BY id LIMIT 1");
                        if (!c.empty()) company = safeStr(c[0]["name"]);
                    } catch (...) {}
                }
                txn.commit();
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setContentTypeCode(drogon::CT_TEXT_HTML);
                resp->setBody(financialReportHtml_(rep, company));
                cb(resp);
            } catch (const PoolExhaustedException& ex) {
                cb(htmlError(503, "The server is temporarily overloaded. Please retry."));
            } catch (const std::exception& ex) {
                LOG_ERROR << "[account/report/print] " << ex.what();
                cb(htmlError(500, devMode ? ex.what() : "An internal error occurred"));
            }
        },
        {drogon::Get});

    // HTML route
    drogon::app().registerHandler(
        "/report/html/{1}/{2}",
        [db, checkAuth, authRedirect, devMode](const drogon::HttpRequestPtr& req,
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
                bool proforma = (req->getParameter("proforma") == "1");
                std::string html = renderDoc_(txn, model, recordId, proforma);
                txn.commit();
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                resp->setContentTypeCode(drogon::CT_TEXT_HTML);
                resp->setBody(html);
                cb(resp);
            } catch (const PoolExhaustedException& ex) {
                LOG_ERROR << "[report] pool: " << ex.what();
                cb(htmlError(503, "The server is temporarily overloaded. Please retry."));
            } catch (const std::runtime_error& ex) {
                // SEC-28: record-not-found messages are safe; gate anyway for consistency
                cb(htmlError(404, devMode ? ex.what() : "Record not found"));
            } catch (const std::exception& ex) {
                // SEC-28: may contain SQL details — never expose in production
                LOG_ERROR << "[report/html] " << ex.what();
                cb(htmlError(500, devMode ? ex.what() : "An internal error occurred"));
            }
        },
        {drogon::Get}
    );

    // PDF route
    drogon::app().registerHandler(
        "/report/pdf/{1}/{2}",
        [db, checkAuth, authRedirect, devMode](const drogon::HttpRequestPtr& req,
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
                bool proforma = (req->getParameter("proforma") == "1");
                std::string html = renderDoc_(txn, model, recordId, proforma);

                // Read template layout settings for PDF generation
                double pdfMarginTop = 15, pdfMarginRight = 18, pdfMarginBottom = 18, pdfMarginLeft = 18;
                int    pdfFontSize  = 10;
                std::string pdfFontColor = "#333333";
                double pdfLineHeight = 1.5;
                std::string pdfPaperFormat = "A4";
                std::string pdfFooterText;
                bool        pdfFooterShowPageNum = true;
                std::string pdfFooterPageNumFmt  = "Page {p} of {t}";
                std::string pdfFooterTextSource  = "custom";
                std::string pdfFooterLineColor   = "#cccccc";
                double      pdfFooterLineWidth   = 0.5;
                try {
                    auto srows = txn.exec(
                        "SELECT COALESCE(margin_top,15)::float AS mt, COALESCE(margin_right,18)::float AS mr, "
                        "COALESCE(margin_bottom,18)::float AS mb, COALESCE(margin_left,18)::float AS ml, "
                        "COALESCE(font_size,10) AS fs, COALESCE(font_color,'#333333') AS fc, "
                        "COALESCE(line_height,1.5)::float AS lh, COALESCE(paper_format,'A4') AS pf, "
                        "COALESCE(footer_text,'') AS ft, "
                        "COALESCE(footer_show_page_num,true) AS fspn, "
                        "COALESCE(footer_page_num_fmt,'Page {p} of {t}') AS fpnf, "
                        "COALESCE(footer_text_source,'custom') AS fts, "
                        "COALESCE(footer_line_color,'#cccccc') AS flc, "
                        "COALESCE(footer_line_width,0.5)::float AS flw "
                        "FROM ir_report_template WHERE model=$1 AND active=true ORDER BY id LIMIT 1",
                        pqxx::params{model});
                    if (!srows.empty()) {
                        pdfMarginTop          = srows[0]["mt"].as<double>(15);
                        pdfMarginRight        = srows[0]["mr"].as<double>(18);
                        pdfMarginBottom       = srows[0]["mb"].as<double>(18);
                        pdfMarginLeft         = srows[0]["ml"].as<double>(18);
                        pdfFontSize           = srows[0]["fs"].as<int>(10);
                        pdfFontColor          = safeStr(srows[0]["fc"]);
                        pdfLineHeight         = srows[0]["lh"].as<double>(1.5);
                        pdfPaperFormat        = safeStr(srows[0]["pf"]);
                        pdfFooterText         = safeStr(srows[0]["ft"]);
                        pdfFooterShowPageNum  = srows[0]["fspn"].is_null() ? true : srows[0]["fspn"].as<bool>();
                        pdfFooterPageNumFmt   = safeStr(srows[0]["fpnf"]);
                        pdfFooterTextSource   = safeStr(srows[0]["fts"]);
                        pdfFooterLineColor    = safeStr(srows[0]["flc"]);
                        pdfFooterLineWidth    = srows[0]["flw"].as<double>(0.5);
                    }
                } catch (...) {}
                txn.commit();

                // S-39: the temp path must contain no request-derived data.
                // It previously interpolated `model` and `idStr` straight from
                // the URL into a string that reached std::system(); std::stoi()
                // accepting "12$(cmd)" meant the id check did not constrain it.
                // mkdtemp() gives an unpredictable 0700 directory (also closing
                // the symlink-attack window on the old deterministic paths) and
                // the file names below are fixed literals.
                infrastructure::SecureTempDir tmpDir;
                const std::string tmpHtml   = tmpDir.file("doc.html");
                const std::string tmpFooter = tmpDir.file("footer.html");
                const std::string tmpPdf    = tmpDir.file("out.pdf");

                // --- Determine footer text based on source setting ---
                std::string footerContent;
  if(pdfFooterTextSource == "website") {
                    try {
                        auto wrows = txn.exec(
                            "SELECT value FROM ir_config_parameter WHERE key='report.website' LIMIT 1");
                        if (!wrows.empty()) footerContent = safeStr(wrows[0]["value"]);
                    } catch (...) {}
                } else if (pdfFooterTextSource == "custom") {
                    if (!pdfFooterText.empty()) {
                        footerContent = pdfFooterText;
                    } else {
                        // Fall back to page-footer div in rendered HTML
                        const std::string marker = "class=\"page-footer\"";
                        size_t pos = html.find(marker);
  if(pos != std::string::npos) {
                            size_t gt = html.find('>', pos);
                            size_t lt = html.find("</div>", gt);
  if(gt != std::string::npos && lt != std::string::npos)
                                footerContent = html.substr(gt + 1, lt - gt - 1);
                        }
                    }
                }
                // 'none' => footerContent stays empty

                bool hasFooter = !footerContent.empty() || pdfFooterShowPageNum || pdfFooterLineWidth > 0;

                // --- Inject PDF-specific CSS overrides before </head> ---
                // With wkhtmltopdf 0.12.6.1-2 (patched Qt), --footer-html works correctly.
                // Hide the in-body .page-footer since it is replaced by --footer-html.
                {
                    std::ostringstream cssOss;
                    cssOss << "<style>"
                           << "body{font-size:" << pdfFontSize << "pt!important;"
                           << "color:" << pdfFontColor << "!important;"
                           << "line-height:" << std::fixed << std::setprecision(2) << pdfLineHeight << "!important;}"
                           << ".page-footer{display:none!important;}"
                           << ".print-btn{display:none!important;}"
                           << ".dle-pg-prev{display:none!important;}"
                           << "</style>";
                    size_t hEnd = html.find("</head>");
                    if (hEnd != std::string::npos) html.insert(hEnd, cssOss.str());
                }

                // --- Write main HTML ---
                { std::ofstream f(tmpHtml); f << html; }

                // --- Write footer HTML using configured settings ---
                // NOTE: [page]/[toPage] are NOT valid in --footer-html files.
                // wkhtmltopdf passes page info as URL query params (?page=N&topage=M...)
                // to the footer HTML. JavaScript must read them and populate the DOM.
  if(hasFooter) {
                    // Build page-number HTML with named spans that JS will populate.
                    // {p} → <span id='pg'></span>, {t} → <span id='tot'></span>
                    std::string pageNumHtml = pdfFooterPageNumFmt;
                    {
                        size_t p;
                        while ((p = pageNumHtml.find("{p}")) != std::string::npos)
                            pageNumHtml.replace(p, 3, "<span id='pg'></span>");
                        while ((p = pageNumHtml.find("{t}")) != std::string::npos)
                            pageNumHtml.replace(p, 3, "<span id='tot'></span>");
                    }

                    std::ostringstream fw;
                    fw << std::fixed << std::setprecision(2);
                    fw << "<!DOCTYPE html><html><head>"
                       // JS reads query params wkhtmltopdf injects: ?page=N&topage=M
                       << "<script>"
                       << "function subst(){"
                       << "var v={};"
                       << "window.location.search.substring(1).split('&').forEach(function(s){"
                       << "var kv=s.split('=');if(kv[0])v[kv[0]]=decodeURIComponent(kv[1]||'');});"
                       << "var pg=document.getElementById('pg');"
                       << "var tot=document.getElementById('tot');"
                       << "if(pg)pg.textContent=v['page']||'';"
                       << "if(tot)tot.textContent=v['topage']||'';"
                       << "}"
                       << "</script>"
                       << "<style>"
                       << "body{margin:0;padding-top:4px;"
                       << "font-family:Arial,Helvetica,sans-serif;font-size:9pt;color:#333333;"
                       << "overflow:hidden;}";
  if(pdfFooterLineWidth > 0) {
                        fw << "body{border-top:" << pdfFooterLineWidth << "pt solid " << pdfFooterLineColor << ";}";
                    }
                    fw << ".footer-text{float:left;}"
                       << ".page-num{float:right;}"
                       << "</style></head>"
                       << "<body onload='subst()'>";
                    if (!footerContent.empty())
                        fw << "<span class=\"footer-text\">" << footerContent << "</span>";
  if(pdfFooterShowPageNum)
                        fw << "<span class=\"page-num\">" << pageNumHtml << "</span>";
                    fw << "</body></html>";

                    std::ofstream ff(tmpFooter);
                    ff << fw.str();
                }

                // --- Run wkhtmltopdf ---
                // --footer-html requires wkhtmltopdf built with patched Qt (0.12.6.1-2.jammy).
                // --margin-bottom provides the safe zone for the footer strip.
                auto mmStr = [](double v) {
                    std::ostringstream s;
                    s << std::fixed << std::setprecision(1) << v << "mm";
                    return s.str();
                };
                double effectiveMarginBottom = hasFooter
                    ? std::max(pdfMarginBottom, 20.0)
                    : pdfMarginBottom;
                // SEC-29: validate paper format against allowlist before shell interpolation
                static const std::set<std::string> kAllowedFormats = {
                    "A3", "A4", "A5", "Letter", "Legal"
                };
                const std::string safePaperFormat =
                    kAllowedFormats.count(pdfPaperFormat) ? pdfPaperFormat : "A4";

                // SEC-31: argv array via execvp() — no shell, so no quoting and
                // no substring of any argument can be reinterpreted as syntax.
                // S-44: --enable-local-file-access removed; it let any HTML
                // reaching the renderer pull local files (e.g. config/system.cfg,
                // which holds the DB password) into the output PDF.
                std::vector<std::string> argv = {
                    "wkhtmltopdf", "--quiet",
                    "--page-size",     safePaperFormat,
                    "--margin-top",    mmStr(pdfMarginTop),
                    "--margin-right",  mmStr(pdfMarginRight),
                    "--margin-bottom", mmStr(effectiveMarginBottom),
                    "--margin-left",   mmStr(pdfMarginLeft),
                };
                if (hasFooter) {
                    argv.push_back("--footer-html");
                    argv.push_back(tmpFooter);
                    argv.push_back("--footer-spacing");
                    argv.push_back("5");
                }
                argv.push_back(tmpHtml);
                argv.push_back(tmpPdf);

                int exitCode = -1;
                const auto pr = infrastructure::runProcess(argv, &exitCode, 30000);
                if (pr != infrastructure::ProcessResult::Ok) {
                    LOG_ERROR << "[report/pdf] wkhtmltopdf failed for model=" << model
                              << " id=" << recordId << " exit=" << exitCode;
                    cb(htmlError(503, "PDF generation failed. Ensure wkhtmltopdf is installed on the server."));
                    return;   // SecureTempDir cleans up on scope exit
                }

                // --- Read and return PDF ---
                std::ifstream pdfFile(tmpPdf, std::ios::binary);
                std::string pdfData((std::istreambuf_iterator<char>(pdfFile)),
                                    std::istreambuf_iterator<char>());
                pdfFile.close();

                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                resp->setContentTypeString("application/pdf");
                // S-39 (same class): idStr is raw request data — a CR/LF or a
                // quote in it would break out of this header. Use the parsed
                // integer and a charset-restricted model name instead.
                std::string safeModel;
                for (char c : model)
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_')
                        safeModel += c;
                resp->addHeader("Content-Disposition",
                    "inline; filename=\"" + safeModel + "_" +
                    std::to_string(recordId) + ".pdf\"");
                resp->setBody(pdfData);
                cb(resp);

            } catch (const PoolExhaustedException& ex) {
                LOG_ERROR << "[report] pool: " << ex.what();
                cb(htmlError(503, "The server is temporarily overloaded. Please retry."));
            } catch (const std::runtime_error& ex) {
                // SEC-28: record-not-found messages are safe; gate anyway for consistency
                cb(htmlError(404, devMode ? ex.what() : "Record not found"));
            } catch (const std::exception& ex) {
                // SEC-28: may contain SQL details — never expose in production
                LOG_ERROR << "[report/pdf] " << ex.what();
                cb(htmlError(500, devMode ? ex.what() : "An internal error occurred"));
            }
        },
        {drogon::Get}
    );

    // Preview route — renders template with dummy data (no real record needed)
    // NOTE: uses /report/preview/ (not /report/html/) to avoid collision with /report/html/{model}/{id}
    drogon::app().registerHandler(
        "/report/preview/{1}",
        [db, devMode](const drogon::HttpRequestPtr& req,
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
  if(model == "account.move") {
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
  if(model == "stock.picking") {
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

            } catch (const PoolExhaustedException& ex) {
                LOG_ERROR << "[report] pool: " << ex.what();
                cb(htmlError(503, "The server is temporarily overloaded. Please retry."));
            } catch (const std::exception& ex) {
                // SEC-28: may contain SQL details — never expose in production
                LOG_ERROR << "[report/preview] " << ex.what();
                cb(htmlError(500, devMode ? ex.what() : "An internal error occurred"));
            }
        },
        {drogon::Get}
    );
}

void ReportModule::initialize() {
    ensureSchema_();
    seedTemplates_();
    seedConfigParams_();
    seedMenuEntries_();
    seedConfigParams_extra_();
}


void ReportModule::ensureSchema_() {
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
    txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS footer_text          TEXT         NOT NULL DEFAULT ''");
    txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS footer_show_page_num BOOLEAN      NOT NULL DEFAULT true");
    txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS footer_page_num_fmt  TEXT         NOT NULL DEFAULT 'Page {p} of {t}'");
    txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS footer_text_source   TEXT         NOT NULL DEFAULT 'custom'");
    txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS footer_line_color    TEXT         NOT NULL DEFAULT '#cccccc'");
    txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS footer_line_width    NUMERIC(4,2) NOT NULL DEFAULT 0.5");
    txn.commit();
}

void ReportModule::seedTemplates_() {
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

    // Upgrade an already-seeded Sales Order template so its line rows carry
    // their display_type class (row-line_section / row-line_note), enabling
    // sections/notes to render. Idempotent — matches only the plain product
    // line row and skips templates that already carry the class (or were
    // customised to include it).
    txn.exec(R"(
        UPDATE ir_report_template
        SET template_html = regexp_replace(template_html,
                '<tr>(\s*<td>\{\{product_name\}\})',
                '<tr class="row-{{line_type}}">\1')
        WHERE model = 'sale.order'
          AND template_html LIKE '%{{product_name}}%'
          AND template_html NOT LIKE '%row-{{line_type}}%'
    )");

    txn.exec("SELECT setval('ir_report_template_id_seq', 4, true)");
    txn.commit();
}

void ReportModule::seedConfigParams_() {
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

void ReportModule::seedMenuEntries_() {
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

    // Action id=95: Groups (res.groups).
    // NOTE: id 36 was used here, but MrpModule owns 36 ('Work Centers') and this
    // insert was ON CONFLICT DO NOTHING — so the Groups menu opened Work Centers
    // and the groups screen was unreachable. Own ids + DO UPDATE so existing
    // databases self-heal. (Same class of bug as docs/076.)
    txn.exec(
        "INSERT INTO ir_act_window (id, name, res_model, view_mode, path) VALUES "
        "(95, 'Groups', 'res.groups', 'list', 'groups') "
        "ON CONFLICT (id) DO UPDATE SET name='Groups', res_model='res.groups', "
        "view_mode='list', path='groups'");

    // Menu id=105: Groups under Technical (id=101), after Document Templates (seq=10)
    // (id=104 is owned by MrpModule for Bills of Materials under Inventory)
    txn.exec(
        "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
        "(105, 'Groups', 101, 20, 95) "
        "ON CONFLICT (id) DO UPDATE SET name='Groups', parent_id=101, sequence=20, action_id=95");

    // Action id=96: ERP Settings.
    // id 31 belonged to StockModule ('Putaway Rules'), which seeds it with
    // ON CONFLICT DO UPDATE — so the ERP Settings menu opened Putaway Rules.
    txn.exec(
        "INSERT INTO ir_act_window (id, name, res_model, view_mode) VALUES "
        "(96, 'ERP Settings', 'ir.erp.settings', 'list,form') "
        "ON CONFLICT (id) DO UPDATE SET name='ERP Settings', "
        "res_model='ir.erp.settings', view_mode='list,form'");

    // Menu id=103: ERP Settings directly under unified Settings (id=30)
    txn.exec(
        "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
        "(103, 'ERP Settings', 30, 25, 96) "
        "ON CONFLICT (id) DO UPDATE SET parent_id=30, sequence=25, action_id=96");

    // Multi-company (docs/072): control-plane admin under Settings (id=30).
    // Renders the CompanyAdmin custom view; admin-gated server-side.
    txn.exec(
        "INSERT INTO ir_act_window (id, name, res_model, view_mode, path) VALUES "
        "(71, 'Companies & Access', 'company.admin', 'list', 'company-admin') "
        "ON CONFLICT (id) DO UPDATE SET name='Companies & Access', "
        "res_model='company.admin', view_mode='list', path='company-admin'");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
    txn.exec(
        "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
        "(131, 'Companies & Access', 30, 40, 71) "
        "ON CONFLICT (id) DO UPDATE SET parent_id=30, sequence=40, action_id=71");

    // Database & backups (docs/075) under Settings (id=30). Renders the DbBackups
    // custom view; every endpoint is admin-gated + per-tenant server-side.
    txn.exec(
        "INSERT INTO ir_act_window (id, name, res_model, view_mode, path) VALUES "
        "(72, 'Database & Backups', 'db.backups', 'list', 'db-backups') "
        "ON CONFLICT (id) DO UPDATE SET name='Database & Backups', "
        "res_model='db.backups', view_mode='list', path='db-backups'");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
    txn.exec(
        "INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES "
        "(132, 'Database & Backups', 30, 45, 72) "
        "ON CONFLICT (id) DO UPDATE SET parent_id=30, sequence=45, action_id=72");

    txn.commit();
}

void ReportModule::seedConfigParams_extra_() {
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

} // namespace odoo::modules::report

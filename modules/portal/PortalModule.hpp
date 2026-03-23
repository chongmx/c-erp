#pragma once
// =============================================================
// modules/portal/PortalModule.hpp
//
// Customer-facing portal for the storage rental ERP.
//
// Features:
//   - Separate portal session management (cookie: portal_sid, 8h TTL)
//   - Per-IP login rate limiting (10 attempts / 5-minute window)
//   - PBKDF2-SHA512 password hashing (same format as AuthService)
//   - PortalPartnerViewModel ("portal.partner") for admin use via JSON-RPC
//   - 10 HTTP routes under /portal/...
//   - DB schema migrations (idempotent)
//   - File upload directory: data/payment_proofs/
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include "DbConnection.hpp"
#include "BaseViewModel.hpp"
#include "interfaces/IViewModel.hpp"
#include <drogon/drogon.h>
#include <drogon/MultiPart.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <pqxx/pqxx>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <map>
#include <unordered_map>
#include <vector>

namespace odoo::modules::portal {

using namespace odoo::core;
using namespace odoo::infrastructure;

// ================================================================
// Portal password utilities (PBKDF2-SHA512, same format as AuthService)
// ================================================================

/**
 * @brief Encode raw bytes to standard base64 (no newlines).
 * Mirrors AuthService::base64Encode_ exactly.
 */
static std::string portalBase64Encode(const unsigned char* data, std::size_t len) {
    BIO* b64  = BIO_new(BIO_f_base64());
    BIO* bmem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, static_cast<int>(len));
    BIO_flush(b64);

    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    return result;
}

/**
 * @brief Decode standard base64 (no newlines) to raw bytes.
 * Mirrors AuthService::base64Decode_ exactly.
 */
static std::vector<char> portalBase64Decode(const std::string& encoded) {
    BIO* b64  = BIO_new(BIO_f_base64());
    BIO* bmem = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
    bmem = BIO_push(b64, bmem);
    BIO_set_flags(bmem, BIO_FLAGS_BASE64_NO_NL);

    std::vector<char> buf(encoded.size());
    const int len = BIO_read(bmem, buf.data(), static_cast<int>(buf.size()));
    BIO_free_all(bmem);

    if (len < 0) return {};
    buf.resize(static_cast<std::size_t>(len));
    return buf;
}

/**
 * @brief Hash a plaintext password using PBKDF2-SHA512.
 *
 * Produces a string in Odoo's passlib format:
 *   $pbkdf2-sha512$<rounds>$<base64-salt>$<base64-hash>
 *
 * @param plaintext  Plaintext password.
 * @param rounds     Iteration count (default: 600000).
 * @returns          Hash string ready for storage.
 */
static std::string portalHashPassword(const std::string& plaintext,
                                      int rounds = 600000)
{
    unsigned char salt[16];
    if (RAND_bytes(salt, sizeof(salt)) != 1)
        throw std::runtime_error("portalHashPassword: RAND_bytes failed");

    unsigned char hash[64];
    if (PKCS5_PBKDF2_HMAC(
            plaintext.c_str(), static_cast<int>(plaintext.size()),
            salt, sizeof(salt),
            rounds,
            EVP_sha512(),
            sizeof(hash), hash) != 1)
    {
        throw std::runtime_error("portalHashPassword: PBKDF2 derivation failed");
    }

    return "$pbkdf2-sha512$"
         + std::to_string(rounds) + "$"
         + portalBase64Encode(salt, sizeof(salt)) + "$"
         + portalBase64Encode(hash, sizeof(hash));
}

/**
 * @brief Verify a plaintext password against a stored PBKDF2-SHA512 hash.
 *
 * Format: $pbkdf2-sha512$<rounds>$<base64-salt>$<base64-hash>
 *
 * Uses constant-time comparison to prevent timing attacks.
 */
static bool portalVerifyPassword(const std::string& plaintext,
                                  const std::string& stored)
{
    const std::string prefix = "$pbkdf2-sha512$";
    if (stored.size() < prefix.size()) return false;
    if (stored.substr(0, prefix.size()) != prefix) return false;

    const auto body = stored.substr(prefix.size());
    const auto p1   = body.find('$');
    if (p1 == std::string::npos) return false;
    const auto p2   = body.find('$', p1 + 1);
    if (p2 == std::string::npos) return false;

    int rounds = 0;
    try { rounds = std::stoi(body.substr(0, p1)); } catch (...) { return false; }
    const auto saltB64 = body.substr(p1 + 1, p2 - p1 - 1);
    const auto hashB64 = body.substr(p2 + 1);

    const auto salt     = portalBase64Decode(saltB64);
    const auto expected = portalBase64Decode(hashB64);

    if (salt.empty() || expected.empty()) return false;

    std::vector<unsigned char> derived(expected.size());
    if (PKCS5_PBKDF2_HMAC(
            plaintext.c_str(), static_cast<int>(plaintext.size()),
            reinterpret_cast<const unsigned char*>(salt.data()),
            static_cast<int>(salt.size()),
            rounds,
            EVP_sha512(),
            static_cast<int>(derived.size()),
            derived.data()) != 1)
    {
        return false;
    }

    // Constant-time comparison
    if (derived.size() != expected.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < derived.size(); ++i)
        diff |= derived[i] ^ static_cast<unsigned char>(expected[i]);
    return diff == 0;
}


// ================================================================
// Portal document rendering helpers (mirrors ReportModule utilities)
// ================================================================
static std::string portalFmtMoney(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << std::abs(v);
    std::string s = oss.str();
    size_t dot = s.find('.');
    size_t start = dot == std::string::npos ? s.size() : dot;
    int ins = (int)start - 3;
    while (ins > 0) { s.insert(ins, ","); ins -= 3; }
    if (v < 0) s = "-" + s;
    return s;
}
static std::string portalFmtMoneyF(const pqxx::field& f) {
    if (f.is_null()) return "0.00";
    try { return portalFmtMoney(f.as<double>()); } catch (...) { return f.c_str(); }
}
static std::string portalFmtPrec(double v, int prec) {
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
static std::string portalFmtPrecF(const pqxx::field& f, int prec) {
    if (f.is_null()) return "0." + std::string(std::max(0, prec), '0');
    try { return portalFmtPrec(f.as<double>(), prec); } catch (...) { return f.c_str(); }
}
static std::string portalSafeStr(const pqxx::field& f) {
    return f.is_null() ? "" : f.c_str();
}
static std::string portalYmdToDisplay(const std::string& ymd) {
    if (ymd.size() >= 10 && ymd[4] == '-' && ymd[7] == '-')
        return ymd.substr(8, 2) + "/" + ymd.substr(5, 2) + "/" + ymd.substr(0, 4);
    return ymd;
}
static std::string portalReplaceAll(std::string str, const std::string& from, const std::string& to) {
    if (from.empty()) return str;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
    return str;
}
static std::string portalRenderTemplate(
    std::string tmpl,
    const std::map<std::string, std::string>& vars,
    const std::vector<std::map<std::string, std::string>>& lines = {})
{
    const std::string eachStart = "{{#each lines}}";
    const std::string eachEnd   = "{{/each}}";
    size_t sPos = tmpl.find(eachStart);
    size_t ePos = tmpl.find(eachEnd);
    if (sPos != std::string::npos && ePos != std::string::npos) {
        std::string before  = tmpl.substr(0, sPos);
        std::string loopTpl = tmpl.substr(sPos + eachStart.size(), ePos - sPos - eachStart.size());
        std::string after   = tmpl.substr(ePos + eachEnd.size());
        std::string expanded;
        for (const auto& line : lines) {
            std::string row = loopTpl;
            for (const auto& [k, v] : line)
                row = portalReplaceAll(row, "{{" + k + "}}", v);
            expanded += row;
        }
        tmpl = before + expanded + after;
    }
    for (const auto& [k, v] : vars)
        tmpl = portalReplaceAll(tmpl, "{{" + k + "}}", v);
    {
        std::string out; out.reserve(tmpl.size());
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

// Render a document using ir_report_template, with portal ownership check.
// model: "account.move", "sale.order", "stock.picking"
// Returns rendered HTML (empty string if not found or partner mismatch).
static std::string portalRenderDoc(
    const std::string& model, int recordId, int partnerId, pqxx::work& txn)
{
    auto tplRows = txn.exec(
        "SELECT template_html, "
        "COALESCE(decimal_qty, 2) AS decimal_qty, "
        "COALESCE(decimal_price, 2) AS decimal_price, "
        "COALESCE(decimal_subtotal, 2) AS decimal_subtotal "
        "FROM ir_report_template "
        "WHERE model=$1 AND active=true ORDER BY id LIMIT 1",
        pqxx::params{model});
    if (tplRows.empty()) return "";
    std::string tplHtml = portalSafeStr(tplRows[0]["template_html"]);
    const int qtyPrec = tplRows[0]["decimal_qty"].as<int>(2);
    const int prcPrec = tplRows[0]["decimal_price"].as<int>(2);
    const int subPrec = tplRows[0]["decimal_subtotal"].as<int>(2);

    std::map<std::string, std::string> vars;
    std::vector<std::map<std::string, std::string>> lines;
    int companyId = 1;

    auto loadCfg = [&](const std::string& key, const std::string& def = "") -> std::string {
        try {
            auto r = txn.exec("SELECT value FROM ir_config_parameter WHERE key=$1", pqxx::params{key});
            if (!r.empty() && !r[0]["value"].is_null()) return r[0]["value"].c_str();
        } catch (...) {}
        return def;
    };

    if (model == "account.move") {
        auto rows = txn.exec(
            "SELECT am.name, am.move_type, am.state, "
            "to_char(am.invoice_date, 'YYYY-MM-DD') AS invoice_date, "
            "to_char(am.due_date, 'YYYY-MM-DD') AS invoice_date_due, "
            "COALESCE(am.amount_untaxed::TEXT,'0') AS amount_untaxed, "
            "COALESCE(am.amount_tax::TEXT,'0') AS amount_tax, "
            "COALESCE(am.amount_total::TEXT,'0') AS amount_total, "
            "am.partner_id, am.company_id "
            "FROM account_move am "
            "WHERE am.id=$1 AND am.partner_id=$2",
            pqxx::params{recordId, partnerId});
        if (rows.empty()) return "";
        const auto& r = rows[0];
        companyId = r["company_id"].is_null() ? 1 : r["company_id"].as<int>();
        std::string moveType = portalSafeStr(r["move_type"]);
        vars["document_title"] = (moveType == "in_invoice") ? "Vendor Bill" :
                                  (moveType == "out_refund")  ? "Credit Note" :
                                  (moveType == "in_refund")   ? "Vendor Credit Note" : "Sales Invoice";
        vars["doc_number"]     = portalSafeStr(r["name"]);
        vars["doc_date"]       = portalYmdToDisplay(portalSafeStr(r["invoice_date"]));
        vars["doc_date_due"]   = portalYmdToDisplay(portalSafeStr(r["invoice_date_due"]));
        vars["amount_untaxed"] = portalFmtMoneyF(r["amount_untaxed"]);
        vars["amount_tax"]     = portalFmtMoneyF(r["amount_tax"]);
        vars["amount_total"]   = portalFmtMoneyF(r["amount_total"]);
        auto lrows = txn.exec(
            "SELECT COALESCE(aml.name,'') AS product_name, "
            "COALESCE(aml.quantity, 0) AS qty, "
            "COALESCE(aml.price_unit, 0) AS price_unit, "
            "COALESCE(NULLIF(aml.price_unit,0) * aml.quantity, aml.debit, 0) AS subtotal, "
            "COALESCE(NULLIF(aml.display_type,''),'product') AS line_type "
            "FROM account_move_line aml "
            "JOIN account_account aa ON aa.id = aml.account_id "
            "WHERE aml.move_id=$1 "
            "AND (aml.display_type IS NULL OR aml.display_type IN ('','product','line_section','line_note')) "
            "AND aa.account_type NOT IN ('liability_payable','asset_receivable') "
            "ORDER BY aml.id",
            pqxx::params{recordId});
        for (const auto& lr : lrows)
            lines.push_back({{"product_name",portalSafeStr(lr["product_name"])},
                              {"qty",        portalFmtPrecF(lr["qty"],        qtyPrec)},
                              {"price_unit", portalFmtPrecF(lr["price_unit"], prcPrec)},
                              {"subtotal",   portalFmtPrecF(lr["subtotal"],   subPrec)},
                              {"line_type",  portalSafeStr(lr["line_type"])},{"uom","Unit"}});

    } else if (model == "sale.order") {
        auto rows = txn.exec(
            "SELECT so.name, so.state, "
            "to_char(so.date_order AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS date_order, "
            "to_char(so.validity_date, 'YYYY-MM-DD') AS validity_date, "
            "COALESCE(so.amount_untaxed::TEXT,'0') AS amount_untaxed, "
            "COALESCE(so.amount_tax::TEXT,'0') AS amount_tax, "
            "COALESCE(so.amount_total::TEXT,'0') AS amount_total, "
            "so.partner_id, so.company_id FROM sale_order so "
            "WHERE so.id=$1 AND so.partner_id=$2",
            pqxx::params{recordId, partnerId});
        if (rows.empty()) return "";
        const auto& r = rows[0];
        companyId = r["company_id"].is_null() ? 1 : r["company_id"].as<int>();
        std::string soState = portalSafeStr(r["state"]);
        vars["document_title"] = (soState == "sale" || soState == "done") ? "Sales Order" : "Quotation";
        vars["doc_number"]     = portalSafeStr(r["name"]);
        vars["doc_date"]       = portalYmdToDisplay(portalSafeStr(r["date_order"]));
        vars["validity_date"]  = portalYmdToDisplay(portalSafeStr(r["validity_date"]));
        vars["amount_untaxed"] = portalFmtMoneyF(r["amount_untaxed"]);
        vars["amount_tax"]     = portalFmtMoneyF(r["amount_tax"]);
        vars["amount_total"]   = portalFmtMoneyF(r["amount_total"]);
        auto lrows = txn.exec(
            "SELECT COALESCE(sol.name, pp.name, '') AS product_name, "
            "sol.product_uom_qty AS qty, sol.price_unit, sol.price_subtotal AS subtotal, "
            "COALESCE(uu.name,'') AS uom "
            "FROM sale_order_line sol "
            "LEFT JOIN product_product pp ON pp.id = sol.product_id "
            "LEFT JOIN uom_uom uu ON uu.id = sol.product_uom_id "
            "WHERE sol.order_id=$1 ORDER BY sol.id",
            pqxx::params{recordId});
        for (const auto& lr : lrows)
            lines.push_back({{"product_name",portalSafeStr(lr["product_name"])},
                              {"qty",        portalFmtPrecF(lr["qty"],        qtyPrec)},
                              {"uom",        portalSafeStr(lr["uom"])},
                              {"price_unit", portalFmtPrecF(lr["price_unit"], prcPrec)},
                              {"subtotal",   portalFmtPrecF(lr["subtotal"],   subPrec)},
                              {"line_type",  "product"}});

    } else if (model == "stock.picking") {
        auto rows = txn.exec(
            "SELECT sp.name, sp.origin, sp.state, "
            "to_char(sp.scheduled_date AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS scheduled_date, "
            "sp.partner_id, sp.location_id, sp.location_dest_id, sp.company_id, "
            "COALESCE(spt.code,'') AS picking_type_code "
            "FROM stock_picking sp "
            "LEFT JOIN stock_picking_type spt ON spt.id = sp.picking_type_id "
            "WHERE sp.id=$1 AND sp.partner_id=$2",
            pqxx::params{recordId, partnerId});
        if (rows.empty()) return "";
        const auto& r = rows[0];
        companyId = r["company_id"].is_null() ? 1 : r["company_id"].as<int>();
        std::string code = portalSafeStr(r["picking_type_code"]);
        std::string srcLoc, dstLoc;
        if (!r["location_id"].is_null()) {
            auto lrow = txn.exec("SELECT complete_name FROM stock_location WHERE id=$1",
                pqxx::params{r["location_id"].as<int>()});
            if (!lrow.empty()) srcLoc = portalSafeStr(lrow[0]["complete_name"]);
        }
        if (!r["location_dest_id"].is_null()) {
            auto lrow = txn.exec("SELECT complete_name FROM stock_location WHERE id=$1",
                pqxx::params{r["location_dest_id"].as<int>()});
            if (!lrow.empty()) dstLoc = portalSafeStr(lrow[0]["complete_name"]);
        }
        vars["document_title"]  = (code == "incoming") ? "Receipt" :
                                   (code == "outgoing") ? "Delivery Order" : "Transfer";
        vars["doc_number"]      = portalSafeStr(r["name"]);
        vars["doc_date"]        = portalYmdToDisplay(portalSafeStr(r["scheduled_date"]));
        vars["origin"]          = portalSafeStr(r["origin"]);
        vars["source_location"] = srcLoc;
        vars["dest_location"]   = dstLoc;
        auto lrows = txn.exec(
            "SELECT COALESCE(pp.name, sm.name, '') AS product_name, "
            "sm.product_uom_qty AS demand, sm.quantity AS done, "
            "COALESCE(uu.name,'') AS uom "
            "FROM stock_move sm "
            "LEFT JOIN product_product pp ON pp.id = sm.product_id "
            "LEFT JOIN uom_uom uu ON uu.id = sm.product_uom_id "
            "WHERE sm.picking_id=$1 ORDER BY sm.id",
            pqxx::params{recordId});
        for (const auto& lr : lrows)
            lines.push_back({{"product_name",portalSafeStr(lr["product_name"])},
                              {"demand", portalFmtPrecF(lr["demand"], qtyPrec)},
                              {"done",   portalFmtPrecF(lr["done"],   qtyPrec)},
                              {"uom",    portalSafeStr(lr["uom"])}});
    } else {
        return "";
    }

    // Company info
    auto crows = txn.exec(
        "SELECT rc.name, COALESCE(rc.website,'') AS website, "
        "COALESCE(rp.street,'') AS street, COALESCE(rp.city,'') AS city "
        "FROM res_company rc LEFT JOIN res_partner rp ON rp.id = rc.partner_id WHERE rc.id=$1",
        pqxx::params{companyId});
    if (!crows.empty()) {
        vars["company_name"]         = portalSafeStr(crows[0]["name"]);
        vars["company_website"]      = portalSafeStr(crows[0]["website"]);
        vars["company_addr1"]        = loadCfg("report.addr1", portalSafeStr(crows[0]["street"]));
        vars["company_city_country"] = loadCfg("report.city_country", portalSafeStr(crows[0]["city"]));
    }
    vars["company_reg"]          = loadCfg("report.reg_number");
    vars["company_addr2"]        = loadCfg("report.addr2");
    vars["company_addr3"]        = loadCfg("report.addr3");
    vars["currency_code"]        = loadCfg("report.currency_code", "MYR");
    vars["payment_term_days"]    = loadCfg("report.payment_term_days", "30");
    vars["bank_account_name"]    = loadCfg("report.bank.account_name");
    vars["bank_account_no"]      = loadCfg("report.bank.account_no");
    vars["bank_name"]            = loadCfg("report.bank.bank_name");
    vars["bank_address"]         = loadCfg("report.bank.bank_address");
    vars["bank_swift"]           = loadCfg("report.bank.swift_code");

    // Partner info
    auto prows = txn.exec(
        "SELECT COALESCE(name,'') AS name, COALESCE(street,'') AS street, "
        "COALESCE(city,'') AS city, COALESCE(phone,'') AS phone, "
        "COALESCE(company_name,'') AS company_name, COALESCE(is_company,false) AS is_company "
        "FROM res_partner WHERE id=$1", pqxx::params{partnerId});
    if (!prows.empty()) {
        std::string pName    = portalSafeStr(prows[0]["name"]);
        std::string compName = portalSafeStr(prows[0]["company_name"]);
        bool isCompany = prows[0]["is_company"].as<bool>(false);
        vars["partner_street"] = portalSafeStr(prows[0]["street"]);
        vars["partner_city"]   = portalSafeStr(prows[0]["city"]);
        vars["partner_phone"]  = portalSafeStr(prows[0]["phone"]);
        if (isCompany) { vars["partner_name"] = pName; vars["attn_name"] = pName; }
        else { vars["partner_name"] = compName.empty() ? pName : compName; vars["attn_name"] = pName; }
    }

    return portalRenderTemplate(tplHtml, vars, lines);
}

// ================================================================
// PortalSession
// ================================================================

/**
 * @brief One authenticated portal (customer) session.
 */
struct PortalSession {
    std::string sessionId;
    int         partnerId = 0;
    std::string name;
    std::string email;

    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    TimePoint accessedAt = Clock::now();
};


// ================================================================
// PortalSessionManager
// ================================================================

/**
 * @brief Thread-safe in-memory portal session store.
 *
 * Separate from the internal SessionManager; uses cookie name "portal_sid"
 * and an 8-hour TTL.
 * Session IDs are 128-bit cryptographically random hex tokens.
 */
class PortalSessionManager {
public:
    static constexpr const char* kCookieName = "portal_sid";

    using Duration = std::chrono::seconds;

    explicit PortalSessionManager(Duration ttl = std::chrono::hours{8})
        : ttl_(ttl) {}

    /**
     * @brief Create a new portal session and return its session ID.
     */
    std::string create(int partnerId, const std::string& name, const std::string& email) {
        PortalSession s;
        s.sessionId  = generateId_();
        s.partnerId  = partnerId;
        s.name       = name;
        s.email      = email;
        s.accessedAt = PortalSession::Clock::now();

        std::scoped_lock lock{mutex_};
        const std::string id = s.sessionId;
        store_[id] = std::move(s);
        return id;
    }

    /**
     * @brief Look up a portal session by ID.
     *
     * Returns nullopt if the ID is unknown or the session has expired.
     * Touching the session resets its TTL.
     */
    std::optional<PortalSession> get(const std::string& sid) {
        std::scoped_lock lock{mutex_};
        auto it = store_.find(sid);
        if (it == store_.end()) return std::nullopt;

        if (isExpired_(it->second)) {
            store_.erase(it);
            return std::nullopt;
        }

        it->second.accessedAt = PortalSession::Clock::now();
        return it->second;
    }

    /**
     * @brief Destroy a portal session (logout).
     */
    void remove(const std::string& sid) {
        std::scoped_lock lock{mutex_};
        store_.erase(sid);
    }

private:
    bool isExpired_(const PortalSession& s) const {
        return (PortalSession::Clock::now() - s.accessedAt) > ttl_;
    }

    static std::string generateId_() {
        unsigned char buf[16];
        if (RAND_bytes(buf, sizeof(buf)) != 1)
            throw std::runtime_error("PortalSessionManager: RAND_bytes failed");
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (unsigned char b : buf)
            ss << std::setw(2) << static_cast<unsigned>(b);
        return ss.str();
    }

    Duration                                     ttl_;
    mutable std::mutex                           mutex_;
    std::unordered_map<std::string, PortalSession> store_;
};


// ================================================================
// PortalLoginRateLimiter
// ================================================================

/**
 * @brief Per-IP sliding-window rate limiter for the portal login endpoint.
 *
 * Allows up to kMaxAttempts failed attempts within kWindowSeconds.
 * A successful login resets the counter for that IP.
 * Thread-safe via a single mutex; the map is pruned on each check to
 * prevent unbounded growth from unique IPs.
 */
class PortalLoginRateLimiter {
public:
    static constexpr int kMaxAttempts   = 10;
    static constexpr int kWindowSeconds = 300; // 5-minute window

    /** Returns true if the IP is allowed to attempt login. */
    bool allow(const std::string& ip) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lk(mutex_);
        prune_(now);
        auto& entry = table_[ip];
        if (entry.count >= kMaxAttempts &&
            (now - entry.windowStart) < std::chrono::seconds(kWindowSeconds))
            return false;
        if ((now - entry.windowStart) >= std::chrono::seconds(kWindowSeconds)) {
            entry.windowStart = now;
            entry.count = 0;
        }
        return true;
    }

    /** Call on every failed attempt. */
    void recordFailure(const std::string& ip) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lk(mutex_);
        auto& entry = table_[ip];
        if ((now - entry.windowStart) >= std::chrono::seconds(kWindowSeconds)) {
            entry.windowStart = now;
            entry.count = 0;
        }
        ++entry.count;
    }

    /** Call on successful login — resets counter for this IP. */
    void recordSuccess(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mutex_);
        table_.erase(ip);
    }

private:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        Clock::time_point windowStart = Clock::now();
        int               count       = 0;
    };

    void prune_(Clock::time_point now) {
        const auto cutoff = std::chrono::seconds(kWindowSeconds * 2);
        for (auto it = table_.begin(); it != table_.end(); ) {
            if ((now - it->second.windowStart) > cutoff)
                it = table_.erase(it);
            else
                ++it;
        }
    }

    std::mutex                              mutex_;
    std::unordered_map<std::string, Entry>  table_;
};


// ================================================================
// PortalPartnerViewModel  ("portal.partner")
// ================================================================
/**
 * @brief Admin-facing ViewModel for managing portal partner access.
 *
 * Registered as "portal.partner" — consumed via the existing internal
 * JSON-RPC system (already protected by internal session authentication).
 *
 * Methods:
 *   portal_reset_password  — set portal_password_hash = hash("Welcome1"),
 *                            portal_active = TRUE for given partner ids
 *   portal_set_active      — set portal_active for given partner ids
 *   portal_set_rental_price — INSERT/UPDATE partner_rental_price
 *   portal_get_rental_prices — list rental prices for a partner
 *   fields_get             — field definitions
 */
class PortalPartnerViewModel : public core::BaseViewModel {
public:
    explicit PortalPartnerViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : db_(std::move(db))
    {
        REGISTER_METHOD("search_read",             handleSearchRead)
        REGISTER_METHOD("web_search_read",         handleSearchRead)
        REGISTER_METHOD("write",                   handleWrite)
        REGISTER_METHOD("set_portal_password",     handleSetPortalPassword)
        REGISTER_METHOD("get_companies",           handleGetCompanies)
        REGISTER_METHOD("portal_reset_password",   handleResetPassword)
        REGISTER_METHOD("portal_set_active",       handleSetActive)
        REGISTER_METHOD("portal_set_rental_price", handleSetRentalPrice)
        REGISTER_METHOD("portal_get_rental_prices",handleGetRentalPrices)
        REGISTER_METHOD("fields_get",              handleFieldsGet)
    }

    std::string modelName() const override { return "portal.partner"; }

private:
    std::shared_ptr<infrastructure::DbConnection> db_;

    // ----------------------------------------------------------
    // search_read — list partners with portal fields
    // Supports domain filter (e.g. [['company_id','=',X]])
    // Returns: id, name, email, company_name, portal_active, has_password
    // ----------------------------------------------------------
    nlohmann::json handleSearchRead(const CallKwArgs& call) {
        int lim = call.limit() > 0 ? call.limit() : 200;
        int off = call.offset();

        auto [where, paramVec] = domainFromJson(call.domain()).toSql();

        // Qualify bare "company_id" references to "rp.company_id" to avoid
        // ambiguity: both rp and rc (the joined company partner) have company_id.
        {
            const std::string from = "company_id";
            const std::string to   = "rp.company_id";
            size_t pos = 0;
            while ((pos = where.find(from, pos)) != std::string::npos) {
                // Skip if already table-qualified (preceded by a dot)
                if (pos > 0 && where[pos - 1] == '.') { pos += from.size(); continue; }
                where.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        std::string sql = R"(
            SELECT rp.id,
                   rp.name,
                   rp.email,
                   rp.phone,
                   rp.portal_active,
                   (rp.portal_password_hash IS NOT NULL) AS has_password,
                   rc.id   AS company_id,
                   rc.name AS company_name
            FROM res_partner rp
            LEFT JOIN res_partner rc ON rc.id = rp.company_id
            WHERE )";
        sql += where;
        sql += " ORDER BY rp.name";
        sql += " LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);

        pqxx::result res;
        if (paramVec.empty()) {
            res = txn.exec(sql);
        } else {
            pqxx::params p; for (auto& s : paramVec) p.append(s);
            res = txn.exec(sql, p);
        }

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json obj;
            obj["id"]           = row["id"].as<int>();
            obj["name"]         = row["name"].is_null()         ? nlohmann::json(false) : nlohmann::json(row["name"].c_str());
            obj["email"]        = row["email"].is_null()        ? nlohmann::json(false) : nlohmann::json(row["email"].c_str());
            obj["phone"]        = row["phone"].is_null()        ? nlohmann::json(false) : nlohmann::json(row["phone"].c_str());
            obj["portal_active"]= row["portal_active"].is_null()? false : (std::string(row["portal_active"].c_str()) == "t");
            obj["has_password"] = row["has_password"].is_null() ? false : (std::string(row["has_password"].c_str()) == "t");
            obj["company_name"] = row["company_name"].is_null() ? nlohmann::json(false) : nlohmann::json(row["company_name"].c_str());
            if (!row["company_id"].is_null()) {
                nlohmann::json co = nlohmann::json::array();
                co.push_back(row["company_id"].as<int>());
                co.push_back(row["company_name"].is_null() ? "" : std::string(row["company_name"].c_str()));
                obj["company_id"] = co;
            } else {
                obj["company_id"] = false;
            }
            arr.push_back(std::move(obj));
        }
        return arr;
    }

    // ----------------------------------------------------------
    // write(ids, vals) — update portal_active
    // ----------------------------------------------------------
    nlohmann::json handleWrite(const CallKwArgs& call) {
        const auto ids  = call.ids();
        const auto vals = call.arg(1);
        if (!vals.is_object()) throw std::runtime_error("write: args[1] must be a dict");

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int id : ids) {
            if (vals.contains("portal_active")) {
                const bool active = vals["portal_active"].is_boolean()
                    ? vals["portal_active"].get<bool>()
                    : (vals["portal_active"].is_number() && vals["portal_active"].get<int>() != 0);
                txn.exec(
                    "UPDATE res_partner SET portal_active=$1, write_date=now() WHERE id=$2",
                    pqxx::params{active, id});
            }
        }
        txn.commit();
        return true;
    }

    // ----------------------------------------------------------
    // set_portal_password(ids, {password}) — hash & store custom password
    // ----------------------------------------------------------
    nlohmann::json handleSetPortalPassword(const CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) throw std::runtime_error("set_portal_password: no ids");

        if (!call.kwargs.contains("password"))
            throw std::runtime_error("set_portal_password: missing password");

        const std::string plain = call.kwargs["password"].get<std::string>();
        if (plain.size() < 8)
            throw std::runtime_error("Password must be at least 8 characters");

        const std::string hash = portalHashPassword(plain);

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int id : ids) {
            txn.exec(
                "UPDATE res_partner SET portal_password_hash=$1, write_date=now() WHERE id=$2",
                pqxx::params{hash, id});
        }
        txn.commit();
        return true;
    }

    // ----------------------------------------------------------
    // get_companies() — list of company partners for filter dropdown
    // ----------------------------------------------------------
    nlohmann::json handleGetCompanies(const CallKwArgs& /*call*/) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        auto rows = txn.exec(
            "SELECT id, name FROM res_partner "
            "WHERE is_company=TRUE AND active=TRUE "
            "ORDER BY name");

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : rows) {
            arr.push_back({
                {"id",   row["id"].as<int>()},
                {"name", std::string(row["name"].c_str())},
            });
        }
        return arr;
    }

    // ----------------------------------------------------------
    // portal_reset_password(ids)
    // Sets portal_password_hash = hash("Welcome1"), portal_active = TRUE
    // ----------------------------------------------------------
    nlohmann::json handleResetPassword(const CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty())
            throw std::runtime_error("portal_reset_password: no ids provided");

        const std::string hash = portalHashPassword("Welcome1");

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int id : ids) {
            txn.exec(
                "UPDATE res_partner "
                "SET portal_password_hash=$1, portal_active=TRUE "
                "WHERE id=$2",
                pqxx::params{hash, id});
        }
        txn.commit();

        return nlohmann::json::array_t{};
    }

    // ----------------------------------------------------------
    // portal_set_active(ids, {active: bool})
    // ----------------------------------------------------------
    nlohmann::json handleSetActive(const CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty())
            throw std::runtime_error("portal_set_active: no ids provided");

        const bool active = call.kwargs.value("active", false);

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int id : ids) {
            txn.exec(
                "UPDATE res_partner SET portal_active=$1 WHERE id=$2",
                pqxx::params{active, id});
        }
        txn.commit();

        return nlohmann::json::array_t{};
    }

    // ----------------------------------------------------------
    // portal_set_rental_price(ids, {product_id, price_unit})
    // INSERT/UPDATE partner_rental_price
    // ----------------------------------------------------------
    nlohmann::json handleSetRentalPrice(const CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty())
            throw std::runtime_error("portal_set_rental_price: no ids provided");

        if (!call.kwargs.contains("product_id"))
            throw std::runtime_error("portal_set_rental_price: missing product_id");
        if (!call.kwargs.contains("price_unit"))
            throw std::runtime_error("portal_set_rental_price: missing price_unit");

        const int    productId = call.kwargs["product_id"].get<int>();
        const double priceUnit = call.kwargs["price_unit"].get<double>();

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int partnerId : ids) {
            txn.exec(
                "INSERT INTO partner_rental_price (partner_id, product_id, price_unit) "
                "VALUES ($1, $2, $3) "
                "ON CONFLICT (partner_id, product_id) "
                "DO UPDATE SET price_unit = EXCLUDED.price_unit",
                pqxx::params{partnerId, productId, priceUnit});
        }
        txn.commit();

        return nlohmann::json::array_t{};
    }

    // ----------------------------------------------------------
    // portal_get_rental_prices(kwargs with partner_id)
    // ----------------------------------------------------------
    nlohmann::json handleGetRentalPrices(const CallKwArgs& call) {
        if (!call.kwargs.contains("partner_id"))
            throw std::runtime_error("portal_get_rental_prices: missing partner_id");

        const int partnerId = call.kwargs["partner_id"].get<int>();

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        auto rows = txn.exec(
            "SELECT prp.id, prp.partner_id, pp.id AS product_id, pp.name, prp.price_unit "
            "FROM partner_rental_price prp "
            "JOIN product_product pp ON pp.id = prp.product_id "
            "WHERE prp.partner_id=$1 "
            "ORDER BY pp.name",
            pqxx::params{partnerId});

        nlohmann::json result = nlohmann::json::array();
        for (const auto& row : rows) {
            result.push_back({
                {"id",         row["id"].as<int>()},
                {"partner_id", row["partner_id"].as<int>()},
                {"product_id", row["product_id"].as<int>()},
                {"name",       std::string(row["name"].c_str())},
                {"price_unit", row["price_unit"].as<double>()},
            });
        }
        return result;
    }

    // ----------------------------------------------------------
    // fields_get
    // ----------------------------------------------------------
    nlohmann::json handleFieldsGet(const CallKwArgs& /*call*/) {
        return {
            {"id",            {{"string","ID"},             {"type","integer"}}},
            {"name",          {{"string","Name"},           {"type","char"}}},
            {"email",         {{"string","Email"},          {"type","char"}}},
            {"company_name",  {{"string","Company"},        {"type","char"}}},
            {"portal_active", {{"string","Portal Access"},  {"type","boolean"}}},
            {"has_password",  {{"string","Has Password"},   {"type","boolean"}}},
        };
    }
};


// ================================================================
// PortalModule
// ================================================================

/**
 * @brief Customer-facing portal module.
 *
 * Registers:
 *   - PortalPartnerViewModel as "portal.partner"
 *   - 10 HTTP routes under /portal/...
 *   - DB schema (idempotent ALTER TABLE / CREATE TABLE)
 *   - Upload directory data/payment_proofs/
 */
class PortalModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "portal"; }

    explicit PortalModule(core::ModelFactory&     /*modelFactory*/,
                          core::ServiceFactory&   serviceFactory,
                          core::ViewModelFactory& viewModelFactory,
                          core::ViewFactory&      /*viewFactory*/)
        : db_           (serviceFactory.db())
        , viewModels_   (viewModelFactory)
        , portalSessions_(std::make_shared<PortalSessionManager>())
        , rateLimiter_  (std::make_shared<PortalLoginRateLimiter>())
    {}

    std::string              moduleName()   const override { return "portal"; }
    std::string              version()      const override { return "19.0.1.0.0"; }
    std::vector<std::string> dependencies() const override {
        return {"base", "account", "product"};
    }

    void registerModels()   override {}
    void registerServices() override {}
    void registerViews()    override {}

    // ----------------------------------------------------------
    // registerViewModels
    // ----------------------------------------------------------
    void registerViewModels() override {
        auto db = db_;
        viewModels_.registerCreator("portal.partner", [db]{
            return std::make_shared<PortalPartnerViewModel>(db);
        });
    }

    // ----------------------------------------------------------
    // registerRoutes — 10 portal HTTP routes
    // ----------------------------------------------------------
    void registerRoutes() override {
        auto db             = db_;
        auto portalSessions = portalSessions_;
        auto rateLimiter    = rateLimiter_;

        // Shared helper: add security headers to a response
        auto addSecHeaders = [](const drogon::HttpResponsePtr& res) {
            res->addHeader("X-Content-Type-Options", "nosniff");
            res->addHeader("X-Frame-Options",        "DENY");
            res->addHeader("Referrer-Policy",
                           "strict-origin-when-cross-origin");
        };

        // ----------------------------------------------------------
        // a. GET /portal  — serve web/static/portal.html
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal",
            [addSecHeaders](const drogon::HttpRequestPtr& /*req*/,
                            std::function<void(const drogon::HttpResponsePtr&)>&& cb)
            {
                auto res = drogon::HttpResponse::newFileResponse(
                    "web/static/portal.html");
                addSecHeaders(res);
                cb(res);
            },
            {drogon::Get});

        // ----------------------------------------------------------
        // b. POST /portal/api/login
        // Body: {email, password}
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/login",
            [db, portalSessions, rateLimiter, addSecHeaders](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                addSecHeaders(res);

                const std::string ip = req->getPeerAddr().toIp();

                // Rate limit check
                if (!rateLimiter->allow(ip)) {
                    res->setStatusCode(drogon::k429TooManyRequests);
                    res->setBody(nlohmann::json{
                        {"error", "Too many login attempts. Please try again later."}
                    }.dump());
                    cb(res);
                    return;
                }

                try {
                    nlohmann::json body;
                    try {
                        body = nlohmann::json::parse(req->body());
                    } catch (...) {
                        res->setStatusCode(drogon::k400BadRequest);
                        res->setBody(nlohmann::json{{"error", "Invalid JSON"}}.dump());
                        cb(res);
                        return;
                    }

                    const std::string email    = body.value("email",    "");
                    const std::string password = body.value("password", "");

                    if (email.empty() || password.empty()) {
                        res->setStatusCode(drogon::k400BadRequest);
                        res->setBody(nlohmann::json{
                            {"error", "email and password are required"}
                        }.dump());
                        cb(res);
                        return;
                    }

                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};

                    auto rows = txn.exec(
                        "SELECT id, name, email, portal_password_hash "
                        "FROM res_partner "
                        "WHERE LOWER(email) = LOWER($1) AND portal_active = TRUE "
                        "LIMIT 1",
                        pqxx::params{email});

                    if (rows.empty()) {
                        rateLimiter->recordFailure(ip);
                        res->setStatusCode(drogon::k401Unauthorized);
                        res->setBody(nlohmann::json{{"error", "Invalid credentials"}}.dump());
                        cb(res);
                        return;
                    }

                    const auto& row         = rows[0];
                    const int   partnerId   = row["id"].as<int>();
                    const std::string name  = row["name"].c_str();
                    const std::string storedHash = row["portal_password_hash"].is_null()
                                                    ? ""
                                                    : std::string(row["portal_password_hash"].c_str());

                    if (storedHash.empty() || !portalVerifyPassword(password, storedHash)) {
                        rateLimiter->recordFailure(ip);
                        res->setStatusCode(drogon::k401Unauthorized);
                        res->setBody(nlohmann::json{{"error", "Invalid credentials"}}.dump());
                        cb(res);
                        return;
                    }

                    rateLimiter->recordSuccess(ip);

                    const std::string sid = portalSessions->create(partnerId, name, email);

                    drogon::Cookie cookie(PortalSessionManager::kCookieName, sid);
                    cookie.setHttpOnly(true);
                    cookie.setSameSite(drogon::Cookie::SameSite::kLax);
                    cookie.setPath("/");
                    cookie.setMaxAge(8 * 3600); // 8 hours

                    res->setStatusCode(drogon::k200OK);
                    res->addCookie(cookie);
                    res->setBody(nlohmann::json{
                        {"ok",    true},
                        {"name",  name},
                        {"email", email},
                    }.dump());
                    cb(res);
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{{"error", e.what()}}.dump());
                    cb(res);
                }
            },
            {drogon::Post});

        // ----------------------------------------------------------
        // c. POST /portal/api/logout
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/logout",
            [portalSessions, addSecHeaders](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                addSecHeaders(res);

                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                if (!sid.empty()) {
                    portalSessions->remove(sid);
                }

                // Clear the cookie by setting Max-Age=0
                drogon::Cookie cookie(PortalSessionManager::kCookieName, "");
                cookie.setHttpOnly(true);
                cookie.setSameSite(drogon::Cookie::SameSite::kLax);
                cookie.setPath("/");
                cookie.setMaxAge(0);

                res->setStatusCode(drogon::k200OK);
                res->addCookie(cookie);
                res->setBody(nlohmann::json{{"ok", true}}.dump());
                cb(res);
            },
            {drogon::Post});

        // ----------------------------------------------------------
        // e. POST /portal/api/change-password  — require portal session
        // Body: {current_password, new_password}
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/change-password",
            [db, portalSessions, addSecHeaders](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                addSecHeaders(res);

                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) {
                    res->setStatusCode(drogon::k401Unauthorized);
                    res->setBody(nlohmann::json{{"error", "Not authenticated"}}.dump());
                    cb(res);
                    return;
                }

                try {
                    nlohmann::json body;
                    try {
                        body = nlohmann::json::parse(req->body());
                    } catch (...) {
                        res->setStatusCode(drogon::k400BadRequest);
                        res->setBody(nlohmann::json{{"error", "Invalid JSON"}}.dump());
                        cb(res);
                        return;
                    }

                    const std::string currentPassword = body.value("current_password", "");
                    const std::string newPassword     = body.value("new_password",     "");

                    if (currentPassword.empty() || newPassword.empty()) {
                        res->setStatusCode(drogon::k400BadRequest);
                        res->setBody(nlohmann::json{
                            {"error", "current_password and new_password are required"}
                        }.dump());
                        cb(res);
                        return;
                    }

                    if (newPassword.size() < 8) {
                        res->setStatusCode(drogon::k400BadRequest);
                        res->setBody(nlohmann::json{
                            {"error", "new_password must be at least 8 characters"}
                        }.dump());
                        cb(res);
                        return;
                    }

                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};

                    // Fetch current stored hash
                    auto rows = txn.exec(
                        "SELECT portal_password_hash FROM res_partner WHERE id=$1",
                        pqxx::params{session->partnerId});

                    if (rows.empty()) {
                        res->setStatusCode(drogon::k404NotFound);
                        res->setBody(nlohmann::json{{"error", "Partner not found"}}.dump());
                        cb(res);
                        return;
                    }

                    const std::string storedHash = rows[0]["portal_password_hash"].is_null()
                                                    ? ""
                                                    : std::string(rows[0]["portal_password_hash"].c_str());

                    if (storedHash.empty() || !portalVerifyPassword(currentPassword, storedHash)) {
                        res->setStatusCode(drogon::k401Unauthorized);
                        res->setBody(nlohmann::json{{"error", "Current password is incorrect"}}.dump());
                        cb(res);
                        return;
                    }

                    const std::string newHash = portalHashPassword(newPassword);
                    txn.exec(
                        "UPDATE res_partner SET portal_password_hash=$1 WHERE id=$2",
                        pqxx::params{newHash, session->partnerId});
                    txn.commit();

                    res->setStatusCode(drogon::k200OK);
                    res->setBody(nlohmann::json{{"ok", true}}.dump());
                    cb(res);
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{{"error", e.what()}}.dump());
                    cb(res);
                }
            },
            {drogon::Post});

        // ----------------------------------------------------------
        // f. GET /portal/api/invoices  — require portal session
        // Returns invoices with attached payment proofs
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/invoices",
            [db, portalSessions, addSecHeaders](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                addSecHeaders(res);

                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) {
                    res->setStatusCode(drogon::k401Unauthorized);
                    res->setBody(nlohmann::json{{"error", "Not authenticated"}}.dump());
                    cb(res);
                    return;
                }

                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};

                    auto invoiceRows = txn.exec(
                        "SELECT id, name, invoice_date, amount_total, payment_state, state "
                        "FROM account_move "
                        "WHERE partner_id=$1 AND move_type='out_invoice' "
                        "ORDER BY id DESC LIMIT 50",
                        pqxx::params{session->partnerId});

                    // Fetch all payment proofs for this partner at once
                    auto proofRows = txn.exec(
                        "SELECT id, invoice_id, filename, upload_date "
                        "FROM payment_proof "
                        "WHERE partner_id=$1 "
                        "ORDER BY invoice_id",
                        pqxx::params{session->partnerId});

                    // Build proof map: invoice_id -> array of proofs
                    std::unordered_map<int, nlohmann::json> proofMap;
                    for (const auto& pr : proofRows) {
                        const int invoiceId = pr["invoice_id"].as<int>();
                        if (proofMap.find(invoiceId) == proofMap.end())
                            proofMap[invoiceId] = nlohmann::json::array();
                        proofMap[invoiceId].push_back({
                            {"id",          pr["id"].as<int>()},
                            {"filename",    std::string(pr["filename"].c_str())},
                            {"upload_date", pr["upload_date"].is_null()
                                            ? ""
                                            : std::string(pr["upload_date"].c_str())},
                        });
                    }

                    nlohmann::json result = nlohmann::json::array();
                    for (const auto& row : invoiceRows) {
                        const int invoiceId = row["id"].as<int>();
                        nlohmann::json inv = {
                            {"id",            invoiceId},
                            {"name",          std::string(row["name"].c_str())},
                            {"invoice_date",  row["invoice_date"].is_null()
                                              ? ""
                                              : std::string(row["invoice_date"].c_str())},
                            {"amount_total",  row["amount_total"].is_null()
                                              ? 0.0
                                              : row["amount_total"].as<double>()},
                            {"payment_state", row["payment_state"].is_null()
                                              ? ""
                                              : std::string(row["payment_state"].c_str())},
                            {"state",         std::string(row["state"].c_str())},
                            {"proofs",        proofMap.count(invoiceId)
                                              ? proofMap[invoiceId]
                                              : nlohmann::json::array()},
                        };
                        result.push_back(std::move(inv));
                    }

                    res->setStatusCode(drogon::k200OK);
                    res->setBody(result.dump());
                    cb(res);
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{{"error", e.what()}}.dump());
                    cb(res);
                }
            },
            {drogon::Get});

        // ----------------------------------------------------------
        // g. GET /portal/api/invoice/{id}/detail  — require portal session
        // Verify invoice belongs to partner, return header + lines
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/invoice/{1}/detail",
            [db, portalSessions, addSecHeaders](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& idStr)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                addSecHeaders(res);

                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) {
                    res->setStatusCode(drogon::k401Unauthorized);
                    res->setBody(nlohmann::json{{"error", "Not authenticated"}}.dump());
                    cb(res);
                    return;
                }

                int invoiceId = 0;
                try { invoiceId = std::stoi(idStr); } catch (...) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{{"error", "Invalid invoice id"}}.dump());
                    cb(res);
                    return;
                }

                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};

                    // Verify invoice belongs to this partner
                    auto hdrRows = txn.exec(
                        "SELECT id, name, invoice_date, amount_total, amount_untaxed, "
                        "payment_state, state "
                        "FROM account_move "
                        "WHERE id=$1 AND partner_id=$2 AND move_type='out_invoice'",
                        pqxx::params{invoiceId, session->partnerId});

                    if (hdrRows.empty()) {
                        res->setStatusCode(drogon::k404NotFound);
                        res->setBody(nlohmann::json{{"error", "Invoice not found"}}.dump());
                        cb(res);
                        return;
                    }

                    const auto& hdr = hdrRows[0];

                    // Fetch invoice lines
                    auto lineRows = txn.exec(
                        "SELECT name, quantity, price_unit, "
                        "(quantity * price_unit) AS subtotal "
                        "FROM account_move_line "
                        "WHERE move_id=$1 AND display_type='' "
                        "ORDER BY id",
                        pqxx::params{invoiceId});

                    nlohmann::json lines = nlohmann::json::array();
                    for (const auto& lr : lineRows) {
                        lines.push_back({
                            {"name",       std::string(lr["name"].c_str())},
                            {"quantity",   lr["quantity"].as<double>()},
                            {"price_unit", lr["price_unit"].as<double>()},
                            {"subtotal",   lr["subtotal"].as<double>()},
                        });
                    }

                    nlohmann::json result = {
                        {"id",            hdr["id"].as<int>()},
                        {"name",          std::string(hdr["name"].c_str())},
                        {"invoice_date",  hdr["invoice_date"].is_null()
                                          ? ""
                                          : std::string(hdr["invoice_date"].c_str())},
                        {"amount_total",  hdr["amount_total"].is_null()
                                          ? 0.0
                                          : hdr["amount_total"].as<double>()},
                        {"amount_untaxed",hdr["amount_untaxed"].is_null()
                                          ? 0.0
                                          : hdr["amount_untaxed"].as<double>()},
                        {"payment_state", hdr["payment_state"].is_null()
                                          ? ""
                                          : std::string(hdr["payment_state"].c_str())},
                        {"state",         std::string(hdr["state"].c_str())},
                        {"lines",         lines},
                    };

                    res->setStatusCode(drogon::k200OK);
                    res->setBody(result.dump());
                    cb(res);
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{{"error", e.what()}}.dump());
                    cb(res);
                }
            },
            {drogon::Get});

        // ----------------------------------------------------------
        // h2. GET /portal/api/invoice/{id}/print  — portal-authenticated, same format as backend
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/invoice/{1}/print",
            [db, portalSessions](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& idStr)
            {
                auto htmlErr = [&cb](int code, const std::string& msg) {
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                    r->setContentTypeCode(drogon::CT_TEXT_HTML);
                    r->setBody("<html><body><p>" + msg + "</p></body></html>");
                    cb(r);
                };
                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) { htmlErr(401, "Not authenticated"); return; }
                int recordId = 0;
                try { recordId = std::stoi(idStr); } catch (...) { htmlErr(400, "Invalid id"); return; }
                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    std::string html = portalRenderDoc("account.move", recordId, session->partnerId, txn);
                    if (html.empty()) { htmlErr(404, "Invoice not found or access denied"); return; }
                    const std::string autoprint = "<script>window.onload=function(){window.print();}</script>";
                    size_t pos = html.rfind("</body>");
                    if (pos != std::string::npos) html.insert(pos, autoprint);
                    auto res = drogon::HttpResponse::newHttpResponse();
                    res->setStatusCode(drogon::k200OK);
                    res->setContentTypeCode(drogon::CT_TEXT_HTML);
                    res->setBody(html);
                    cb(res);
                } catch (const std::exception& e) {
                    htmlErr(500, std::string("Error: ") + e.what());
                }
            },
            {drogon::Get});

        // ----------------------------------------------------------
        // k. GET /portal/api/orders  — list of sale orders for this partner
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/orders",
            [db, portalSessions, addSecHeaders](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                addSecHeaders(res);
                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) {
                    res->setStatusCode(drogon::k401Unauthorized);
                    res->setBody(nlohmann::json{{"error","Not authenticated"}}.dump());
                    cb(res); return;
                }
                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    auto rows = txn.exec(
                        "SELECT so.id, so.name, so.state, "
                        "to_char(so.date_order AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS date_order, "
                        "COALESCE(so.amount_total,0) AS amount_total "
                        "FROM sale_order so "
                        "WHERE so.partner_id=$1 ORDER BY so.id DESC",
                        pqxx::params{session->partnerId});
                    nlohmann::json result = nlohmann::json::array();
                    for (const auto& row : rows)
                        result.push_back({{"id",row["id"].as<int>()},{"name",std::string(row["name"].c_str())},
                            {"state",std::string(row["state"].c_str())},
                            {"date_order",row["date_order"].is_null()?"":std::string(row["date_order"].c_str())},
                            {"amount_total",row["amount_total"].as<double>()}});
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(result.dump());
                    cb(res);
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{{"error",e.what()}}.dump());
                    cb(res);
                }
            },
            {drogon::Get});

        // ----------------------------------------------------------
        // l. GET /portal/api/order/{id}/detail
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/order/{1}/detail",
            [db, portalSessions, addSecHeaders](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& idStr)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                addSecHeaders(res);
                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) {
                    res->setStatusCode(drogon::k401Unauthorized);
                    res->setBody(nlohmann::json{{"error","Not authenticated"}}.dump());
                    cb(res); return;
                }
                int orderId = 0;
                try { orderId = std::stoi(idStr); } catch (...) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{{"error","Invalid id"}}.dump());
                    cb(res); return;
                }
                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    auto hdr = txn.exec(
                        "SELECT so.id, so.name, so.state, "
                        "to_char(so.date_order AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS date_order, "
                        "COALESCE(so.amount_untaxed,0) AS amount_untaxed, "
                        "COALESCE(so.amount_tax,0) AS amount_tax, "
                        "COALESCE(so.amount_total,0) AS amount_total "
                        "FROM sale_order so WHERE so.id=$1 AND so.partner_id=$2",
                        pqxx::params{orderId, session->partnerId});
                    if (hdr.empty()) {
                        res->setStatusCode(drogon::k404NotFound);
                        res->setBody(nlohmann::json{{"error","Order not found"}}.dump());
                        cb(res); return;
                    }
                    auto lrows = txn.exec(
                        "SELECT COALESCE(sol.name, pp.name, '') AS name, "
                        "sol.product_uom_qty AS quantity, sol.price_unit, "
                        "sol.price_subtotal AS subtotal, COALESCE(uu.name,'') AS uom "
                        "FROM sale_order_line sol "
                        "LEFT JOIN product_product pp ON pp.id = sol.product_id "
                        "LEFT JOIN uom_uom uu ON uu.id = sol.product_uom_id "
                        "WHERE sol.order_id=$1 ORDER BY sol.id",
                        pqxx::params{orderId});
                    nlohmann::json lines = nlohmann::json::array();
                    for (const auto& lr : lrows)
                        lines.push_back({{"name",std::string(lr["name"].c_str())},
                            {"quantity",lr["quantity"].as<double>()},
                            {"price_unit",lr["price_unit"].as<double>()},
                            {"subtotal",lr["subtotal"].as<double>()},
                            {"uom",std::string(lr["uom"].c_str())}});
                    const auto& r = hdr[0];
                    nlohmann::json result = {
                        {"id",r["id"].as<int>()},{"name",std::string(r["name"].c_str())},
                        {"state",std::string(r["state"].c_str())},
                        {"date_order",r["date_order"].is_null()?"":std::string(r["date_order"].c_str())},
                        {"amount_untaxed",r["amount_untaxed"].as<double>()},
                        {"amount_tax",r["amount_tax"].as<double>()},
                        {"amount_total",r["amount_total"].as<double>()},{"lines",lines}};
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(result.dump());
                    cb(res);
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{{"error",e.what()}}.dump());
                    cb(res);
                }
            },
            {drogon::Get});

        // ----------------------------------------------------------
        // m. GET /portal/api/order/{id}/print
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/order/{1}/print",
            [db, portalSessions](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& idStr)
            {
                auto htmlErr = [&cb](int code, const std::string& msg) {
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                    r->setContentTypeCode(drogon::CT_TEXT_HTML);
                    r->setBody("<html><body><p>" + msg + "</p></body></html>");
                    cb(r);
                };
                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) { htmlErr(401, "Not authenticated"); return; }
                int recordId = 0;
                try { recordId = std::stoi(idStr); } catch (...) { htmlErr(400, "Invalid id"); return; }
                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    std::string html = portalRenderDoc("sale.order", recordId, session->partnerId, txn);
                    if (html.empty()) { htmlErr(404, "Order not found or access denied"); return; }
                    const std::string autoprint = "<script>window.onload=function(){window.print();}</script>";
                    size_t pos = html.rfind("</body>");
                    if (pos != std::string::npos) html.insert(pos, autoprint);
                    auto res = drogon::HttpResponse::newHttpResponse();
                    res->setStatusCode(drogon::k200OK);
                    res->setContentTypeCode(drogon::CT_TEXT_HTML);
                    res->setBody(html);
                    cb(res);
                } catch (const std::exception& e) {
                    htmlErr(500, std::string("Error: ") + e.what());
                }
            },
            {drogon::Get});

        // ----------------------------------------------------------
        // n. GET /portal/api/deliveries  — outgoing stock_picking for this partner
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/deliveries",
            [db, portalSessions, addSecHeaders](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                addSecHeaders(res);
                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) {
                    res->setStatusCode(drogon::k401Unauthorized);
                    res->setBody(nlohmann::json{{"error","Not authenticated"}}.dump());
                    cb(res); return;
                }
                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    auto rows = txn.exec(
                        "SELECT sp.id, sp.name, sp.state, "
                        "COALESCE(sp.origin,'') AS origin, "
                        "to_char(sp.scheduled_date AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS scheduled_date, "
                        "COALESCE(spt.code,'') AS picking_type_code "
                        "FROM stock_picking sp "
                        "LEFT JOIN stock_picking_type spt ON spt.id = sp.picking_type_id "
                        "WHERE sp.partner_id=$1 AND spt.code='outgoing' "
                        "ORDER BY sp.id DESC",
                        pqxx::params{session->partnerId});
                    nlohmann::json result = nlohmann::json::array();
                    for (const auto& row : rows)
                        result.push_back({{"id",row["id"].as<int>()},{"name",std::string(row["name"].c_str())},
                            {"state",std::string(row["state"].c_str())},
                            {"origin",std::string(row["origin"].c_str())},
                            {"scheduled_date",row["scheduled_date"].is_null()?"":std::string(row["scheduled_date"].c_str())}});
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(result.dump());
                    cb(res);
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{{"error",e.what()}}.dump());
                    cb(res);
                }
            },
            {drogon::Get});

        // ----------------------------------------------------------
        // o. GET /portal/api/delivery/{id}/detail
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/delivery/{1}/detail",
            [db, portalSessions, addSecHeaders](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& idStr)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                addSecHeaders(res);
                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) {
                    res->setStatusCode(drogon::k401Unauthorized);
                    res->setBody(nlohmann::json{{"error","Not authenticated"}}.dump());
                    cb(res); return;
                }
                int pickId = 0;
                try { pickId = std::stoi(idStr); } catch (...) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{{"error","Invalid id"}}.dump());
                    cb(res); return;
                }
                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    auto hdr = txn.exec(
                        "SELECT sp.id, sp.name, sp.state, "
                        "COALESCE(sp.origin,'') AS origin, "
                        "to_char(sp.scheduled_date AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS scheduled_date, "
                        "COALESCE(spt.code,'') AS picking_type_code "
                        "FROM stock_picking sp "
                        "LEFT JOIN stock_picking_type spt ON spt.id = sp.picking_type_id "
                        "WHERE sp.id=$1 AND sp.partner_id=$2",
                        pqxx::params{pickId, session->partnerId});
                    if (hdr.empty()) {
                        res->setStatusCode(drogon::k404NotFound);
                        res->setBody(nlohmann::json{{"error","Delivery not found"}}.dump());
                        cb(res); return;
                    }
                    auto lrows = txn.exec(
                        "SELECT COALESCE(pp.name, sm.name, '') AS name, "
                        "sm.product_uom_qty AS demand, sm.quantity AS done, "
                        "COALESCE(uu.name,'') AS uom "
                        "FROM stock_move sm "
                        "LEFT JOIN product_product pp ON pp.id = sm.product_id "
                        "LEFT JOIN uom_uom uu ON uu.id = sm.product_uom_id "
                        "WHERE sm.picking_id=$1 ORDER BY sm.id",
                        pqxx::params{pickId});
                    nlohmann::json lines = nlohmann::json::array();
                    for (const auto& lr : lrows)
                        lines.push_back({{"name",std::string(lr["name"].c_str())},
                            {"demand",lr["demand"].as<double>()},{"done",lr["done"].as<double>()},
                            {"uom",std::string(lr["uom"].c_str())}});
                    const auto& r = hdr[0];
                    std::string code = std::string(r["picking_type_code"].c_str());
                    std::string docTitle = (code == "incoming") ? "Receipt" :
                                           (code == "outgoing") ? "Delivery Order" : "Transfer";
                    nlohmann::json result = {
                        {"id",r["id"].as<int>()},{"name",std::string(r["name"].c_str())},
                        {"state",std::string(r["state"].c_str())},
                        {"origin",std::string(r["origin"].c_str())},
                        {"document_title",docTitle},
                        {"scheduled_date",r["scheduled_date"].is_null()?"":std::string(r["scheduled_date"].c_str())},
                        {"lines",lines}};
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(result.dump());
                    cb(res);
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{{"error",e.what()}}.dump());
                    cb(res);
                }
            },
            {drogon::Get});

        // ----------------------------------------------------------
        // p. GET /portal/api/delivery/{id}/print
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/delivery/{1}/print",
            [db, portalSessions](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& idStr)
            {
                auto htmlErr = [&cb](int code, const std::string& msg) {
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                    r->setContentTypeCode(drogon::CT_TEXT_HTML);
                    r->setBody("<html><body><p>" + msg + "</p></body></html>");
                    cb(r);
                };
                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) { htmlErr(401, "Not authenticated"); return; }
                int recordId = 0;
                try { recordId = std::stoi(idStr); } catch (...) { htmlErr(400, "Invalid id"); return; }
                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    std::string html = portalRenderDoc("stock.picking", recordId, session->partnerId, txn);
                    if (html.empty()) { htmlErr(404, "Delivery not found or access denied"); return; }
                    const std::string autoprint = "<script>window.onload=function(){window.print();}</script>";
                    size_t pos = html.rfind("</body>");
                    if (pos != std::string::npos) html.insert(pos, autoprint);
                    auto res = drogon::HttpResponse::newHttpResponse();
                    res->setStatusCode(drogon::k200OK);
                    res->setContentTypeCode(drogon::CT_TEXT_HTML);
                    res->setBody(html);
                    cb(res);
                } catch (const std::exception& e) {
                    htmlErr(500, std::string("Error: ") + e.what());
                }
            },
            {drogon::Get});

        // ----------------------------------------------------------
        // h. POST /portal/api/invoice/{id}/proof  — multipart upload
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/invoice/{1}/proof",
            [db, portalSessions, addSecHeaders](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& idStr)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                addSecHeaders(res);

                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) {
                    res->setStatusCode(drogon::k401Unauthorized);
                    res->setBody(nlohmann::json{{"error", "Not authenticated"}}.dump());
                    cb(res);
                    return;
                }

                int invoiceId = 0;
                try { invoiceId = std::stoi(idStr); } catch (...) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{{"error", "Invalid invoice id"}}.dump());
                    cb(res);
                    return;
                }

                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};

                    // Verify invoice belongs to this partner
                    auto checkRows = txn.exec(
                        "SELECT id FROM account_move "
                        "WHERE id=$1 AND partner_id=$2 AND move_type='out_invoice'",
                        pqxx::params{invoiceId, session->partnerId});

                    if (checkRows.empty()) {
                        res->setStatusCode(drogon::k404NotFound);
                        res->setBody(nlohmann::json{{"error", "Invoice not found"}}.dump());
                        cb(res);
                        return;
                    }

                    // Parse multipart form
                    drogon::MultiPartParser parser;
                    if (parser.parse(req) != 0) {
                        res->setStatusCode(drogon::k400BadRequest);
                        res->setBody(nlohmann::json{{"error", "Failed to parse multipart body"}}.dump());
                        cb(res);
                        return;
                    }

                    const auto& files = parser.getFiles();
                    if (files.empty()) {
                        res->setStatusCode(drogon::k400BadRequest);
                        res->setBody(nlohmann::json{{"error", "No file uploaded"}}.dump());
                        cb(res);
                        return;
                    }

                    const auto& file = files[0];

                    // Generate unique filename: {invoiceId}_{timestamp}_{original}
                    const auto now = std::chrono::system_clock::now();
                    const auto ts  = std::chrono::duration_cast<std::chrono::seconds>(
                        now.time_since_epoch()).count();

                    const std::string originalName = file.getFileName();
                    const std::string uniqueName   =
                        std::to_string(invoiceId) + "_" +
                        std::to_string(ts)          + "_" +
                        originalName;

                    const std::string savePath = "data/payment_proofs/" + uniqueName;

                    // Save file to disk
                    file.saveAs(savePath);

                    // Determine mimetype from content-type header, fallback to generic
                    const std::string mimetype = "application/octet-stream";

                    // Insert into payment_proof
                    txn.exec(
                        "INSERT INTO payment_proof "
                        "(invoice_id, partner_id, filename, mimetype, filepath) "
                        "VALUES ($1, $2, $3, $4, $5)",
                        pqxx::params{invoiceId, session->partnerId,
                                     originalName, mimetype, savePath});
                    txn.commit();

                    res->setStatusCode(drogon::k200OK);
                    res->setBody(nlohmann::json{
                        {"ok",       true},
                        {"filename", originalName},
                    }.dump());
                    cb(res);
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{{"error", e.what()}}.dump());
                    cb(res);
                }
            },
            {drogon::Post});

        // ----------------------------------------------------------
        // i. GET /portal/api/products  — require portal session
        // Returns partner-specific rental prices
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/products",
            [db, portalSessions, addSecHeaders](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                addSecHeaders(res);

                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) {
                    res->setStatusCode(drogon::k401Unauthorized);
                    res->setBody(nlohmann::json{{"error", "Not authenticated"}}.dump());
                    cb(res);
                    return;
                }

                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};

                    auto rows = txn.exec(
                        "SELECT prp.id, pp.id AS product_id, pp.name, prp.price_unit "
                        "FROM partner_rental_price prp "
                        "JOIN product_product pp ON pp.id = prp.product_id "
                        "WHERE prp.partner_id=$1 "
                        "ORDER BY pp.name",
                        pqxx::params{session->partnerId});

                    nlohmann::json result = nlohmann::json::array();
                    for (const auto& row : rows) {
                        result.push_back({
                            {"id",         row["id"].as<int>()},
                            {"product_id", row["product_id"].as<int>()},
                            {"name",       std::string(row["name"].c_str())},
                            {"price_unit", row["price_unit"].as<double>()},
                        });
                    }

                    res->setStatusCode(drogon::k200OK);
                    res->setBody(result.dump());
                    cb(res);
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{{"error", e.what()}}.dump());
                    cb(res);
                }
            },
            {drogon::Get});

        // ----------------------------------------------------------
        // j. POST /portal/api/request  — require portal session
        // Body: {product_id}
        // Creates a draft invoice for the partner
        // ----------------------------------------------------------
        drogon::app().registerHandler("/portal/api/request",
            [db, portalSessions, addSecHeaders](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                addSecHeaders(res);

                const std::string sid = req->getCookie(PortalSessionManager::kCookieName);
                auto session = portalSessions->get(sid);
                if (!session) {
                    res->setStatusCode(drogon::k401Unauthorized);
                    res->setBody(nlohmann::json{{"error", "Not authenticated"}}.dump());
                    cb(res);
                    return;
                }

                try {
                    nlohmann::json body;
                    try {
                        body = nlohmann::json::parse(req->body());
                    } catch (...) {
                        res->setStatusCode(drogon::k400BadRequest);
                        res->setBody(nlohmann::json{{"error", "Invalid JSON"}}.dump());
                        cb(res);
                        return;
                    }

                    if (!body.contains("product_id")) {
                        res->setStatusCode(drogon::k400BadRequest);
                        res->setBody(nlohmann::json{{"error", "product_id is required"}}.dump());
                        cb(res);
                        return;
                    }

                    const int productId = body["product_id"].get<int>();

                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};

                    // Verify product is in partner_rental_price for this partner
                    auto priceRows = txn.exec(
                        "SELECT prp.price_unit, pp.name, pp.income_account_id "
                        "FROM partner_rental_price prp "
                        "JOIN product_product pp ON pp.id = prp.product_id "
                        "WHERE prp.partner_id=$1 AND prp.product_id=$2",
                        pqxx::params{session->partnerId, productId});

                    if (priceRows.empty()) {
                        res->setStatusCode(drogon::k403Forbidden);
                        res->setBody(nlohmann::json{
                            {"error", "Product not available for this partner"}
                        }.dump());
                        cb(res);
                        return;
                    }

                    const double      priceUnit      = priceRows[0]["price_unit"].as<double>();
                    const std::string productName    = priceRows[0]["name"].c_str();
                    const int         incomeAccountId = priceRows[0]["income_account_id"].is_null()
                                                        ? 6  // fallback to account id=6
                                                        : priceRows[0]["income_account_id"].as<int>();

                    // Create draft invoice header
                    auto invoiceRows = txn.exec(
                        "INSERT INTO account_move "
                        "(name, move_type, state, partner_id, journal_id, company_id, "
                        " invoice_date, invoice_origin) "
                        "VALUES ('/', 'out_invoice', 'draft', $1, 1, 1, CURRENT_DATE, 'Portal Request') "
                        "RETURNING id",
                        pqxx::params{session->partnerId});

                    if (invoiceRows.empty()) {
                        throw std::runtime_error("Failed to create invoice");
                    }

                    const int newInvoiceId = invoiceRows[0]["id"].as<int>();

                    // Create invoice line
                    txn.exec(
                        "INSERT INTO account_move_line "
                        "(move_id, account_id, journal_id, partner_id, name, quantity, price_unit, display_type) "
                        "VALUES ($1, $2, 1, $3, $4, 1, $5, '')",
                        pqxx::params{newInvoiceId, incomeAccountId,
                                     session->partnerId, productName, priceUnit});

                    txn.commit();

                    res->setStatusCode(drogon::k200OK);
                    res->setBody(nlohmann::json{
                        {"ok",         true},
                        {"invoice_id", newInvoiceId},
                    }.dump());
                    cb(res);
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{{"error", e.what()}}.dump());
                    cb(res);
                }
            },
            {drogon::Post});
    }

    // ----------------------------------------------------------
    // initialize — DB migrations + upload directory creation
    // ----------------------------------------------------------
    void initialize() override {
        ensureSchema_();
        ensureUploadDir_();
        seedMenu_();
    }

private:
    std::shared_ptr<infrastructure::DbConnection> db_;
    core::ViewModelFactory&                       viewModels_;
    std::shared_ptr<PortalSessionManager>         portalSessions_;
    std::shared_ptr<PortalLoginRateLimiter>       rateLimiter_;

    // ----------------------------------------------------------
    // Schema (idempotent)
    // ----------------------------------------------------------
    void ensureSchema_() {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        // Extend res_partner with portal columns
        txn.exec(
            "ALTER TABLE res_partner "
            "ADD COLUMN IF NOT EXISTS portal_password_hash VARCHAR");
        txn.exec(
            "ALTER TABLE res_partner "
            "ADD COLUMN IF NOT EXISTS portal_active BOOLEAN NOT NULL DEFAULT FALSE");

        // Payment proof uploads
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS payment_proof (
                id          SERIAL PRIMARY KEY,
                invoice_id  INTEGER NOT NULL REFERENCES account_move(id),
                partner_id  INTEGER NOT NULL REFERENCES res_partner(id),
                filename    VARCHAR NOT NULL,
                mimetype    VARCHAR NOT NULL,
                filepath    VARCHAR NOT NULL,
                upload_date TIMESTAMP DEFAULT now()
            )
        )");

        // Partner-specific rental prices
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS partner_rental_price (
                id          SERIAL PRIMARY KEY,
                partner_id  INTEGER NOT NULL REFERENCES res_partner(id),
                product_id  INTEGER NOT NULL REFERENCES product_product(id),
                price_unit  NUMERIC(16,2) NOT NULL DEFAULT 0,
                UNIQUE(partner_id, product_id)
            )
        )");

        // Backfill company_id FK for partners that have a company_name text value
        // but no company_id set (happens when contacts were saved before the FK fix).
        txn.exec(R"(
            UPDATE res_partner rp
            SET company_id = co.id
            FROM res_partner co
            WHERE co.is_company = TRUE
              AND co.active     = TRUE
              AND LOWER(co.name) = LOWER(rp.company_name)
              AND rp.company_id IS NULL
              AND rp.company_name IS NOT NULL
              AND rp.company_name <> ''
        )");

        txn.commit();
    }

    // ----------------------------------------------------------
    // Upload directory
    // ----------------------------------------------------------
    void ensureUploadDir_() {
        std::filesystem::create_directories("data/payment_proofs");
    }

    // ----------------------------------------------------------
    // seedMenu_ — IR action + menu item for Portal Users
    // ----------------------------------------------------------
    void seedMenu_() {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        // Action 35: open portal.partner list
        txn.exec(R"(
            INSERT INTO ir_act_window (id, name, res_model, view_mode, domain)
            VALUES (35, 'Portal Users', 'portal.partner', 'list,form', '[]')
            ON CONFLICT (id) DO NOTHING
        )");

        // Menu 120: Portal Users under Settings (id=30)
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, action_id, sequence, web_icon)
            VALUES (120, 'Portal Users', 30, 35, 35, 'settings')
            ON CONFLICT (id) DO NOTHING
        )");

        txn.commit();
    }
};

} // namespace odoo::modules::portal

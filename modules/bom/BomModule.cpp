// =============================================================
// modules/bom/BomModule.cpp — docs/107
// =============================================================
#include "BomModule.hpp"
#include <drogon/drogon.h>
#include "BaseViewModel.hpp"
#include "DbConnection.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace odoo::modules::bom {

using namespace odoo::infrastructure;
using namespace odoo::core;

// ── small helpers ───────────────────────────────────────────
static int jint(const nlohmann::json& v, const char* k, int dflt = 0) {
    if (!v.is_object() || !v.contains(k)) return dflt;
    if (v[k].is_number_integer()) return v[k].get<int>();
    if (v[k].is_array() && !v[k].empty() && v[k][0].is_number_integer()) return v[k][0].get<int>();
    if (v[k].is_string()) { try { return std::stoi(v[k].get<std::string>()); } catch (...) {} }
    return dflt;
}
static std::string jstr(const nlohmann::json& v, const char* k) {
    return (v.is_object() && v.contains(k) && v[k].is_string()) ? v[k].get<std::string>() : std::string{};
}
static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
static std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n\"'");
    if (a == std::string::npos) return "";
    const auto b = s.find_last_not_of(" \t\r\n\"'");
    return s.substr(a, b - a + 1);
}

/// Split "C1,C2, C5" / "C1 C2 C5" / "C1;C2" into individual designators.
/// Also expands the range forms real BOMs use: "R1-R4" and "R1..R4".
static std::vector<std::string> splitDesignators(const std::string& in) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        const std::string t = trim(cur);
        cur.clear();
        if (t.empty()) return;
        // A range only makes sense when both ends share a prefix and differ by number.
        const auto dash = t.find_first_of("-");
        const auto dots = t.find("..");
        const size_t sep = (dots != std::string::npos) ? dots : dash;
        const size_t seplen = (dots != std::string::npos) ? 2 : 1;
        if (sep != std::string::npos && sep > 0 && sep + seplen < t.size()) {
            const std::string a = trim(t.substr(0, sep));
            const std::string b = trim(t.substr(sep + seplen));
            auto split = [](const std::string& s, std::string& pre, long& num) {
                size_t i = s.size();
                while (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1]))) --i;
                if (i == s.size()) return false;
                pre = s.substr(0, i);
                try { num = std::stol(s.substr(i)); } catch (...) { return false; }
                return true;
            };
            std::string pa, pb; long na = 0, nb = 0;
            if (split(a, pa, na) && split(b, pb, nb) && pa == pb && nb >= na && nb - na < 512) {
                for (long n = na; n <= nb; ++n) out.push_back(pa + std::to_string(n));
                return;
            }
        }
        out.push_back(t);
    };
    for (const char c : in) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r') flush();
        else cur += c;
    }
    flush();
    return out;
}

/// Parse a CSV line honouring quoted fields — BOM exports quote descriptions
/// that contain commas, and a naive split turns one column into three.
static std::vector<std::string> csvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inQ = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (inQ) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
            else if (c == '"') inQ = false;
            else cur += c;
        } else if (c == '"') inQ = true;
        else if (c == ',' || c == ';' || c == '\t') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

// ================================================================
// bom.import — parse, resolve, review, commit
// ================================================================
//
// The importer never decides what a line IS. It resolves candidates from data
// already in the catalogue and reports what it found; a person commits. That is
// the same rule as part.lookup, for the same reason: a wrong capacitor that
// lands silently becomes a board that does not work.
//
// The AI seam is deliberately narrow. A model may supply the COLUMN MAPPING for
// a layout the deterministic header matcher does not recognise, or hand over
// already-normalised rows. It never supplies the product id. Mapping headers is
// a judgement call that varies per vendor; choosing a part is a lookup that must
// be reproducible.
class BomImportViewModel : public core::BaseViewModel {
public:
    explicit BomImportViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("describe",     handleDescribe)
        REGISTER_MUTATOR("parse",        handleParse)
        REGISTER_METHOD("staged",       handleStaged)
        REGISTER_MUTATOR("set_line",     handleSetLine)
        REGISTER_MUTATOR("commit",       handleCommit)
        REGISTER_MUTATOR("discard",      handleDiscard)
        REGISTER_METHOD("search_parts", handleSearchParts)
    }
    std::string modelName() const override { return "bom.import"; }

private:
    std::shared_ptr<DbConnection> db_;

    /// The vocabulary an agent needs to normalise a BOM it has never seen.
    nlohmann::json handleDescribe(const core::CallKwArgs&) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        nlohmann::json footprints = nlohmann::json::array();
        for (const auto& r : txn.exec("SELECT name FROM part_footprint ORDER BY name LIMIT 300"))
            footprints.push_back(r[0].c_str());
        nlohmann::json units = nlohmann::json::array();
        for (const auto& r : txn.exec("SELECT symbol FROM part_unit ORDER BY quantity_kind, factor"))
            units.push_back(r[0].c_str());
        return {
            {"schema_version", 1},
            {"row_fields", {
                {"designators", "Reference designators, e.g. \"C1,C2,C5\" or \"R1-R4\". Ranges are expanded."},
                {"quantity",    "Integer. Must equal the number of designators when both are given."},
                {"mpn",         "Manufacturer part number. The strongest match — supply it whenever the source has one."},
                {"manufacturer","Free text."},
                {"value",       "e.g. \"100nF\", \"4k7\", \"10uF\". SI prefixes and R-notation are both understood."},
                {"footprint",   "Package name, e.g. \"0603\"."},
                {"description", "Free text, kept as the line note."},
                {"fitted",      "false for DNP / do-not-populate lines."}}},
            {"known_footprints", footprints},
            {"known_units", units},
            {"header_aliases", {
                {"designators", {"designator","designators","reference","references","refdes","ref","part reference"}},
                {"quantity",    {"quantity","qty","count","amount"}},
                {"mpn",         {"mpn","manufacturer part number","mfr part number","part number","mfg part #","supplier part number"}},
                {"manufacturer",{"manufacturer","mfr","mfg","brand","maker"}},
                {"value",       {"value","val","comment"}},
                {"footprint",   {"footprint","package","pattern","case","pcb footprint"}},
                {"description", {"description","desc","note","notes","comment"}}}},
            {"contract",
             "Call parse with {bom_id, text} and let the server map the headers. If the "
             "layout is unrecognised, call parse with {bom_id, rows:[{...row_fields...}]} "
             "instead, or with {bom_id, text, mapping:{designators:0, quantity:2, ...}}. "
             "Never supply a product id: resolution against the catalogue is the server's "
             "job so that it is reproducible and reviewable."}
        };
    }

    // ---- header mapping ---------------------------------------------------
    struct Mapping { int designators = -1, quantity = -1, mpn = -1, manufacturer = -1,
                         value = -1, footprint = -1, description = -1; };

    static Mapping mapHeaders(const std::vector<std::string>& hdr) {
        Mapping m;
        auto match = [&](int i, const std::string& h) {
            static const std::vector<std::pair<const char*, int Mapping::*>> kAliases = {};
            (void)kAliases; (void)i; (void)h;
        };
        (void)match;
        for (size_t i = 0; i < hdr.size(); ++i) {
            const std::string h = lower(trim(hdr[i]));
            auto is = [&](std::initializer_list<const char*> alts) {
                for (const char* a : alts) if (h == a) return true;
                return false;
            };
            if (m.designators < 0 && is({"designator","designators","reference","references",
                                         "refdes","ref","part reference"}))            m.designators  = static_cast<int>(i);
            else if (m.quantity < 0 && is({"quantity","qty","count","amount"}))          m.quantity     = static_cast<int>(i);
            else if (m.mpn < 0 && is({"mpn","manufacturer part number","mfr part number",
                                      "part number","mfg part #","supplier part number"})) m.mpn       = static_cast<int>(i);
            else if (m.manufacturer < 0 && is({"manufacturer","mfr","mfg","brand","maker"})) m.manufacturer = static_cast<int>(i);
            else if (m.value < 0 && is({"value","val"}))                                 m.value        = static_cast<int>(i);
            else if (m.footprint < 0 && is({"footprint","package","pattern","case","pcb footprint"})) m.footprint = static_cast<int>(i);
            else if (m.description < 0 && is({"description","desc","note","notes","comment"})) m.description = static_cast<int>(i);
        }
        return m;
    }

    // ---- resolution -------------------------------------------------------
    struct Resolved {
        int productId = 0;
        std::string severity = "ok";     // ok | warning | error
        std::vector<std::string> issues;
        nlohmann::json candidates = nlohmann::json::array();
    };

    /// Resolve one row against the catalogue. MPN first because it is exact;
    /// value+footprint second because it is a real match but a weaker one.
    Resolved resolveRow(pqxx::work& txn, const nlohmann::json& row) {
        Resolved r;
        const std::string mpn  = trim(jstr(row, "mpn"));
        const std::string val  = trim(jstr(row, "value"));
        const std::string fp   = trim(jstr(row, "footprint"));

        auto addCandidates = [&](const pqxx::result& res) {
            for (const auto& x : res)
                r.candidates.push_back({{"id", x[0].as<int>()}, {"name", x[1].c_str()},
                                        {"code", x[2].c_str()}});
        };

        if (!mpn.empty()) {
            auto res = txn.exec(
                "SELECT pp.id, pp.name, COALESCE(pp.default_code,'') "
                "FROM part_manufacturer_info mi JOIN product_product pp ON pp.id = mi.product_id "
                "WHERE pp.active AND lower(mi.part_number) = lower($1) LIMIT 10",
                pqxx::params{mpn});
            addCandidates(res);
            if (res.size() == 1) { r.productId = res[0][0].as<int>(); return r; }
            if (res.size() > 1) {
                r.severity = "warning";
                r.issues.push_back("More than one part carries this MPN — pick one.");
                return r;
            }
        }

        if (!val.empty() && !fp.empty()) {
            // value_text is what a human typed on the part; comparing it
            // case-insensitively catches "100nF" against "100nf" without needing
            // the parameter name, which BOMs never carry.
            auto res = txn.exec(
                "SELECT DISTINCT pp.id, pp.name, COALESCE(pp.default_code,'') "
                "FROM product_product pp "
                "JOIN part_parameter pa ON pa.product_id = pp.id "
                "JOIN part_footprint f  ON f.id = pp.footprint_id "
                "WHERE pp.active AND lower(f.name) = lower($1) "
                "  AND lower(replace(COALESCE(pa.value_text,''),' ','')) = lower(replace($2,' ','')) "
                "LIMIT 10",
                pqxx::params{fp, val});
            addCandidates(res);
            if (res.size() == 1) {
                r.productId = res[0][0].as<int>();
                if (!mpn.empty()) {
                    r.severity = "warning";
                    r.issues.push_back("Matched on value and footprint; the MPN was not found.");
                }
                return r;
            }
            if (res.size() > 1) {
                r.severity = "warning";
                r.issues.push_back("Several parts share this value and footprint — pick one.");
                return r;
            }
        }

        r.severity = "error";
        if (mpn.empty() && (val.empty() || fp.empty()))
            r.issues.push_back("Not enough to identify a part: give an MPN, or a value and a footprint.");
        else
            r.issues.push_back("No part in the catalogue matches this line.");
        return r;
    }

    /// Validations that do not depend on the catalogue at all.
    static void validateRow(nlohmann::json& row, Resolved& r) {
        const auto des = splitDesignators(jstr(row, "designators"));
        int qty = jint(row, "quantity", 0);

        if (!des.empty() && qty <= 0) qty = static_cast<int>(des.size());
        row["quantity"] = qty;
        row["designator_list"] = des;
        // Store the EXPANDED form. "R1-R4" is how a BOM writes it, but the BOM
        // line has to carry the four designators it actually means — otherwise
        // the range is re-parsed by every consumer, and the count that was just
        // validated is not the count that gets written.
        if (!des.empty()) {
            std::string joined;
            for (size_t i = 0; i < des.size(); ++i) { if (i) joined += ","; joined += des[i]; }
            row["designators"] = joined;
        }

        if (qty <= 0) {
            r.severity = "error";
            r.issues.push_back("Quantity must be greater than zero.");
        }
        // The check that earns its keep. "C1,C2,C5" with quantity 4 is the most
        // common hand-edited BOM error there is, and it becomes an unpopulated
        // pad or a short order.
        if (!des.empty() && qty > 0 && static_cast<int>(des.size()) != qty) {
            r.severity = "error";
            r.issues.push_back("There are " + std::to_string(des.size()) +
                               " designators but a quantity of " + std::to_string(qty) + ".");
        }
    }

    // ---- parse ------------------------------------------------------------
    nlohmann::json handleParse(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const int bomId = jint(v, "bom_id");
        if (bomId <= 0) throw ValidationError("bom_id is required.");

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        if (txn.exec("SELECT 1 FROM mrp_bom WHERE id=$1", pqxx::params{bomId}).empty())
            throw ValidationError("No such BOM.");

        // Rows may arrive already normalised (from an agent) or as raw text.
        std::vector<nlohmann::json> rows;
        if (v.contains("rows") && v["rows"].is_array()) {
            for (const auto& r : v["rows"]) if (r.is_object()) rows.push_back(r);
        } else {
            const std::string text = jstr(v, "text");
            if (trim(text).empty()) throw ValidationError("Nothing to parse — give text or rows.");

            std::vector<std::string> lines;
            { std::istringstream is(text); std::string ln;
              while (std::getline(is, ln)) { if (!trim(ln).empty()) lines.push_back(ln); } }
            if (lines.empty()) throw ValidationError("Nothing to parse.");

            Mapping m;
            size_t first = 0;
            if (v.contains("mapping") && v["mapping"].is_object()) {
                const auto& mp = v["mapping"];
                auto g = [&](const char* k) { return mp.contains(k) && mp[k].is_number_integer()
                                                   ? mp[k].get<int>() : -1; };
                m.designators = g("designators"); m.quantity = g("quantity");
                m.mpn = g("mpn"); m.manufacturer = g("manufacturer");
                m.value = g("value"); m.footprint = g("footprint");
                m.description = g("description");
                if (v.contains("skip_header") && v["skip_header"].is_boolean()
                    && v["skip_header"].get<bool>()) first = 1;
            } else {
                m = mapHeaders(csvLine(lines[0]));
                first = 1;
                if (m.designators < 0 && m.mpn < 0 && m.value < 0)
                    throw ValidationError(
                        "The header row was not recognised. Supply a mapping, or send "
                        "normalised rows — call describe for the column vocabulary.");
            }

            for (size_t i = first; i < lines.size(); ++i) {
                const auto c = csvLine(lines[i]);
                auto at = [&](int idx) -> std::string {
                    return (idx >= 0 && idx < static_cast<int>(c.size())) ? trim(c[idx]) : std::string{};
                };
                nlohmann::json row;
                row["designators"]  = at(m.designators);
                row["mpn"]          = at(m.mpn);
                row["manufacturer"] = at(m.manufacturer);
                row["value"]        = at(m.value);
                row["footprint"]    = at(m.footprint);
                row["description"]  = at(m.description);
                const std::string q = at(m.quantity);
                row["quantity"] = q.empty() ? 0 : (int)[&]{ try { return std::stoi(q); } catch (...) { return 0; } }();
                if (row["designators"].get<std::string>().empty() &&
                    row["mpn"].get<std::string>().empty() &&
                    row["value"].get<std::string>().empty()) continue;   // blank line
                rows.push_back(row);
            }
        }
        if (rows.empty()) throw ValidationError("No usable rows were found.");

        // Replace any previous staging for this BOM — an import is a fresh attempt.
        txn.exec("DELETE FROM mrp_bom_import_line WHERE bom_id=$1", pqxx::params{bomId});

        std::map<std::string, int> seenDesignators;
        int seq = 10;
        for (auto& row : rows) {
            Resolved r;
            validateRow(row, r);
            if (r.severity != "error" || r.issues.empty()) {
                Resolved rr = resolveRow(txn, row);
                if (rr.severity == "error" || r.severity != "error") {
                    // keep the worse severity and merge the issues
                    if (rr.severity == "error") r.severity = "error";
                    else if (rr.severity == "warning" && r.severity == "ok") r.severity = "warning";
                    for (auto& i : rr.issues) r.issues.push_back(i);
                    r.productId  = rr.productId;
                    r.candidates = rr.candidates;
                }
            }
            // A designator may appear once across the whole board.
            for (const auto& d : row["designator_list"]) {
                const std::string key = lower(d.get<std::string>());
                if (++seenDesignators[key] > 1) {
                    r.severity = "error";
                    r.issues.push_back("Designator " + d.get<std::string>() + " appears more than once.");
                    break;
                }
            }

            nlohmann::json issues = nlohmann::json::array();
            for (const auto& i : r.issues) issues.push_back(i);

            txn.exec(
                "INSERT INTO mrp_bom_import_line "
                "(bom_id, sequence, designators, quantity, mpn, manufacturer, value_text, "
                " footprint, description, product_id, severity, issues, candidates, fitted) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,NULLIF($10,0),$11,$12,$13,TRUE)",
                pqxx::params{bomId, seq,
                             jstr(row, "designators"), row["quantity"].get<int>(),
                             jstr(row, "mpn"), jstr(row, "manufacturer"),
                             jstr(row, "value"), jstr(row, "footprint"),
                             jstr(row, "description"), r.productId,
                             r.severity, issues.dump(), r.candidates.dump()});
            seq += 10;
        }
        txn.commit();
        return staged(bomId);
    }

    nlohmann::json handleStaged(const core::CallKwArgs& call) {
        return staged(jint(call.arg(0), "bom_id"));
    }

    nlohmann::json staged(int bomId) {
        if (bomId <= 0) throw ValidationError("bom_id is required.");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        nlohmann::json rows = nlohmann::json::array();
        int ok = 0, warn = 0, err = 0;
        for (const auto& r : txn.exec(
                "SELECT l.id, l.sequence, l.designators, l.quantity, l.mpn, l.manufacturer, "
                // AS product_id matters: without an alias the result column is
                // named "coalesce" and reading it by name throws.
                "       l.value_text, l.footprint, l.description, "
                "       COALESCE(l.product_id,0) AS product_id, "
                "       l.severity, l.issues, l.candidates, l.fitted, "
                "       COALESCE(p.name,'') AS product_name, COALESCE(p.default_code,'') AS product_code "
                "FROM mrp_bom_import_line l "
                "LEFT JOIN product_product p ON p.id = l.product_id "
                "WHERE l.bom_id=$1 ORDER BY l.sequence, l.id", pqxx::params{bomId})) {
            const std::string sev = r["severity"].c_str();
            if (sev == "error") ++err; else if (sev == "warning") ++warn; else ++ok;
            rows.push_back({
                {"id", r["id"].as<int>()}, {"sequence", r["sequence"].as<int>(0)},
                {"designators", r["designators"].c_str()}, {"quantity", r["quantity"].as<int>(0)},
                {"mpn", r["mpn"].c_str()}, {"manufacturer", r["manufacturer"].c_str()},
                {"value", r["value_text"].c_str()}, {"footprint", r["footprint"].c_str()},
                {"description", r["description"].c_str()},
                {"product_id", r["product_id"].as<int>(0)},
                {"product_name", r["product_name"].c_str()},
                {"product_code", r["product_code"].c_str()},
                {"severity", sev}, {"fitted", r["fitted"].as<bool>(true)},
                {"issues", nlohmann::json::parse(r["issues"].c_str(), nullptr, false)},
                {"candidates", nlohmann::json::parse(r["candidates"].c_str(), nullptr, false)}});
        }
        return {{"bom_id", bomId}, {"rows", rows},
                {"counts", {{"ok", ok}, {"warning", warn}, {"error", err},
                            {"total", ok + warn + err}}}};
    }

    /// Correct one staged line — pick a candidate, fix a quantity, mark DNP.
    /// Re-validates and re-resolves, so a fix cannot leave a stale status behind.
    nlohmann::json handleSetLine(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const int id = jint(v, "id");
        if (id <= 0) throw ValidationError("id is required.");

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto cur = txn.exec(
            "SELECT bom_id, designators, quantity, mpn, manufacturer, value_text, footprint, "
            "       description, COALESCE(product_id,0), fitted "
            "FROM mrp_bom_import_line WHERE id=$1", pqxx::params{id});
        if (cur.empty()) throw ValidationError("No such staged line.");

        nlohmann::json row;
        row["designators"] = v.contains("designators") ? jstr(v, "designators") : std::string(cur[0][1].c_str());
        row["quantity"]    = v.contains("quantity")    ? jint(v, "quantity")    : cur[0][2].as<int>(0);
        row["mpn"]         = v.contains("mpn")         ? jstr(v, "mpn")         : std::string(cur[0][3].c_str());
        row["manufacturer"]= v.contains("manufacturer")? jstr(v, "manufacturer"): std::string(cur[0][4].c_str());
        row["value"]       = v.contains("value")       ? jstr(v, "value")       : std::string(cur[0][5].c_str());
        row["footprint"]   = v.contains("footprint")   ? jstr(v, "footprint")   : std::string(cur[0][6].c_str());
        row["description"] = v.contains("description") ? jstr(v, "description") : std::string(cur[0][7].c_str());
        const bool fitted  = v.contains("fitted") && v["fitted"].is_boolean()
                           ? v["fitted"].get<bool>() : cur[0][9].as<bool>(true);

        Resolved r;
        validateRow(row, r);

        // An explicitly chosen product is taken as given — the user has resolved
        // the ambiguity the importer could not.
        int chosen = jint(v, "product_id", -1);
        if (chosen >= 0) {
            if (chosen > 0 && txn.exec("SELECT 1 FROM product_product WHERE id=$1 AND active",
                                       pqxx::params{chosen}).empty())
                throw ValidationError("No such product.");
            r.productId = chosen;
            if (chosen == 0 && r.severity != "error") {
                r.severity = "error";
                r.issues.push_back("No part chosen for this line.");
            }
        } else {
            Resolved rr = resolveRow(txn, row);
            if (rr.severity == "error") r.severity = "error";
            else if (rr.severity == "warning" && r.severity == "ok") r.severity = "warning";
            for (auto& i : rr.issues) r.issues.push_back(i);
            r.productId  = rr.productId;
            r.candidates = rr.candidates;
        }

        nlohmann::json issues = nlohmann::json::array();
        for (const auto& i : r.issues) issues.push_back(i);
        txn.exec(
            "UPDATE mrp_bom_import_line SET designators=$1, quantity=$2, mpn=$3, manufacturer=$4, "
            "  value_text=$5, footprint=$6, description=$7, product_id=NULLIF($8,0), "
            "  severity=$9, issues=$10, candidates=$11, fitted=$12, write_date=now() WHERE id=$13",
            pqxx::params{row["designators"].get<std::string>(), row["quantity"].get<int>(),
                         row["mpn"].get<std::string>(), row["manufacturer"].get<std::string>(),
                         row["value"].get<std::string>(), row["footprint"].get<std::string>(),
                         row["description"].get<std::string>(), r.productId,
                         r.severity, issues.dump(), r.candidates.dump(), fitted, id});
        const int bomId = cur[0][0].as<int>();
        txn.commit();
        return staged(bomId);
    }

    /// Commit the staged lines into the BOM. Refuses while any line is in error:
    /// a half-imported BOM is worse than none, because it looks complete.
    nlohmann::json handleCommit(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const int bomId = jint(v, "bom_id");
        if (bomId <= 0) throw ValidationError("bom_id is required.");
        const bool replace = !v.contains("replace") || !v["replace"].is_boolean()
                           || v["replace"].get<bool>();

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        const long errs = txn.exec(
            "SELECT count(*) FROM mrp_bom_import_line WHERE bom_id=$1 AND severity='error'",
            pqxx::params{bomId})[0][0].as<long>(0);
        if (errs > 0)
            throw ValidationError(std::to_string(errs) +
                " line(s) still have errors. Fix or remove them before importing.");

        auto rows = txn.exec(
            "SELECT product_id, quantity, designators, description, fitted, sequence "
            "FROM mrp_bom_import_line WHERE bom_id=$1 AND product_id IS NOT NULL "
            "ORDER BY sequence, id", pqxx::params{bomId});
        if (rows.empty()) throw ValidationError("There is nothing staged to import.");

        if (replace) txn.exec("DELETE FROM mrp_bom_line WHERE bom_id=$1", pqxx::params{bomId});

        int uom = 1;
        { auto u = txn.exec("SELECT id FROM uom_uom ORDER BY id LIMIT 1");
          if (!u.empty()) uom = u[0][0].as<int>(); }

        int n = 0;
        for (const auto& r : rows) {
            txn.exec(
                "INSERT INTO mrp_bom_line (bom_id, product_id, product_qty, product_uom_id, "
                "  sequence, reference_designators, note, fitted) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8)",
                pqxx::params{bomId, r[0].as<int>(), r[1].as<int>(0), uom,
                             r[5].as<int>(0), r[2].c_str(), r[3].c_str(), r[4].as<bool>(true)});
            ++n;
        }
        txn.exec("DELETE FROM mrp_bom_import_line WHERE bom_id=$1", pqxx::params{bomId});
        txn.commit();
        LOG_INFO << "[bom] imported " << n << " line(s) into BOM " << bomId;
        return {{"ok", true}, {"lines", n}, {"replaced", replace}};
    }

    nlohmann::json handleDiscard(const core::CallKwArgs& call) {
        const int bomId = jint(call.arg(0), "bom_id");
        if (bomId <= 0) throw ValidationError("bom_id is required.");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec("DELETE FROM mrp_bom_import_line WHERE bom_id=$1", pqxx::params{bomId});
        txn.commit();
        return {{"ok", true}};
    }

    /// Free-text part search, for the picker on an unresolved line.
    nlohmann::json handleSearchParts(const core::CallKwArgs& call) {
        const std::string q = trim(jstr(call.arg(0), "q"));
        if (q.size() < 2) return nlohmann::json::array();
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : txn.exec(
                "SELECT DISTINCT pp.id, pp.name, COALESCE(pp.default_code,''), "
                "       COALESCE(f.name,'') "
                "FROM product_product pp "
                "LEFT JOIN part_footprint f ON f.id = pp.footprint_id "
                "LEFT JOIN part_manufacturer_info mi ON mi.product_id = pp.id "
                "WHERE pp.active AND (pp.name ILIKE $1 OR COALESCE(pp.default_code,'') ILIKE $1 "
                "   OR COALESCE(mi.part_number,'') ILIKE $1) "
                "ORDER BY pp.name LIMIT 25", pqxx::params{"%" + q + "%"}))
            arr.push_back({{"id", r[0].as<int>()}, {"name", r[1].c_str()},
                           {"code", r[2].c_str()}, {"footprint", r[3].c_str()}});
        return arr;
    }
};

// ================================================================
// bom.editor — the BOM and its committed lines
// ================================================================
class BomEditorViewModel : public core::BaseViewModel {
public:
    explicit BomEditorViewModel(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("boms",      handleBoms)
        REGISTER_METHOD("lines",     handleLines)
        REGISTER_MUTATOR("set_line",  handleSetLine)
        REGISTER_MUTATOR("add_line",  handleAddLine)
        REGISTER_MUTATOR("del_line",  handleDelLine)
        REGISTER_MUTATOR("create_bom", handleCreateBom)
    }
    std::string modelName() const override { return "bom.editor"; }

private:
    std::shared_ptr<DbConnection> db_;

    nlohmann::json handleBoms(const core::CallKwArgs&) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : txn.exec(
                "SELECT b.id, COALESCE(b.code,''), COALESCE(b.bom_kind,'general'), "
                "       COALESCE(b.revision,''), COALESCE(p.name,''), b.bom_type, "
                "       (SELECT count(*) FROM mrp_bom_line l WHERE l.bom_id=b.id) "
                "FROM mrp_bom b LEFT JOIN product_product p ON p.id = b.product_id "
                "WHERE b.active ORDER BY b.id DESC LIMIT 200"))
            arr.push_back({{"id", r[0].as<int>()}, {"code", r[1].c_str()},
                           {"kind", r[2].c_str()}, {"revision", r[3].c_str()},
                           {"product_name", r[4].c_str()}, {"bom_type", r[5].c_str()},
                           {"line_count", r[6].as<long>(0)}});
        return arr;
    }

    nlohmann::json handleLines(const core::CallKwArgs& call) {
        const int bomId = jint(call.arg(0), "bom_id");
        if (bomId <= 0) throw ValidationError("bom_id is required.");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        auto b = txn.exec(
            "SELECT b.id, COALESCE(b.code,''), COALESCE(b.bom_kind,'general'), "
            "       COALESCE(b.revision,''), b.bom_type, COALESCE(p.name,'') "
            "FROM mrp_bom b LEFT JOIN product_product p ON p.id=b.product_id WHERE b.id=$1",
            pqxx::params{bomId});
        if (b.empty()) throw ValidationError("No such BOM.");
        const std::string kind = b[0][2].c_str();

        nlohmann::json rows = nlohmann::json::array();
        std::map<std::string, int> seen;
        for (const auto& r : txn.exec(
                "SELECT l.id, l.sequence, l.product_id, COALESCE(p.name,''), "
                "       COALESCE(p.default_code,''), l.product_qty, "
                "       COALESCE(l.reference_designators,''), COALESCE(l.note,''), "
                "       COALESCE(l.fitted,TRUE), COALESCE(f.name,'') "
                "FROM mrp_bom_line l "
                "LEFT JOIN product_product p ON p.id=l.product_id "
                "LEFT JOIN part_footprint f ON f.id=p.footprint_id "
                "WHERE l.bom_id=$1 ORDER BY l.sequence, l.id", pqxx::params{bomId})) {
            const std::string des = r[6].c_str();
            const int qty = static_cast<int>(r[5].as<double>(0));
            const auto list = splitDesignators(des);

            // The editor re-checks the same things the importer does, because a
            // line typed by hand can be just as wrong as one imported.
            std::string sev = "ok";
            nlohmann::json issues = nlohmann::json::array();
            if (r[2].is_null())          { sev = "error"; issues.push_back("No part chosen."); }
            if (qty <= 0)                { sev = "error"; issues.push_back("Quantity must be greater than zero."); }
            if (kind == "pcba" && des.empty()) {
                if (sev != "error") sev = "warning";
                issues.push_back("A PCBA line should carry reference designators.");
            }
            if (!list.empty() && qty > 0 && static_cast<int>(list.size()) != qty) {
                sev = "error";
                issues.push_back("There are " + std::to_string(list.size()) +
                                 " designators but a quantity of " + std::to_string(qty) + ".");
            }
            for (const auto& d : list) {
                if (++seen[lower(d)] > 1) {
                    sev = "error";
                    issues.push_back("Designator " + d + " appears more than once in this BOM.");
                    break;
                }
            }
            rows.push_back({{"id", r[0].as<int>()}, {"sequence", r[1].as<int>(0)},
                            {"product_id", r[2].is_null() ? 0 : r[2].as<int>(0)},
                            {"product_name", r[3].c_str()}, {"product_code", r[4].c_str()},
                            {"quantity", qty}, {"designators", des}, {"note", r[7].c_str()},
                            {"fitted", r[8].as<bool>(true)}, {"footprint", r[9].c_str()},
                            {"severity", sev}, {"issues", issues}});
        }
        return {{"bom", {{"id", b[0][0].as<int>()}, {"code", b[0][1].c_str()},
                         {"kind", kind}, {"revision", b[0][3].c_str()},
                         {"bom_type", b[0][4].c_str()}, {"product_name", b[0][5].c_str()}}},
                {"lines", rows}};
    }

    nlohmann::json handleAddLine(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const int bomId = jint(v, "bom_id");
        if (bomId <= 0) throw ValidationError("bom_id is required.");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        int uom = 1;
        { auto u = txn.exec("SELECT id FROM uom_uom ORDER BY id LIMIT 1");
          if (!u.empty()) uom = u[0][0].as<int>(); }
        const int seq = txn.exec("SELECT COALESCE(max(sequence),0)+10 FROM mrp_bom_line WHERE bom_id=$1",
                                 pqxx::params{bomId})[0][0].as<int>(10);
        txn.exec("INSERT INTO mrp_bom_line (bom_id, product_id, product_qty, product_uom_id, "
                 " sequence, reference_designators, note, fitted) "
                 "VALUES ($1, NULLIF($2,0), $3, $4, $5, $6, $7, TRUE)",
                 pqxx::params{bomId, jint(v, "product_id"), jint(v, "quantity", 1), uom, seq,
                              jstr(v, "designators"), jstr(v, "note")});
        txn.commit();
        return {{"ok", true}};
    }

    nlohmann::json handleSetLine(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const int id = jint(v, "id");
        if (id <= 0) throw ValidationError("id is required.");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto cur = txn.exec("SELECT bom_id FROM mrp_bom_line WHERE id=$1", pqxx::params{id});
        if (cur.empty()) throw ValidationError("No such line.");

        if (v.contains("product_id")) {
            const int pid = jint(v, "product_id");
            if (pid > 0 && txn.exec("SELECT 1 FROM product_product WHERE id=$1 AND active",
                                    pqxx::params{pid}).empty())
                throw ValidationError("No such product.");
            txn.exec("UPDATE mrp_bom_line SET product_id=NULLIF($1,0) WHERE id=$2",
                     pqxx::params{pid, id});
        }
        if (v.contains("quantity"))
            txn.exec("UPDATE mrp_bom_line SET product_qty=$1 WHERE id=$2",
                     pqxx::params{jint(v, "quantity", 1), id});
        if (v.contains("designators"))
            txn.exec("UPDATE mrp_bom_line SET reference_designators=$1 WHERE id=$2",
                     pqxx::params{jstr(v, "designators"), id});
        if (v.contains("note"))
            txn.exec("UPDATE mrp_bom_line SET note=$1 WHERE id=$2", pqxx::params{jstr(v, "note"), id});
        if (v.contains("fitted") && v["fitted"].is_boolean())
            txn.exec("UPDATE mrp_bom_line SET fitted=$1 WHERE id=$2",
                     pqxx::params{v["fitted"].get<bool>(), id});
        txn.commit();
        return {{"ok", true}};
    }

    nlohmann::json handleDelLine(const core::CallKwArgs& call) {
        const int id = jint(call.arg(0), "id");
        if (id <= 0) throw ValidationError("id is required.");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec("DELETE FROM mrp_bom_line WHERE id=$1", pqxx::params{id});
        txn.commit();
        return {{"ok", true}};
    }

    nlohmann::json handleCreateBom(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const int productId = jint(v, "product_id");
        if (productId <= 0) throw ValidationError("product_id is required.");
        std::string kind = jstr(v, "kind");
        static const std::set<std::string> kKinds = {"pcba","mechanical","kit","general"};
        if (!kKinds.count(kind)) kind = "general";
        // docs/105 §5b — a kit is packed, never manufactured, so it is always a
        // phantom BOM. Letting the two disagree would reserve components to make
        // something that does not physically exist.
        const std::string bomType = (kind == "kit") ? "phantom" : "normal";

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        if (txn.exec("SELECT 1 FROM product_product WHERE id=$1 AND active",
                     pqxx::params{productId}).empty())
            throw ValidationError("No such product.");
        int uom = 1;
        { auto u = txn.exec("SELECT id FROM uom_uom ORDER BY id LIMIT 1");
          if (!u.empty()) uom = u[0][0].as<int>(); }
        auto ins = txn.exec(
            "INSERT INTO mrp_bom (product_id, code, bom_type, product_qty, product_uom_id, "
            "  active, bom_kind, revision) VALUES ($1,$2,$3,1,$4,TRUE,$5,$6) RETURNING id",
            pqxx::params{productId, jstr(v, "code"), bomType, uom, kind,
                         v.contains("revision") ? jstr(v, "revision") : std::string("A")});
        const int id = ins[0][0].as<int>();
        txn.commit();
        return {{"ok", true}, {"id", id}, {"bom_type", bomType}, {"kind", kind}};
    }
};

// ================================================================
// MODULE
// ================================================================
BomModule::BomModule(core::ModelFactory& m, core::ServiceFactory& s,
                     core::ViewModelFactory& vm, core::ViewFactory& v)
    : models_(m), services_(s), viewModels_(vm), views_(v) {}

std::string              BomModule::moduleName()   const { return "bom"; }
std::string              BomModule::version()      const { return "1.0"; }
std::vector<std::string> BomModule::dependencies() const { return {"mrp", "product"}; }

void BomModule::registerModels()     {}
void BomModule::registerServices()   {}
void BomModule::registerViews()      {}
void BomModule::registerRoutes()     {}

void BomModule::registerViewModels() {
    auto db = services_.db();
    viewModels_.registerCreator("bom.import", [db]{ return std::make_shared<BomImportViewModel>(db); });
    viewModels_.registerCreator("bom.editor", [db]{ return std::make_shared<BomEditorViewModel>(db); });
}

void BomModule::initialize() { ensureSchema_(); seedMenus_(); }

void BomModule::ensureSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    // docs/105 Phase 1 — what a PCBA BOM needs beyond a quantity.
    txn.exec("ALTER TABLE mrp_bom ADD COLUMN IF NOT EXISTS bom_kind VARCHAR NOT NULL DEFAULT 'general'");
    txn.exec("ALTER TABLE mrp_bom ADD COLUMN IF NOT EXISTS revision VARCHAR");
    txn.exec("ALTER TABLE mrp_bom ADD COLUMN IF NOT EXISTS revision_of_id INTEGER "
             "REFERENCES mrp_bom(id) ON DELETE SET NULL");
    txn.exec("ALTER TABLE mrp_bom_line ADD COLUMN IF NOT EXISTS reference_designators TEXT");
    txn.exec("ALTER TABLE mrp_bom_line ADD COLUMN IF NOT EXISTS note TEXT");
    txn.exec("ALTER TABLE mrp_bom_line ADD COLUMN IF NOT EXISTS fitted BOOLEAN NOT NULL DEFAULT TRUE");

    // The staging table. Deliberately separate from mrp_bom_line: nothing an
    // importer produces reaches the BOM until a person commits it, exactly as
    // part_lookup_result keeps agent proposals out of the catalogue.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS mrp_bom_import_line (
            id           SERIAL PRIMARY KEY,
            bom_id       INTEGER NOT NULL REFERENCES mrp_bom(id) ON DELETE CASCADE,
            sequence     INTEGER NOT NULL DEFAULT 10,
            designators  TEXT,
            quantity     INTEGER NOT NULL DEFAULT 0,
            mpn          VARCHAR,
            manufacturer VARCHAR,
            value_text   VARCHAR,
            footprint    VARCHAR,
            description  TEXT,
            product_id   INTEGER REFERENCES product_product(id) ON DELETE SET NULL,
            severity     VARCHAR NOT NULL DEFAULT 'ok',
            issues       TEXT NOT NULL DEFAULT '[]',
            candidates   TEXT NOT NULL DEFAULT '[]',
            fitted       BOOLEAN NOT NULL DEFAULT TRUE,
            create_date  TIMESTAMP DEFAULT now(),
            write_date   TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_bom_import_bom ON mrp_bom_import_line (bom_id, sequence)");
    txn.commit();
}

void BomModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, path, context) VALUES
            (116, 'BOM Editor', 'bom.editor', 'list', 'bom-editor', '{}')
        ON CONFLICT (id) DO UPDATE SET name='BOM Editor', res_model='bom.editor',
            view_mode='list', path='bom-editor', domain=NULL
    )");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (150, 'BOM Editor', 110, 5, 116)
        ON CONFLICT (id) DO UPDATE SET name='BOM Editor', parent_id=110,
            sequence=5, action_id=116
    )");
    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");
    txn.commit();
}

} // namespace odoo::modules::bom

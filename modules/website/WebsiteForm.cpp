// =============================================================
// modules/website/WebsiteForm.cpp — implementation (docs/116 A1/A2)
// =============================================================
#include "WebsiteForm.hpp"
#include "WebsiteRender.hpp"
#include "BaseModel.hpp"
#include "GenericViewModel.hpp"
#include "Factories.hpp"
#include "DbConnection.hpp"
#include "ClientIp.hpp"
#include "Errors.hpp"
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <chrono>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

namespace cerp::modules::website {

using namespace cerp::infrastructure;
using namespace cerp::core;
using R = WebsiteRender;

namespace {

inline int fM2oId(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer()) return v[0].get<int>();
    return 0;
}
inline std::string sOr(const pqxx::field& f) {
    return f.is_null() ? std::string{} : std::string(f.c_str());
}

// One submission is at most this much text in total, and one field at most
// this much. An unauthenticated write endpoint needs a ceiling it does not
// take from the caller.
constexpr std::size_t kMaxFieldLen = 4000;
constexpr std::size_t kMaxTotalLen = 20000;

// Rate limiter for the public POST. Same shape as the kiosk's (docs/113):
// the state lives in the process, keyed on IP.
class SubmitLimiter {
public:
    // Every attempt counts, including ones rejected for a missing required
    // field — an attacker would otherwise flood with invalid bodies for free.
    // That makes the ceiling a UX number as much as a security one: a whole
    // office behind one NAT address shares this budget, and somebody
    // correcting a validation error spends it. 20 in five minutes is high
    // enough not to bite a real group of people and low enough that scripted
    // abuse stops being worthwhile.
    static constexpr int kMax = 20;
    static constexpr int kWindow = 300;
    bool allow(const std::string& ip) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lk(m_);
        auto& e = t_[ip];
        if ((now - e.start) >= std::chrono::seconds(kWindow)) { e.start = now; e.count = 0; }
        if (e.count >= kMax) return false;
        ++e.count;
        return true;
    }
private:
    using Clock = std::chrono::steady_clock;
    struct E { Clock::time_point start = Clock::now(); int count = 0; };
    std::mutex m_;
    std::unordered_map<std::string, E> t_;
};
std::shared_ptr<SubmitLimiter> g_limiter = std::make_shared<SubmitLimiter>();

} // anonymous namespace

bool WebsiteForm::isValidFieldType(const std::string& t) {
    static const std::set<std::string> ok = {
        "text","email","tel","textarea","number","select","checkbox"
    };
    return ok.count(t) != 0;
}

// ================================================================
// MODELS
// ================================================================
class WebForm : public BaseModel<WebForm> {
public:
    static constexpr const char* MODEL_NAME = "website.form";
    static constexpr const char* TABLE_NAME = "website_form";
    explicit WebForm(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    std::string slug, title, description, submitLabel, successMessage, targetModel;
    bool active = true;

    void registerFields() {
        fieldRegistry_.add({"slug",            FieldType::Char,     "Slug", true});
        fieldRegistry_.add({"title",           FieldType::Char,     "Title", true});
        fieldRegistry_.add({"description",     FieldType::Text,     "Intro text"});
        fieldRegistry_.add({"submit_label",    FieldType::Char,     "Button label"});
        fieldRegistry_.add({"success_message", FieldType::Text,     "Thank-you message"});
        fieldRegistry_.add({"target_model",    FieldType::Selection,"Also create"});
        fieldRegistry_.add({"active",          FieldType::Boolean,  "Active"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["slug"] = slug; j["title"] = title;
        j["description"]     = description.empty()    ? nlohmann::json(false) : nlohmann::json(description);
        j["submit_label"]    = submitLabel.empty()    ? nlohmann::json(false) : nlohmann::json(submitLabel);
        j["success_message"] = successMessage.empty() ? nlohmann::json(false) : nlohmann::json(successMessage);
        j["target_model"]    = targetModel.empty()    ? nlohmann::json(false) : nlohmann::json(targetModel);
        j["active"] = active;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("slug")  && j["slug"].is_string())  slug  = j["slug"].get<std::string>();
        if (j.contains("title") && j["title"].is_string()) title = j["title"].get<std::string>();
        if (j.contains("description")     && j["description"].is_string())     description    = j["description"].get<std::string>();
        if (j.contains("submit_label")    && j["submit_label"].is_string())    submitLabel    = j["submit_label"].get<std::string>();
        if (j.contains("success_message") && j["success_message"].is_string()) successMessage = j["success_message"].get<std::string>();
        if (j.contains("target_model")    && j["target_model"].is_string())    targetModel    = j["target_model"].get<std::string>();
        if (j.contains("active") && j["active"].is_boolean()) active = j["active"].get<bool>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = title; return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (title.empty()) e.push_back("A form title is required");
        if (slug.empty())  e.push_back("A form slug is required");
        else if (!R::isValidSlug(slug))
            e.push_back("The form slug may contain only lowercase letters, "
                        "digits, hyphens and slashes");
        // Where a submission may ALSO be routed. An allow-list, because this
        // value chooses a code path that writes to another table.
        if (!targetModel.empty() && targetModel != "none" && targetModel != "project.task")
            e.push_back("Unsupported target model");
        return e;
    }
};

class WebFormField : public BaseModel<WebFormField> {
public:
    static constexpr const char* MODEL_NAME = "website.form.field";
    static constexpr const char* TABLE_NAME = "website_form_field";
    explicit WebFormField(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    int formId = 0, sequence = 10;
    std::string name, label, fieldType = "text", placeholder, options;
    bool required = false;

    void registerFields() {
        fieldRegistry_.add({"form_id",     FieldType::Many2one, "Form", true, false, true, false, "website.form"});
        fieldRegistry_.add({"name",        FieldType::Char,     "Field name", true});
        fieldRegistry_.add({"label",       FieldType::Char,     "Label", true});
        fieldRegistry_.add({"field_type",  FieldType::Selection,"Type"});
        fieldRegistry_.add({"placeholder", FieldType::Char,     "Placeholder"});
        fieldRegistry_.add({"options",     FieldType::Text,     "Choices (one per line)"});
        fieldRegistry_.add({"required",    FieldType::Boolean,  "Required"});
        fieldRegistry_.add({"sequence",    FieldType::Integer,  "Sequence"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["form_id"]     = formId > 0 ? nlohmann::json::array({formId, ""}) : nlohmann::json(false);
        j["name"] = name; j["label"] = label; j["field_type"] = fieldType;
        j["placeholder"] = placeholder.empty() ? nlohmann::json(false) : nlohmann::json(placeholder);
        j["options"]     = options.empty()     ? nlohmann::json(false) : nlohmann::json(options);
        j["required"]    = required;
        j["sequence"]    = sequence;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("form_id")) formId = fM2oId(j["form_id"]);
        if (j.contains("name")  && j["name"].is_string())  name  = j["name"].get<std::string>();
        if (j.contains("label") && j["label"].is_string()) label = j["label"].get<std::string>();
        if (j.contains("field_type")  && j["field_type"].is_string())  fieldType   = j["field_type"].get<std::string>();
        if (j.contains("placeholder") && j["placeholder"].is_string()) placeholder = j["placeholder"].get<std::string>();
        if (j.contains("options")     && j["options"].is_string())     options     = j["options"].get<std::string>();
        if (j.contains("required") && j["required"].is_boolean()) required = j["required"].get<bool>();
        if (j.contains("sequence") && j["sequence"].is_number_integer()) sequence = j["sequence"].get<int>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = label; return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (formId <= 0)   e.push_back("A form is required");
        if (label.empty()) e.push_back("A field label is required");
        if (name.empty())  e.push_back("A field name is required");
        else {
            // The field name is the key in the submitted body and in the
            // stored JSON. Constrain it so it can never collide with the
            // control keys the submit route uses, or look like anything else.
            bool ok = name.size() <= 40;
            for (char c : name)
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) ok = false;
            if (!ok) e.push_back("A field name may contain only lowercase letters, "
                                 "digits and underscores (max 40)");
            if (name == "website" || name == "_form")
                e.push_back("That field name is reserved");
        }
        if (!WebsiteForm::isValidFieldType(fieldType))
            e.push_back("Unsupported field type");
        return e;
    }
};

class WebFormSubmission : public BaseModel<WebFormSubmission> {
public:
    static constexpr const char* MODEL_NAME = "website.form.submission";
    static constexpr const char* TABLE_NAME = "website_form_submission";
    explicit WebFormSubmission(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    int formId = 0, taskId = 0;
    std::string dataJson, state = "new", sourceIp;

    void registerFields() {
        fieldRegistry_.add({"form_id",   FieldType::Many2one, "Form", false, false, true, false, "website.form"});
        fieldRegistry_.add({"data_json", FieldType::Text,     "Submitted data"});
        fieldRegistry_.add({"state",     FieldType::Selection,"Status"});
        fieldRegistry_.add({"source_ip", FieldType::Char,     "Source IP"});
        fieldRegistry_.add({"task_id",   FieldType::Many2one, "Task", false, false, true, false, "project.task"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["form_id"]   = formId > 0 ? nlohmann::json::array({formId, ""}) : nlohmann::json(false);
        j["data_json"] = dataJson.empty() ? nlohmann::json("{}") : nlohmann::json(dataJson);
        j["state"]     = state;
        j["source_ip"] = sourceIp.empty() ? nlohmann::json(false) : nlohmann::json(sourceIp);
        j["task_id"]   = taskId > 0 ? nlohmann::json::array({taskId, ""}) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        // data_json, source_ip and form_id are written by the public route
        // only. A back-office user may move the state and nothing else.
        if (j.contains("state") && j["state"].is_string()) {
            const std::string s = j["state"].get<std::string>();
            if (s == "new" || s == "read" || s == "archived") state = s;
        }
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = "Submission " + std::to_string(getId());
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override { return {}; }
};

// ================================================================
// RENDERING
// ================================================================
std::string WebsiteForm::renderForm(pqxx::transaction_base& txn,
                                    const std::string& slug)
{
    if (!R::isValidSlug(slug)) return {};
    auto f = txn.exec(
        "SELECT id, title, description, submit_label FROM website_form "
        " WHERE slug=$1 AND active = TRUE LIMIT 1",
        pqxx::params{slug});
    if (f.empty()) return {};
    const int formId = f[0]["id"].as<int>();

    auto fields = txn.exec(
        "SELECT name, label, field_type, placeholder, options, required "
        "  FROM website_form_field WHERE form_id=$1 ORDER BY sequence, id",
        pqxx::params{formId});

    std::ostringstream h;
    h << "<form class=\"w-form\" method=\"post\" action=\"/site/form/"
      << R::esc(slug) << "\" data-slug=\"" << R::esc(slug) << "\">";
    const std::string desc = sOr(f[0]["description"]);
    if (!desc.empty()) h << "<p class=\"w-p\">" << R::esc(desc) << "</p>";

    for (const auto& fl : fields) {
        const std::string nm  = sOr(fl["name"]);
        const std::string lbl = sOr(fl["label"]);
        const std::string ty  = sOr(fl["field_type"]);
        const std::string ph  = sOr(fl["placeholder"]);
        const bool req = !fl["required"].is_null() && fl["required"].as<bool>(false);
        const std::string reqAttr = req ? " required=\"required\"" : "";

        h << "<label class=\"w-field\"><span class=\"w-label\">" << R::esc(lbl)
          << (req ? " *" : "") << "</span>";
        if (ty == "textarea") {
            h << "<textarea name=\"" << R::esc(nm) << "\" rows=\"5\" placeholder=\""
              << R::esc(ph) << "\"" << reqAttr << "></textarea>";
        } else if (ty == "select") {
            h << "<select name=\"" << R::esc(nm) << "\"" << reqAttr << ">";
            h << "<option value=\"\">--</option>";
            std::istringstream is(sOr(fl["options"]));
            std::string line;
            while (std::getline(is, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                h << "<option value=\"" << R::esc(line) << "\">" << R::esc(line) << "</option>";
            }
            h << "</select>";
        } else if (ty == "checkbox") {
            h << "<input type=\"checkbox\" name=\"" << R::esc(nm) << "\" value=\"yes\""
              << reqAttr << "/>";
        } else {
            // text / email / tel / number all map to an <input>; the type
            // comes from the allow-list, never from the stored string directly.
            const std::string it = (ty == "email" || ty == "tel" || ty == "number") ? ty : "text";
            h << "<input type=\"" << it << "\" name=\"" << R::esc(nm)
              << "\" placeholder=\"" << R::esc(ph) << "\" maxlength=\"4000\""
              << reqAttr << "/>";
        }
        h << "</label>";
    }

    // The honeypot: a real field name a bot will happily fill, hidden from
    // people. Any submission that fills it is discarded.
    h << "<div class=\"w-hp\" aria-hidden=\"true\">"
      << "<label>Leave this empty<input type=\"text\" name=\"website\" "
      << "tabindex=\"-1\" autocomplete=\"off\"/></label></div>";

    const std::string btn = sOr(f[0]["submit_label"]);
    h << "<button type=\"submit\" class=\"w-btn\">"
      << R::esc(btn.empty() ? "Send" : btn) << "</button>"
      << "<p class=\"w-form-msg\" role=\"status\"></p></form>";
    return h.str();
}

// ================================================================
// SCHEMA + SEEDS
// ================================================================
void WebsiteForm::ensureSchema(pqxx::transaction_base& txn) {
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS website_form (
            id              SERIAL PRIMARY KEY,
            slug            VARCHAR NOT NULL,
            title           VARCHAR NOT NULL,
            description     TEXT,
            submit_label    VARCHAR,
            success_message TEXT,
            target_model    VARCHAR,
            active          BOOLEAN NOT NULL DEFAULT TRUE,
            create_date     TIMESTAMP NOT NULL DEFAULT now(),
            write_date      TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT website_form_slug_uniq UNIQUE (slug)
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS website_form_field (
            id          SERIAL PRIMARY KEY,
            form_id     INTEGER NOT NULL REFERENCES website_form(id) ON DELETE CASCADE,
            name        VARCHAR NOT NULL,
            label       VARCHAR NOT NULL,
            field_type  VARCHAR NOT NULL DEFAULT 'text',
            placeholder VARCHAR,
            options     TEXT,
            required    BOOLEAN NOT NULL DEFAULT FALSE,
            sequence    INTEGER NOT NULL DEFAULT 10,
            create_date TIMESTAMP NOT NULL DEFAULT now(),
            write_date  TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT website_form_field_uniq UNIQUE (form_id, name)
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS website_form_submission (
            id          SERIAL PRIMARY KEY,
            form_id     INTEGER REFERENCES website_form(id) ON DELETE CASCADE,
            data_json   TEXT NOT NULL DEFAULT '{}',
            state       VARCHAR NOT NULL DEFAULT 'new',
            source_ip   VARCHAR,
            task_id     INTEGER,
            create_date TIMESTAMP NOT NULL DEFAULT now(),
            write_date  TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT website_form_submission_state_chk
                CHECK (state IN ('new','read','archived'))
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS website_form_sub_idx "
             "ON website_form_submission (form_id, state, create_date DESC)");
}

void WebsiteForm::seedMenus(pqxx::transaction_base& txn) {
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (125, 'Website Forms', 'website.form', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
            view_mode=EXCLUDED.view_mode
    )");
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (126, 'Form Submissions', 'website.form.submission', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
            view_mode=EXCLUDED.view_mode
    )");
    auto parent = txn.exec("SELECT id FROM ir_ui_menu WHERE name='Settings' AND parent_id IS NULL LIMIT 1");
    if (parent.empty()) return;
    const int pid = parent[0][0].as<int>();
    txn.exec("INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) "
             "VALUES (411, 'Website Forms', $1, 72, 125) "
             "ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id, "
             "  sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id",
             pqxx::params{pid});
    txn.exec("INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) "
             "VALUES (412, 'Form Submissions', $1, 73, 126) "
             "ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id, "
             "  sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id",
             pqxx::params{pid});
}

// ================================================================
// REGISTRATION
// ================================================================
void WebsiteForm::registerModels(core::ModelFactory& models,
                                 std::shared_ptr<infrastructure::DbConnection> db) {
    models.registerCreator("website.form",            [db]{ return std::make_shared<WebForm>(db); });
    models.registerCreator("website.form.field",      [db]{ return std::make_shared<WebFormField>(db); });
    models.registerCreator("website.form.submission", [db]{ return std::make_shared<WebFormSubmission>(db); });
}

void WebsiteForm::registerViewModels(core::ViewModelFactory& vms,
                                     std::shared_ptr<infrastructure::DbConnection> db) {
    vms.registerCreator("website.form",            [db]{ return std::make_shared<GenericViewModel<WebForm>>(db); });
    vms.registerCreator("website.form.field",      [db]{ return std::make_shared<GenericViewModel<WebFormField>>(db); });
    vms.registerCreator("website.form.submission", [db]{ return std::make_shared<GenericViewModel<WebFormSubmission>>(db); });
}

// ================================================================
// THE PUBLIC SUBMIT ROUTE
// ================================================================
void WebsiteForm::registerRoutes(std::shared_ptr<infrastructure::DbConnection> db,
                                 bool devMode,
                                 const std::string& trustedProxies)
{
    const ClientIpResolver clientIp{trustedProxies};
    auto limiter = g_limiter;

    drogon::app().registerHandlerViaRegex("^/site/form/([a-z0-9/-]+)$",
        [db, devMode, clientIp, limiter](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& slug)
        {
            auto res = drogon::HttpResponse::newHttpResponse();
            res->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            res->addHeader("X-Content-Type-Options", "nosniff");
            res->addHeader("X-Frame-Options", "DENY");

            auto fail = [&](drogon::HttpStatusCode code, const std::string& msg) {
                res->setStatusCode(code);
                res->setBody(nlohmann::json{{"error", msg}}.dump());
                cb(res);
            };

            // An unauthenticated write endpoint gets a ceiling before it gets
            // anything else.
            if (!limiter->allow(clientIp(req))) {
                fail(drogon::k429TooManyRequests,
                     "Too many submissions. Please try again shortly.");
                return;
            }
            if (!R::isValidSlug(slug)) { fail(drogon::k404NotFound, "Unknown form"); return; }
            if (req->body().size() > kMaxTotalLen) {
                fail(drogon::k400BadRequest, "That submission is too large."); return;
            }

            nlohmann::json body;
            try { body = nlohmann::json::parse(req->body()); }
            catch (...) { fail(drogon::k400BadRequest, "Invalid submission"); return; }
            if (!body.is_object()) { fail(drogon::k400BadRequest, "Invalid submission"); return; }

            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};

                auto f = txn.exec(
                    "SELECT id, success_message, target_model, title FROM website_form "
                    " WHERE slug=$1 AND active = TRUE LIMIT 1",
                    pqxx::params{slug});
                // An inactive form answers exactly as an absent one.
                if (f.empty()) { fail(drogon::k404NotFound, "Unknown form"); return; }
                const int formId = f[0]["id"].as<int>();

                // THE HONEYPOT. A person never sees this field; a bot fills it.
                // Answer 200 so the bot learns nothing from the difference.
                auto hp = body.find("website");
                if (hp != body.end() && hp->is_string() && !hp->get<std::string>().empty()) {
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(nlohmann::json{{"ok", true}}.dump());
                    cb(res);
                    return;
                }

                auto fields = txn.exec(
                    "SELECT name, label, field_type, required FROM website_form_field "
                    " WHERE form_id=$1 ORDER BY sequence, id",
                    pqxx::params{formId});

                // THE ALLOW-LIST. We iterate the form's DECLARED fields and
                // pull each one out of the body. Anything else the caller sent
                // is never looked at, so it cannot be stored, echoed, or
                // mapped anywhere later.
                nlohmann::json data = nlohmann::json::object();
                std::vector<std::string> missing;
                std::size_t total = 0;

                for (const auto& fl : fields) {
                    const std::string nm  = sOr(fl["name"]);
                    const std::string lbl = sOr(fl["label"]);
                    const std::string ty  = sOr(fl["field_type"]);
                    const bool req = !fl["required"].is_null() && fl["required"].as<bool>(false);

                    std::string val;
                    auto it = body.find(nm);
                    if (it != body.end()) {
                        if      (it->is_string())          val = it->get<std::string>();
                        else if (it->is_number_integer())  val = std::to_string(it->get<long long>());
                        else if (it->is_number_float())    val = std::to_string(it->get<double>());
                        else if (it->is_boolean())         val = it->get<bool>() ? "yes" : "";
                        // arrays/objects are ignored: a form field is a scalar
                    }
                    if (val.size() > kMaxFieldLen) val = val.substr(0, kMaxFieldLen);
                    total += val.size();

                    if (ty == "number" && !val.empty()) {
                        // Keep it a number or drop it; never store prose in a
                        // field the back office will read as a figure.
                        bool numeric = true; bool seenDigit = false;
                        for (std::size_t i = 0; i < val.size(); ++i) {
                            const char c = val[i];
                            if (c >= '0' && c <= '9') { seenDigit = true; continue; }
                            if ((c == '-' || c == '+') && i == 0) continue;
                            if (c == '.') continue;
                            numeric = false; break;
                        }
                        if (!numeric || !seenDigit) val.clear();
                    }

                    if (req && val.empty()) missing.push_back(lbl.empty() ? nm : lbl);
                    if (!val.empty()) data[nm] = val;
                }

                if (total > kMaxTotalLen) {
                    fail(drogon::k400BadRequest, "That submission is too large."); return;
                }
                if (!missing.empty()) {
                    std::string m = "Please complete: ";
                    for (std::size_t i = 0; i < missing.size(); ++i)
                        m += (i ? ", " : "") + missing[i];
                    fail(drogon::k400BadRequest, m); return;
                }
                if (data.empty()) { fail(drogon::k400BadRequest, "Nothing was submitted."); return; }

                const std::string ip = clientIp(req);
                auto ins = txn.exec(
                    "INSERT INTO website_form_submission (form_id, data_json, source_ip) "
                    "VALUES ($1, $2, $3) RETURNING id",
                    pqxx::params{formId, data.dump(), ip});
                const int subId = ins[0][0].as<int>();

                // A2 — optional routing. The ONLY extra place a submission may
                // go, chosen from an allow-list on the form, mapped explicitly.
                const std::string target = sOr(f[0]["target_model"]);
                if (target == "project.task") {
                    std::string summary;
                    for (auto it = data.begin(); it != data.end(); ++it) {
                        if (!summary.empty()) summary += "\n";
                        summary += it.key() + ": " + it.value().get<std::string>();
                    }
                    auto proj = txn.exec("SELECT id FROM project_project ORDER BY id LIMIT 1");
                    if (!proj.empty()) {
                        auto t = txn.exec(
                            "INSERT INTO project_task (name, project_id, description) "
                            "VALUES ($1, $2, $3) RETURNING id",
                            pqxx::params{std::string("Website: ") + sOr(f[0]["title"]),
                                         proj[0][0].as<int>(), summary});
                        txn.exec("UPDATE website_form_submission SET task_id=$2 WHERE id=$1",
                                 pqxx::params{subId, t[0][0].as<int>()});
                    }
                }
                txn.commit();

                const std::string msg = sOr(f[0]["success_message"]);
                res->setStatusCode(drogon::k200OK);
                res->setBody(nlohmann::json{
                    {"ok", true},
                    {"message", msg.empty() ? "Thank you — we have received your message."
                                            : msg},
                }.dump());
                cb(res);
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[website/form] pool: " << e.what();
                fail(drogon::k503ServiceUnavailable,
                     "The site is busy. Please try again shortly.");
            } catch (const std::exception& e) {
                LOG_ERROR << "[website/form] " << e.what();
                fail(drogon::k500InternalServerError,
                     devMode ? e.what() : "An internal error occurred");
            }
        },
        {drogon::Post});
}

} // namespace cerp::modules::website

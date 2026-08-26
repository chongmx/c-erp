// =============================================================
// modules/help/HelpModule.cpp — docs/101
// =============================================================
#include "HelpModule.hpp"
#include "HelpContent.hpp"
#include "HelpContentB.hpp"
#include <drogon/drogon.h>          // LOG_INFO
#include "BaseModel.hpp"
#include "BaseView.hpp"
#include "BaseViewModel.hpp"
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::help {

using namespace odoo::infrastructure;
using namespace odoo::core;

static int m2oId(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer()) return v[0].get<int>();
    return 0;
}
static std::string jstr(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string{};
}

// ================================================================
// MODEL
// ================================================================
class HelpArticle : public BaseModel<HelpArticle> {
public:
    static constexpr const char* MODEL_NAME = "help.article";
    static constexpr const char* TABLE_NAME = "help_article";

    std::string book, bookLabel, slug, title, body, keywords;
    int  parentId = 0, sequence = 10;
    bool isSection = false, active = true;

    explicit HelpArticle(std::shared_ptr<DbConnection> db) : BaseModel<HelpArticle>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"book",       FieldType::Char, "Book", true});
        fieldRegistry_.add({"book_label", FieldType::Char, "Book Label"});
        fieldRegistry_.add({"slug",       FieldType::Char, "Slug", true});
        fieldRegistry_.add({"title",      FieldType::Char, "Title", true});
        fieldRegistry_.add({"body",       FieldType::Text, "Body"});
        fieldRegistry_.add({"keywords",   FieldType::Char, "Keywords"});
        fieldRegistry_.add({"parent_id",  FieldType::Many2one, "Section", false, false, true, false, "help.article"});
        fieldRegistry_.add({"sequence",   FieldType::Integer, "Sequence"});
        fieldRegistry_.add({"is_section", FieldType::Boolean, "Is Section"});
        fieldRegistry_.add({"active",     FieldType::Boolean, "Active"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["book"] = book; j["book_label"] = bookLabel; j["slug"] = slug;
        j["title"] = title; j["body"] = body; j["keywords"] = keywords;
        j["parent_id"] = parentId > 0 ? nlohmann::json(parentId) : nlohmann::json(false);
        j["sequence"] = sequence; j["is_section"] = isSection; j["active"] = active;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("book"))       book      = jstr(j, "book");
        if (j.contains("book_label")) bookLabel = jstr(j, "book_label");
        if (j.contains("slug"))       slug      = jstr(j, "slug");
        if (j.contains("title"))      title     = jstr(j, "title");
        if (j.contains("body"))       body      = jstr(j, "body");
        if (j.contains("keywords"))   keywords  = jstr(j, "keywords");
        if (j.contains("parent_id"))  parentId  = m2oId(j["parent_id"]);
        if (j.contains("sequence")   && j["sequence"].is_number())    sequence  = j["sequence"].get<int>();
        if (j.contains("is_section") && j["is_section"].is_boolean()) isSection = j["is_section"].get<bool>();
        if (j.contains("active")     && j["active"].is_boolean())     active    = j["active"].get<bool>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = title;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (book.empty())  e.push_back("book is required");
        if (slug.empty())  e.push_back("slug is required");
        if (title.empty()) e.push_back("title is required");
        return e;
    }
};

// ================================================================
// VIEWMODEL
// ================================================================
class HelpArticleViewModel : public GenericViewModel<HelpArticle> {
public:
    explicit HelpArticleViewModel(std::shared_ptr<DbConnection> db)
        : GenericViewModel<HelpArticle>(db), db_(std::move(db)) {
        REGISTER_METHOD("books",   handleBooks)
        REGISTER_METHOD("tree",    handleTree)
        REGISTER_METHOD("article", handleArticle)
        REGISTER_METHOD("search",  handleSearch)
        REGISTER_METHOD("related", handleRelated)
    }
private:
    std::shared_ptr<DbConnection> db_;

    /// Every book the system knows about, whether or not it has articles yet.
    /// A module with no help returns count 0 and the client shows it as not yet
    /// documented — a tab that is missing entirely would just look like a bug.
    nlohmann::json handleBooks(const core::CallKwArgs&) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        std::map<std::string, long> counts;
        for (const auto& row : txn.exec(
                "SELECT book, count(*) FROM help_article "
                " WHERE active AND NOT is_section GROUP BY 1"))
            counts[row[0].c_str()] = row[1].as<long>(0);

        nlohmann::json arr = nlohmann::json::array();
        for (int i = 0; i < kHelpBookCount; ++i) {
            const auto& b = kHelpBooks[i];
            const long n = counts.count(b.slug) ? counts[b.slug] : 0;
            arr.push_back({{"slug", b.slug}, {"label", b.label},
                           {"sequence", b.sequence}, {"count", n}});
            counts.erase(b.slug);
        }
        // Anything written for a book not in the known list still gets a tab,
        // so adding articles never requires editing two places at once.
        for (const auto& [slug, n] : counts)
            arr.push_back({{"slug", slug}, {"label", slug}, {"sequence", 800}, {"count", n}});
        return arr;
    }

    /// One book's contents as a two-level tree: sections, each with articles.
    nlohmann::json handleTree(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const std::string book = jstr(v, "book");
        if (book.empty()) throw ValidationError("book is required.");

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto res = txn.exec(
            "SELECT id, slug, title, COALESCE(parent_id,0) AS parent_id, "
            "       sequence, is_section, COALESCE(keywords,'') AS keywords "
            "FROM help_article WHERE active AND book=$1 ORDER BY sequence, id",
            pqxx::params{book});

        std::vector<nlohmann::json> sections;
        std::map<int, size_t> byId;
        std::vector<nlohmann::json> loose;

        for (const auto& row : res) {
            if (!row["is_section"].as<bool>(false)) continue;
            byId[row["id"].as<int>()] = sections.size();
            sections.push_back({{"id", row["id"].as<int>()},
                                {"slug", row["slug"].c_str()},
                                {"title", row["title"].c_str()},
                                {"articles", nlohmann::json::array()}});
        }
        for (const auto& row : res) {
            if (row["is_section"].as<bool>(false)) continue;
            nlohmann::json a = {{"id", row["id"].as<int>()},
                                {"slug", row["slug"].c_str()},
                                {"title", row["title"].c_str()},
                                {"keywords", row["keywords"].c_str()}};
            const int pid = row["parent_id"].as<int>(0);
            if (pid && byId.count(pid)) sections[byId[pid]]["articles"].push_back(a);
            else loose.push_back(a);
        }
        // An article whose section was deleted must still be reachable, or it
        // becomes invisible content that nothing links to.
        if (!loose.empty()) {
            nlohmann::json other = {{"id", 0}, {"slug", "_other"}, {"title", "Other"},
                                    {"articles", nlohmann::json::array()}};
            for (auto& a : loose) other["articles"].push_back(a);
            sections.push_back(other);
        }

        nlohmann::json out = nlohmann::json::array();
        for (auto& s : sections) out.push_back(s);
        return out;
    }

    nlohmann::json handleArticle(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const std::string slug = jstr(v, "slug");
        if (slug.empty()) throw ValidationError("slug is required.");

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto res = txn.exec(
            "SELECT a.id, a.book, a.slug, a.title, COALESCE(a.body,'') AS body, "
            "       COALESCE(a.keywords,'') AS keywords, "
            "       COALESCE(p.title,'') AS section_title, COALESCE(p.slug,'') AS section_slug "
            "FROM help_article a LEFT JOIN help_article p ON p.id = a.parent_id "
            "WHERE a.active AND a.slug=$1", pqxx::params{slug});
        // ValidationError, not runtime_error: a missing article is an ordinary
        // not-found that the reader should be told about, and a slug is public.
        // A runtime_error would be masked as "An internal error occurred" by
        // SEC-28 — correct for a leaked SQL message, useless for a dead link.
        if (res.empty()) throw ValidationError("No such help article: " + slug);

        return {{"id", res[0]["id"].as<int>()},
                {"book", res[0]["book"].c_str()},
                {"slug", res[0]["slug"].c_str()},
                {"title", res[0]["title"].c_str()},
                {"body", res[0]["body"].c_str()},
                {"keywords", res[0]["keywords"].c_str()},
                {"section_title", res[0]["section_title"].c_str()},
                {"section_slug", res[0]["section_slug"].c_str()}};
    }

    /// Search across titles, keywords and bodies. Titles and keywords outrank
    /// body hits, because a word in a title is what the article is *about*
    /// while a word in the body may be an aside.
    nlohmann::json handleSearch(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const std::string q = jstr(v, "q");
        const std::string book = jstr(v, "book");
        if (q.size() < 2) return nlohmann::json::array();

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        std::string sql =
            "SELECT slug, title, book, COALESCE(keywords,'') AS keywords, "
            "       CASE WHEN title    ILIKE $1 THEN 3 "
            "            WHEN keywords ILIKE $1 THEN 2 ELSE 1 END AS rank, "
            "       substring(COALESCE(body,'') from greatest(1, position(lower($2) in lower(COALESCE(body,''))) - 60) "
            "                 for 180) AS excerpt "
            "FROM help_article WHERE active AND NOT is_section "
            "  AND (title ILIKE $1 OR COALESCE(keywords,'') ILIKE $1 OR COALESCE(body,'') ILIKE $1)";
        pqxx::params p;
        p.append("%" + q + "%");
        p.append(q);
        if (!book.empty()) { sql += " AND book=$3"; p.append(book); }
        sql += " ORDER BY rank DESC, title LIMIT 40";

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : txn.exec(sql, p))
            arr.push_back({{"slug", row["slug"].c_str()}, {"title", row["title"].c_str()},
                           {"book", row["book"].c_str()}, {"rank", row["rank"].as<int>(1)},
                           {"excerpt", row["excerpt"].c_str()}});
        return arr;
    }

    /// Articles related to the one being read — the same retrieval step an
    /// assistant would run before answering, exposed on its own so the panel
    /// is useful before any model is wired up.
    nlohmann::json handleRelated(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const std::string slug = jstr(v, "slug");
        if (slug.empty()) return nlohmann::json::array();

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto me = txn.exec("SELECT book, COALESCE(keywords,'') FROM help_article WHERE slug=$1",
                           pqxx::params{slug});
        if (me.empty()) return nlohmann::json::array();
        const std::string book = me[0][0].c_str();
        const std::string kw   = me[0][1].c_str();

        // Score by shared keywords; same book first.
        std::vector<std::string> words;
        std::string cur;
        for (const char c : kw) {
            if (c == ' ') { if (cur.size() > 3) words.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (cur.size() > 3) words.push_back(cur);

        std::map<std::string, std::pair<int, std::string>> score;   // slug -> (hits, title)
        for (const auto& w : words) {
            for (const auto& row : txn.exec(
                    "SELECT slug, title FROM help_article "
                    " WHERE active AND NOT is_section AND slug<>$1 AND book=$2 "
                    "   AND (COALESCE(keywords,'') ILIKE $3 OR title ILIKE $3) LIMIT 20",
                    pqxx::params{slug, book, "%" + w + "%"})) {
                auto& s = score[row[0].c_str()];
                s.first += 1;
                s.second = row[1].c_str();
            }
        }
        std::vector<std::pair<std::string, std::pair<int, std::string>>> ranked(score.begin(), score.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.second.first > b.second.first; });

        nlohmann::json arr = nlohmann::json::array();
        std::set<std::string> taken{slug};
        for (size_t i = 0; i < ranked.size() && arr.size() < 6; ++i) {
            arr.push_back({{"slug", ranked[i].first}, {"title", ranked[i].second.second},
                           {"hits", ranked[i].second.first}});
            taken.insert(ranked[i].first);
        }

        // Keyword overlap alone is sparse — an article with distinctive
        // keywords ("walkthrough", "scenario") shares none with its neighbours
        // and would show an empty panel, which reads as broken rather than as
        // "nothing related". Top up with its section, then its book, so the
        // panel always offers somewhere to go next.
        auto topUp = [&](const std::string& sql, pqxx::params p) {
            if (arr.size() >= 6) return;
            for (const auto& row : txn.exec(sql, p)) {
                const std::string s = row[0].c_str();
                if (taken.count(s)) continue;
                arr.push_back({{"slug", s}, {"title", row[1].c_str()}, {"hits", 0}});
                taken.insert(s);
                if (arr.size() >= 6) return;
            }
        };
        topUp("SELECT a.slug, a.title FROM help_article a "
              " WHERE a.active AND NOT a.is_section AND a.slug<>$1 "
              "   AND a.parent_id = (SELECT parent_id FROM help_article WHERE slug=$1) "
              " ORDER BY a.sequence, a.id LIMIT 8", pqxx::params{slug});
        topUp("SELECT slug, title FROM help_article "
              " WHERE active AND NOT is_section AND slug<>$1 AND book=$2 "
              " ORDER BY sequence, id LIMIT 12", pqxx::params{slug, book});
        // Last resort: other books. A book with a single article has no
        // siblings at all, and an empty panel reads as broken rather than as
        // "this happens to be the only page in this book".
        topUp("SELECT slug, title FROM help_article "
              " WHERE active AND NOT is_section AND slug<>$1 "
              " ORDER BY book, sequence, id LIMIT 12", pqxx::params{slug});
        return arr;
    }
};

// ================================================================
// VIEW
// ================================================================
class HelpArticleListView : public core::BaseView {
public:
    std::string viewName() const override { return "help.article.list"; }
    std::string modelName() const override { return "help.article"; }
    std::string viewType() const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Help Articles\">"
               "<field name=\"book\"/><field name=\"title\"/><field name=\"slug\"/>"
               "<field name=\"sequence\"/><field name=\"is_section\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {{"book",       {{"type","char"},    {"string","Book"}}},
                {"title",      {{"type","char"},    {"string","Title"}}},
                {"slug",       {{"type","char"},    {"string","Slug"}}},
                {"sequence",   {{"type","integer"}, {"string","Sequence"}}},
                {"is_section", {{"type","boolean"}, {"string","Section"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ================================================================
// MODULE
// ================================================================
HelpModule::HelpModule(core::ModelFactory& models, core::ServiceFactory& services,
                       core::ViewModelFactory& viewModels, core::ViewFactory& views)
    : models_(models), services_(services), viewModels_(viewModels), views_(views) {}

std::string              HelpModule::moduleName()   const { return "help"; }
std::string              HelpModule::version()      const { return "1.0"; }
std::vector<std::string> HelpModule::dependencies() const { return {"base"}; }

void HelpModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("help.article", [db]{ return std::make_shared<HelpArticle>(db); });
}
void HelpModule::registerServices() {}
void HelpModule::registerViewModels() {
    auto db = services_.db();
    viewModels_.registerCreator("help.article", [db]{ return std::make_shared<HelpArticleViewModel>(db); });
}
void HelpModule::registerViews() {
    views_.registerCreator("help.article.list", []{ return std::make_shared<HelpArticleListView>(); });
}
void HelpModule::registerRoutes() {}

void HelpModule::initialize() {
    ensureSchema_();
    seedContent_();
    seedMenus_();
}

void HelpModule::ensureSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS help_article (
            id          SERIAL PRIMARY KEY,
            book        VARCHAR NOT NULL,
            book_label  VARCHAR,
            slug        VARCHAR NOT NULL,
            title       VARCHAR NOT NULL,
            body        TEXT,
            keywords    VARCHAR,
            parent_id   INTEGER REFERENCES help_article(id) ON DELETE SET NULL,
            sequence    INTEGER NOT NULL DEFAULT 10,
            is_section  BOOLEAN NOT NULL DEFAULT FALSE,
            active      BOOLEAN NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");
    // The slug is the article's public address — it is what a deep link and,
    // later, an assistant's citation both point at, so it must be unique.
    txn.exec("CREATE UNIQUE INDEX IF NOT EXISTS help_article_slug_uniq ON help_article (slug)");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_help_article_book ON help_article (book, sequence)");
    txn.commit();
}

void HelpModule::seedContent_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    // Upsert on slug: shipped help is refreshed on every start so a corrected
    // article reaches existing installs. Anything an operator writes themselves
    // has a slug we never seed, so it is left alone.
    //
    // Two passes over both content files: every SECTION is written before any
    // article, so an article can always resolve its parent by slug regardless of
    // which file it lives in or what order the arrays are in.
    const HelpSeed* tables[] = {kHelpSeeds, kHelpSeedsB};
    const int       counts[] = {kHelpSeedCount, kHelpSeedCountB};
    int written = 0;

    for (int pass = 0; pass < 2; ++pass) {
        for (int t = 0; t < 2; ++t) {
            for (int i = 0; i < counts[t]; ++i) {
                const auto& s = tables[t][i];
                const bool isSection = (s.parent[0] == '\0');
                if ((pass == 0) != isSection) continue;

                int parentId = 0;
                if (!isSection) {
                    auto p = txn.exec("SELECT id FROM help_article WHERE slug=$1",
                                      pqxx::params{s.parent});
                    if (p.empty())
                        // A typo in a parent slug would otherwise silently produce
                        // an article nothing links to.
                        LOG_ERROR << "[help] article '" << s.slug
                                  << "' names a section that does not exist: " << s.parent;
                    else
                        parentId = p[0][0].as<int>();
                }
                txn.exec(
                    "INSERT INTO help_article "
                    "  (book, book_label, slug, title, body, keywords, parent_id, sequence, is_section) "
                    "VALUES ($1,$2,$3,$4,$5,$6,NULLIF($7,0),$8,$9) "
                    "ON CONFLICT (slug) DO UPDATE SET "
                    "  book=EXCLUDED.book, book_label=EXCLUDED.book_label, title=EXCLUDED.title, "
                    "  body=EXCLUDED.body, keywords=EXCLUDED.keywords, parent_id=EXCLUDED.parent_id, "
                    "  sequence=EXCLUDED.sequence, is_section=EXCLUDED.is_section, write_date=now()",
                    pqxx::params{s.book, s.bookLabel, s.slug, s.title, s.body, s.keywords,
                                 parentId, s.sequence, isSection});
                ++written;
            }
        }
    }
    txn.commit();
    LOG_INFO << "[help] " << written << " help rows seeded/refreshed";
}

void HelpModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, path, context) VALUES
            (114, 'Help Centre', 'help.center', 'list', 'help', '{}'),
            (115, 'Help Articles', 'help.article', 'list,form', 'help-articles', '{}')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
                view_mode=EXCLUDED.view_mode, path=EXCLUDED.path, domain=NULL
    )");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");

    // App root — DO NOTHING, never DO UPDATE (see verify_menu_ids.sh).
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id, web_icon) VALUES
            (400, 'Help', NULL, 95, NULL, 'help')
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (401, 'Help Centre',  400, 10, 114),
            (402, 'Help Articles', 400, 20, 115)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");
    txn.commit();
}

} // namespace odoo::modules::help

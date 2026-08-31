// =============================================================
// modules/website/WebsiteModule.cpp — implementation (docs/115)
// =============================================================
#include "WebsiteModule.hpp"
#include "WebsiteRender.hpp"
#include "WebsitePalette.hpp"
#include "WebsiteMedia.hpp"
#include "WebsiteForm.hpp"
#include "Filestore.hpp"
#include "BaseModel.hpp"
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include "SessionManager.hpp"
#include "Groups.hpp"
#include "Errors.hpp"
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace cerp::modules::website {

using namespace cerp::infrastructure;
using namespace cerp::core;

namespace {
inline int wM2oId(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer()) return v[0].get<int>();
    return 0;
}
inline std::string sOr(const pqxx::field& f) {
    return f.is_null() ? std::string{} : std::string(f.c_str());
}
} // anonymous namespace

// ================================================================
// MODELS
// ================================================================
class WebsitePage : public BaseModel<WebsitePage> {
public:
    static constexpr const char* MODEL_NAME = "website.page";
    static constexpr const char* TABLE_NAME = "website_page";
    explicit WebsitePage(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    std::string slug, title, blocksJson, metaTitle, metaDescription, metaKeywords;
    // A blog post is a PAGE with a kind, not a second table (docs/116 A4).
    // the reference ERP has blog.post + blog.tag + blog.blog; here a post has the same
    // slug, blocks, publishing and SEO a page has, and inventing a parallel
    // model would mean maintaining two of everything for one extra field.
    std::string pageKind = "page";        // "page" | "post"
    std::string publishDate, author, excerpt;
    bool isPublished = false;
    bool isIndexed   = true;
    bool isHomepage  = false;
    int  sequence    = 10;

    void registerFields() {
        fieldRegistry_.add({"slug",             FieldType::Char,    "URL Slug", true});
        fieldRegistry_.add({"title",            FieldType::Char,    "Title",    true});
        fieldRegistry_.add({"blocks_json",      FieldType::Text,    "Content Blocks"});
        fieldRegistry_.add({"is_published",     FieldType::Boolean, "Published"});
        fieldRegistry_.add({"is_indexed",       FieldType::Boolean, "Search-engine indexed"});
        fieldRegistry_.add({"is_homepage",      FieldType::Boolean, "Is Homepage"});
        fieldRegistry_.add({"sequence",         FieldType::Integer, "Sequence"});
        fieldRegistry_.add({"meta_title",       FieldType::Char,    "Meta Title"});
        fieldRegistry_.add({"meta_description", FieldType::Text,    "Meta Description"});
        fieldRegistry_.add({"meta_keywords",    FieldType::Char,    "Meta Keywords"});
        fieldRegistry_.add({"page_kind",        FieldType::Selection,"Kind"});
        fieldRegistry_.add({"publish_date",     FieldType::Date,    "Publish date"});
        fieldRegistry_.add({"author",           FieldType::Char,    "Author"});
        fieldRegistry_.add({"excerpt",          FieldType::Text,    "Excerpt"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["slug"] = slug; j["title"] = title;
        j["blocks_json"]      = blocksJson.empty()      ? nlohmann::json("[]")  : nlohmann::json(blocksJson);
        j["is_published"]     = isPublished;
        j["is_indexed"]       = isIndexed;
        j["is_homepage"]      = isHomepage;
        j["sequence"]         = sequence;
        j["meta_title"]       = metaTitle.empty()       ? nlohmann::json(false) : nlohmann::json(metaTitle);
        j["meta_description"] = metaDescription.empty() ? nlohmann::json(false) : nlohmann::json(metaDescription);
        j["meta_keywords"]    = metaKeywords.empty()    ? nlohmann::json(false) : nlohmann::json(metaKeywords);
        j["page_kind"]        = pageKind;
        j["publish_date"]     = publishDate.empty() ? nlohmann::json(false) : nlohmann::json(publishDate);
        j["author"]           = author.empty()      ? nlohmann::json(false) : nlohmann::json(author);
        j["excerpt"]          = excerpt.empty()     ? nlohmann::json(false) : nlohmann::json(excerpt);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("slug")  && j["slug"].is_string())  slug  = j["slug"].get<std::string>();
        if (j.contains("title") && j["title"].is_string()) title = j["title"].get<std::string>();
        if (j.contains("blocks_json") && j["blocks_json"].is_string()) blocksJson = j["blocks_json"].get<std::string>();
        if (j.contains("is_published") && j["is_published"].is_boolean()) isPublished = j["is_published"].get<bool>();
        if (j.contains("is_indexed")   && j["is_indexed"].is_boolean())   isIndexed   = j["is_indexed"].get<bool>();
        if (j.contains("is_homepage")  && j["is_homepage"].is_boolean())  isHomepage  = j["is_homepage"].get<bool>();
        if (j.contains("sequence") && j["sequence"].is_number_integer())  sequence    = j["sequence"].get<int>();
        if (j.contains("meta_title")       && j["meta_title"].is_string())       metaTitle       = j["meta_title"].get<std::string>();
        if (j.contains("meta_description") && j["meta_description"].is_string()) metaDescription = j["meta_description"].get<std::string>();
        if (j.contains("meta_keywords")    && j["meta_keywords"].is_string())    metaKeywords    = j["meta_keywords"].get<std::string>();
        // page_kind selects a rendering path, so it comes from an allow-list.
        if (j.contains("page_kind") && j["page_kind"].is_string()) {
            const std::string k = j["page_kind"].get<std::string>();
            if (k == "page" || k == "post") pageKind = k;
        }
        if (j.contains("publish_date") && j["publish_date"].is_string()) publishDate = j["publish_date"].get<std::string>();
        if (j.contains("author")       && j["author"].is_string())       author      = j["author"].get<std::string>();
        if (j.contains("excerpt")      && j["excerpt"].is_string())      excerpt     = j["excerpt"].get<std::string>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = title; return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (title.empty()) e.push_back("A page title is required");
        if (slug.empty())  e.push_back("A URL slug is required");
        // The slug becomes a public URL. Reject anything outside the charset
        // here as well as at the route, so a bad slug cannot be stored at all.
        else if (!WebsiteRender::isValidSlug(slug))
            e.push_back("The URL slug may contain only lowercase letters, "
                        "digits, hyphens and slashes");
        if (!blocksJson.empty()) {
            // Content is parsed on the way IN. A page whose blocks do not
            // parse would render as nothing, silently, at the worst moment.
            try {
                auto v = nlohmann::json::parse(blocksJson);
                if (!v.is_array()) e.push_back("Content blocks must be a list");
            } catch (...) { e.push_back("Content blocks are not valid JSON"); }
        }
        return e;
    }
};

class WebsiteMenu : public BaseModel<WebsiteMenu> {
public:
    static constexpr const char* MODEL_NAME = "website.menu";
    static constexpr const char* TABLE_NAME = "website_menu";
    explicit WebsiteMenu(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    std::string name, url;
    int  pageId = 0, parentId = 0, sequence = 10;
    bool newWindow = false;

    void registerFields() {
        fieldRegistry_.add({"name",       FieldType::Char,     "Label", true});
        fieldRegistry_.add({"url",        FieldType::Char,     "URL"});
        fieldRegistry_.add({"page_id",    FieldType::Many2one, "Page",   false, false, true, false, "website.page"});
        fieldRegistry_.add({"parent_id",  FieldType::Many2one, "Parent", false, false, true, false, "website.menu"});
        fieldRegistry_.add({"sequence",   FieldType::Integer,  "Sequence"});
        fieldRegistry_.add({"new_window", FieldType::Boolean,  "Open in new window"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"]       = name;
        j["url"]        = url.empty() ? nlohmann::json(false) : nlohmann::json(url);
        j["page_id"]    = pageId   > 0 ? nlohmann::json::array({pageId, ""})   : nlohmann::json(false);
        j["parent_id"]  = parentId > 0 ? nlohmann::json::array({parentId, ""}) : nlohmann::json(false);
        j["sequence"]   = sequence;
        j["new_window"] = newWindow;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("url")  && j["url"].is_string())  url  = j["url"].get<std::string>();
        if (j.contains("page_id"))   pageId   = wM2oId(j["page_id"]);
        if (j.contains("parent_id")) parentId = wM2oId(j["parent_id"]);
        if (j.contains("sequence") && j["sequence"].is_number_integer()) sequence = j["sequence"].get<int>();
        if (j.contains("new_window") && j["new_window"].is_boolean()) newWindow = j["new_window"].get<bool>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = name; return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("A menu label is required");
        // An external URL on a menu is rendered as an href. Anything that is
        // not a safe scheme is rejected on the way in.
        if (!url.empty()) {
            const std::string probe =
                "<a href=\"" + url + "\">x</a>";
            if (WebsiteRender::sanitize(probe).find("href=") == std::string::npos)
                e.push_back("That menu URL is not allowed");
        }
        return e;
    }
};

// ================================================================
// PAGE RENDERING — chrome around the blocks
// ================================================================
namespace {

struct SiteSettings {
    std::string name, footer, baseUrl;
    std::string themeKey;            ///< the preset in force
    std::string accent, onAccent;    ///< accent, and the readable ink on it
    std::string onAccentOverride;    ///< empty when onAccent was computed
    Scheme      light, dark;         ///< resolved tokens, overrides applied
    DarkMode    darkMode = DarkMode::Auto;
    bool        showLogin = true;    ///< a "Sign in" link in the top bar
};

SiteSettings loadSettings(pqxx::transaction_base& txn) {
    SiteSettings s;
    s.name    = "Our Company";
    s.baseUrl = "http://localhost:8069";

    std::map<std::string, std::string> cfg;
    auto rows = txn.exec(
        "SELECT key, value FROM ir_config_parameter "
        " WHERE key LIKE 'website.%' OR key = 'web.base.url'");
    for (const auto& r : rows) cfg[sOr(r[0])] = sOr(r[1]);
    auto get = [&cfg](const char* k) -> std::string {
        const auto it = cfg.find(k);
        return it == cfg.end() ? std::string() : it->second;
    };

    if (!get("website.site_name").empty()) s.name    = get("website.site_name");
    if (!get("web.base.url").empty())      s.baseUrl = get("web.base.url");
    s.footer = get("website.footer");

    // The preset supplies a complete pair of schemes; an unknown key is not an
    // error worth failing a public page over, it just means the default.
    const Palette* p = WebsitePalette::preset(get("website.theme"));
    if (!p) p = &WebsitePalette::fallback();
    s.themeKey = p->key;
    s.light    = p->light;
    s.dark     = p->dark;

    // Every colour below lands inside a <style> block, so each one is
    // re-validated here and silently dropped if it is not a hex literal. A
    // config value is not author-trusted just because an administrator set it.
    const std::string accOverride = WebsitePalette::normalizeHex(get("website.accent"));
    s.accent   = accOverride.empty() ? p->accent : accOverride;

    // Text on the accent is computed by contrast, which is the right default
    // and occasionally the wrong brand. #e94560 genuinely reads better with
    // near-black (4.85:1) than white (3.83:1), but an owner whose buttons have
    // always been white-on-pink should be able to say so — and own the
    // consequence — rather than being argued with by a stylesheet.
    const std::string onOverride = WebsitePalette::normalizeHex(get("website.on_accent"));
    s.onAccentOverride = onOverride;
    s.onAccent = onOverride.empty() ? WebsitePalette::onColor(s.accent) : onOverride;

    auto applyOverride = [&get](const char* key, std::string& slot) {
        const std::string v = WebsitePalette::normalizeHex(get(key));
        if (!v.empty()) slot = v;
    };
    applyOverride("website.color.bg",           s.light.bg);
    applyOverride("website.color.surface",      s.light.surface);
    applyOverride("website.color.ink",          s.light.ink);
    applyOverride("website.color.muted",        s.light.mut);
    applyOverride("website.color.line",         s.light.line);
    applyOverride("website.color.dark.bg",      s.dark.bg);
    applyOverride("website.color.dark.surface", s.dark.surface);
    applyOverride("website.color.dark.ink",     s.dark.ink);
    applyOverride("website.color.dark.muted",   s.dark.mut);
    applyOverride("website.color.dark.line",    s.dark.line);

    s.darkMode  = WebsitePalette::darkModeFromString(get("website.dark_mode"));
    s.showLogin = get("website.login_link") != "off";   // present unless refused
    return s;
}

std::string renderMenu(pqxx::transaction_base& txn) {
    auto rows = txn.exec(
        "SELECT m.name, m.url, m.new_window, p.slug, p.is_published "
        "  FROM website_menu m "
        "  LEFT JOIN website_page p ON p.id = m.page_id "
        " WHERE m.parent_id IS NULL "
        " ORDER BY m.sequence, m.id");
    std::ostringstream h;
    h << "<nav class=\"w-nav\">";
    for (const auto& r : rows) {
        std::string href;
        if (!r["slug"].is_null()) {
            // A menu pointing at an unpublished page is hidden rather than
            // rendered as a link into a 404.
            if (r["is_published"].is_null() || !r["is_published"].as<bool>(false)) continue;
            href = "/site/" + sOr(r["slug"]);
        } else {
            href = sOr(r["url"]);
            if (href.empty()) continue;
        }
        const bool nw = !r["new_window"].is_null() && r["new_window"].as<bool>(false);
        h << "<a href=\"" << WebsiteRender::esc(href) << "\""
          << (nw ? " target=\"_blank\" rel=\"noopener\"" : "")
          << ">" << WebsiteRender::esc(sOr(r["name"])) << "</a>";
    }
    h << "</nav>";
    return h.str();
}

std::string pageShell(const SiteSettings& s,
                      const std::string& title,
                      const std::string& metaDesc,
                      const std::string& metaKeywords,
                      const std::string& canonicalPath,
                      bool indexed,
                      const std::string& menuHtml,
                      const std::string& bodyHtml,
                      const std::string& banner = "",
                      const std::string& editor = "",
                      const std::string& jsonLd = "")
{
    using R = WebsiteRender;
    const std::string canonical = s.baseUrl + canonicalPath;
    std::ostringstream h;
    h << "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      << "<title>" << R::esc(title) << " &middot; " << R::esc(s.name) << "</title>";
    if (!metaDesc.empty())
        h << "<meta name=\"description\" content=\"" << R::esc(metaDesc) << "\">";
    if (!metaKeywords.empty())
        h << "<meta name=\"keywords\" content=\"" << R::esc(metaKeywords) << "\">";
    if (!indexed) h << "<meta name=\"robots\" content=\"noindex, nofollow\">";
    h << "<link rel=\"canonical\" href=\"" << R::esc(canonical) << "\">"
      << "<meta property=\"og:type\" content=\"website\">"
      << "<meta property=\"og:title\" content=\"" << R::esc(title) << "\">"
      << "<meta property=\"og:url\" content=\"" << R::esc(canonical) << "\">"
      << "<meta property=\"og:site_name\" content=\"" << R::esc(s.name) << "\">";
    if (!metaDesc.empty())
        h << "<meta property=\"og:description\" content=\"" << R::esc(metaDesc) << "\">"
          << "<meta name=\"twitter:description\" content=\"" << R::esc(metaDesc) << "\">";
    h << "<meta name=\"twitter:card\" content=\"summary\">"
      << "<meta name=\"twitter:title\" content=\"" << R::esc(title) << "\">";

    // schema.org JSON-LD (docs/118 E2). the reference ERP emits none —
    // `grep -rl "application/ld+json" website/` finds nothing — so this is
    // the SEO layer above the OpenGraph tags it does have.
    if (!jsonLd.empty())
        h << "<script type=\"application/ld+json\">" << jsonLd << "</script>";

    // The palette (docs/121). Tokens come from the preset and its overrides;
    // every one was validated as a hex literal in loadSettings, because these
    // strings are pasted into a stylesheet. `--on-a` is computed from the
    // accent's luminance rather than assumed to be white.
    // The derived accent tones are per-scheme: an 8% wash over white is nearly
    // white and over #0e151d is nearly black, so they cannot be written once
    // and reused across both.
    auto tokens = [&s](const Scheme& sc) {
        const Accents a = WebsitePalette::derive(sc, s.accent);
        return "--bg:" + sc.bg + ";--surface:" + sc.surface + ";--ink:" + sc.ink +
               ";--mut:" + sc.mut + ";--line:" + sc.line +
               ";--a-tint:" + a.tint + ";--a-tint2:" + a.tint2 + ";--a-soft:" + a.soft +
               ";--a-deep:" + a.deep + ";--a-rule:" + a.rule + ";--a-text:" + a.text;
    };
    const Scheme& baseScheme = (s.darkMode == DarkMode::On) ? s.dark : s.light;

    h << "<style>"
         ":root{--a:" << s.accent << ";--on-a:" << s.onAccent << ";"
      << tokens(baseScheme) << "}";
    // Only "auto" asks the visitor's OS. "on" already emitted the dark tokens
    // above, and "off" means a dark-mode visitor still gets the light site.
    if (s.darkMode == DarkMode::Auto)
        h << "@media(prefers-color-scheme:dark){:root{" << tokens(s.dark) << "}}";
    h << "*{box-sizing:border-box}"
         "body{margin:0;background:var(--bg);color:var(--ink);line-height:1.65;"
         "font-family:ui-sans-serif,system-ui,-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif}"
         ".w-wrap{max-width:940px;margin:0 auto;padding:0 22px}"
         /* --- top bar: tinted ground, an accent hairline, and a brand mark --- */
         "header.w-top{position:sticky;top:0;z-index:50;background:var(--a-tint);"
         "border-bottom:1px solid var(--a-rule);margin-bottom:34px;"
         "backdrop-filter:saturate(1.4) blur(6px)}"
         "header.w-top:after{content:'';display:block;height:3px;"
         "background:linear-gradient(90deg,var(--a),var(--a-deep));"
         "position:absolute;left:0;right:0;bottom:-1px}"
         "header.w-top .w-wrap{display:flex;flex-wrap:wrap;gap:14px;align-items:center;"
         "justify-content:space-between;padding-top:14px;padding-bottom:14px}"
         ".w-brand{display:inline-flex;align-items:center;gap:9px;font-weight:750;"
         "font-size:1.12rem;letter-spacing:-.01em;color:var(--ink);text-decoration:none}"
         ".w-brand:before{content:'';width:15px;height:15px;border-radius:4px;"
         "background:linear-gradient(135deg,var(--a),var(--a-deep));flex:none}"
         ".w-navwrap{display:flex;flex-wrap:wrap;align-items:center;gap:16px}"
         ".w-nav{display:flex;flex-wrap:wrap;gap:18px}"
         ".w-nav a{position:relative;color:var(--mut);text-decoration:none;font-size:.92rem;"
         "padding:3px 0;font-weight:550}"
         ".w-nav a:after{content:'';position:absolute;left:0;right:100%;bottom:0;height:2px;"
         "border-radius:2px;background:var(--a);transition:right .18s ease}"
         ".w-nav a:hover{color:var(--a-text)}"
         ".w-nav a:hover:after{right:0}"
         /* the staff sign-in affordance */
         ".w-login{display:inline-flex;align-items:center;gap:6px;font-size:.85rem;"
         "font-weight:650;text-decoration:none;color:var(--a-text);padding:6px 13px;"
         "border:1px solid var(--a-rule);border-radius:100px;background:var(--surface)}"
         ".w-login:hover{background:var(--a);color:var(--on-a);border-color:var(--a)}"
         "main{padding-bottom:64px}"
         ".w-h{position:relative;line-height:1.2;letter-spacing:-.018em;"
         "margin:1.6em 0 .5em;text-wrap:balance}"
         "h1.w-h{font-size:2.1rem;margin-top:0}h2.w-h{font-size:1.45rem}h3.w-h{font-size:1.14rem}"
         /* a short accent rule above section headings — structure, not decoration */
         "h2.w-h:before{content:'';display:block;width:34px;height:3px;border-radius:2px;"
         "background:var(--a);margin-bottom:12px}"
         ".w-p{margin:0 0 1em;max-width:70ch}"
         ".w-fig{margin:1.6em 0}.w-fig img{max-width:100%;height:auto;border-radius:6px;display:block}"
         ".w-fig figcaption{color:var(--mut);font-size:.85rem;margin-top:.5em}"
         ".w-hr{border:0;border-top:1px solid var(--line);margin:2.2em 0}"
         ".w-btn{display:inline-block;background:linear-gradient(135deg,var(--a),var(--a-deep));"
         "color:var(--on-a);text-decoration:none;padding:10px 20px;border-radius:7px;"
         "font-weight:600;font-size:.95rem;box-shadow:0 1px 2px rgba(0,0,0,.14);"
         "transition:transform .12s ease,box-shadow .12s ease}"
         ".w-btn:hover{transform:translateY(-1px);box-shadow:0 4px 12px rgba(0,0,0,.18)}"
         ".w-cols{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:26px;margin:1.8em 0}"
         ".w-col-h{margin:0 0 .4em;font-size:1.02rem}"
         ".w-raw{margin:1.4em 0}.w-raw img{max-width:100%;height:auto}"
         ".w-refs{list-style:none;padding:0;margin:1.6em 0;display:grid;"
         "grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:18px}"
         ".w-ref{display:flex;flex-direction:column;gap:5px;padding:14px;"
         "background:var(--surface);border:1px solid var(--line);border-radius:6px}"
         ".w-ref-logo{max-height:38px;max-width:100%;object-fit:contain;align-self:flex-start}"
         ".w-ref-name{font-weight:650}.w-ref-note{color:var(--mut);font-size:.87rem}"
         /* ---- video (docs/125) ---- */
         ".w-video{margin:1.8em 0}"
         ".w-video-frame{position:relative;padding-top:56.25%;border-radius:12px;"
         "overflow:hidden;background:var(--a-tint);border:1px solid var(--line)}"
         ".w-video-frame iframe{position:absolute;inset:0;width:100%;height:100%;border:0}"
         ".w-video-el{width:100%;height:auto;display:block;border-radius:12px;"
         "border:1px solid var(--line);background:#000}"
         ".w-video figcaption{color:var(--mut);font-size:.85rem;margin-top:.6em}"
         /* ---- gallery ---- */
         ".w-gal{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));"
         "gap:12px;margin:1.8em 0}"
         ".w-gal-i{margin:0}"
         ".w-gal-i img{width:100%;height:170px;object-fit:cover;display:block;"
         "border-radius:10px;border:1px solid var(--line);transition:transform .16s ease}"
         ".w-gal-i img:hover{transform:scale(1.02)}"
         ".w-gal-i figcaption{color:var(--mut);font-size:.82rem;margin-top:.45em}"
         /* ---- quote ---- */
         ".w-quote{margin:1.8em 0;padding:22px 24px;border:1px solid var(--a-rule);"
         "border-left:4px solid var(--a);border-radius:0 12px 12px 0;"
         "background:var(--a-tint)}"
         ".w-quote p{margin:0 0 12px;font-size:1.12rem;line-height:1.55;"
         "letter-spacing:-.008em}"
         ".w-quote footer{display:flex;flex-wrap:wrap;gap:8px;align-items:baseline;"
         "font-size:.88rem}"
         ".w-quote-who{font-weight:650}"
         ".w-quote-role{color:var(--mut)}"
         /* ---- stats ---- */
         ".w-stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));"
         "gap:18px;margin:1.8em 0;padding:0}"
         ".w-stat{margin:0;padding:16px 18px;border:1px solid var(--line);border-radius:12px;"
         "background:var(--surface)}"
         ".w-stat-v{margin:0;font-size:1.85rem;font-weight:760;letter-spacing:-.025em;"
         "color:var(--a-text);font-variant-numeric:tabular-nums}"
         ".w-stat-l{margin:.25em 0 0;color:var(--mut);font-size:.88rem}"
         /* ---- mid-page call to action ---- */
         ".w-cta{display:flex;flex-wrap:wrap;gap:18px;align-items:center;"
         "justify-content:space-between;margin:2em 0;padding:24px 26px;border-radius:14px;"
         "border:1px solid var(--a-rule);"
         "background:linear-gradient(135deg,var(--a-tint2),var(--a-tint))}"
         ".w-cta-h{margin:0;font-size:1.22rem;font-weight:720;letter-spacing:-.015em}"
         ".w-cta-s{margin:.3em 0 0;color:var(--mut);font-size:.94rem}"
         /* ---- table ---- */
         ".w-table-wrap{overflow-x:auto;margin:1.8em 0;border:1px solid var(--line);"
         "border-radius:12px;background:var(--surface)}"
         ".w-table{width:100%;border-collapse:collapse;font-size:.93rem}"
         ".w-table th,.w-table td{padding:11px 15px;text-align:left;"
         "border-bottom:1px solid var(--line)}"
         ".w-table thead th{background:var(--a-tint);font-weight:670;font-size:.82rem;"
         "letter-spacing:.04em;text-transform:uppercase;color:var(--a-text)}"
         ".w-table tbody tr:last-child td{border-bottom:0}"
         ".w-table tbody tr:hover{background:var(--a-tint)}"
         ".w-table td{font-variant-numeric:tabular-nums}"
         /* ---- spacer ---- */
         ".w-sp-s{height:18px}.w-sp-m{height:44px}.w-sp-l{height:88px}"
         ".w-map{margin:1.6em 0}.w-map iframe{border-radius:6px}"
         ".w-map-link{font-size:.85rem;margin:.5em 0 0}"
         ".w-post{padding:0 0 1.4em;margin-bottom:1.4em;border-bottom:1px solid var(--line)}"
         ".w-post:last-child{border-bottom:0}"
         ".w-post h2.w-h{margin-top:0}.w-post h2 a{color:inherit;text-decoration:none}"
         ".w-post h2 a:hover{color:var(--a)}"
         ".w-post-meta{color:var(--mut);font-size:.85rem;margin:.2em 0 .7em}"
         ".w-post-back{font-size:.85rem;margin:0 0 .6em}"
         ".w-post-back a{color:var(--mut);text-decoration:none}"
         ".w-form{display:grid;gap:14px;max-width:520px;margin:1.6em 0}"
         ".w-field{display:grid;gap:5px}"
         ".w-label{font-size:.85rem;font-weight:600;color:var(--mut)}"
         ".w-form input,.w-form textarea,.w-form select{font:inherit;padding:9px 11px;"
         "border:1px solid var(--line);border-radius:6px;background:var(--surface);color:var(--ink);width:100%}"
         ".w-form button{justify-self:start;border:0;cursor:pointer}"
         ".w-hp{position:absolute;left:-9999px;width:1px;height:1px;overflow:hidden}"
         ".w-form-msg{margin:0;font-size:.9rem}"
         /* hero */
         /* the hero sits on a washed accent band that bleeds past the column */
         /* The bleed is exactly the wrapper's own padding — a larger negative
            margin puts the hero past the viewport edge and scrolls the whole
            page sideways once the column stops being narrower than the screen. */
         ".w-hero{position:relative;padding:38px 22px 32px;margin:0 -22px 40px;"
         "border-radius:14px;border:1px solid var(--a-rule);"
         "background:linear-gradient(155deg,var(--a-soft),var(--bg) 72%)}"
         "@media(max-width:640px){.w-hero{margin-left:0;margin-right:0;padding:28px 18px}}"
         ".w-hero-eyebrow{display:inline-flex;align-items:center;gap:8px;font-size:.72rem;"
         "letter-spacing:.16em;text-transform:uppercase;color:var(--a-text);"
         "font-weight:700;margin:0 0 12px}"
         ".w-hero-eyebrow:before{content:'';width:20px;height:2px;border-radius:2px;"
         "background:var(--a)}"
         ".w-hero-h{font-size:clamp(1.9rem,5vw,3rem);line-height:1.06;letter-spacing:-.022em;"
         "margin:0 0 14px;text-wrap:balance;font-weight:780}"
         ".w-hero-sub{font-size:1.1rem;color:var(--mut);max-width:60ch;margin:0 0 22px}"
         ".w-hero-cta{display:flex;flex-wrap:wrap;gap:12px;margin:0}"
         ".w-btn-ghost{display:inline-block;padding:10px 20px;border-radius:7px;font-weight:600;"
         "font-size:.95rem;text-decoration:none;color:var(--a-text);background:var(--surface);"
         "border:1px solid var(--a-rule)}"
         ".w-btn-ghost:hover{background:var(--a-tint2);border-color:var(--a)}"
         /* pricing */
         ".w-plans{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));"
         "gap:20px;margin:1.8em 0}"
         ".w-plan{position:relative;background:var(--surface);border:1px solid var(--line);"
         "border-radius:12px;padding:24px;transition:transform .14s ease,box-shadow .14s ease}"
         ".w-plan:hover{transform:translateY(-2px);box-shadow:0 8px 24px rgba(0,0,0,.10)}"
         /* the highlighted plan gets a washed accent ground and a coloured cap */
         ".w-plan.is-feat{border-color:var(--a-rule);"
         "background:linear-gradient(180deg,var(--a-tint2),var(--a-tint) 42%,var(--surface));"
         "box-shadow:0 6px 22px rgba(0,0,0,.09)}"
         /* The cap is clipped to the card's own radius rather than by
            overflow:hidden — that clipped the badge, which overhangs the top
            edge on purpose. */
         ".w-plan.is-feat:before{content:'';position:absolute;left:-1px;right:-1px;top:-1px;"
         "height:4px;border-radius:12px 12px 0 0;"
         "background:linear-gradient(90deg,var(--a),var(--a-deep))}"
         ".w-plan.is-feat .w-plan-price{color:var(--a-text)}"
         ".w-plan-badge{position:absolute;z-index:2;top:-11px;left:22px;background:var(--a);"
         "color:var(--on-a);font-size:.68rem;font-weight:700;letter-spacing:.08em;"
         "text-transform:uppercase;padding:3px 10px;border-radius:100px;"
         "box-shadow:0 2px 6px rgba(0,0,0,.16)}"
         ".w-plan-name{margin:0 0 2px;font-size:1.2rem}"
         ".w-plan-size{margin:0 0 14px;color:var(--mut);font-size:.9rem}"
         ".w-plan-price{margin:0 0 16px;font-size:1.9rem;font-weight:750;letter-spacing:-.02em}"
         ".w-plan-per{font-size:.85rem;font-weight:500;color:var(--mut);margin-left:5px}"
         ".w-plan-feats{list-style:none;padding:0;margin:0 0 20px;display:grid;gap:8px}"
         ".w-plan-feats li{padding-left:22px;position:relative;font-size:.92rem}"
         ".w-plan-feats li:before{content:'';position:absolute;left:2px;top:.55em;width:9px;"
         "height:5px;border-left:2px solid var(--a);border-bottom:2px solid var(--a);"
         "transform:rotate(-45deg)}"
         ".w-plan-cta{margin:0}"
         /* steps */
         ".w-steps{list-style:none;counter-reset:s;padding:0;margin:1.8em 0;display:grid;gap:22px}"
         ".w-step{counter-increment:s;position:relative;padding-left:52px}"
         /* the trail that makes the numbers read as a sequence rather than bullets */
         ".w-step:not(:last-child):after{content:'';position:absolute;left:16px;top:36px;"
         "bottom:-22px;width:2px;background:var(--a-rule)}"
         ".w-step:before{content:counter(s);position:absolute;left:0;top:-2px;width:34px;"
         "height:34px;border-radius:50%;background:var(--a);color:var(--on-a);display:flex;"
         "align-items:center;justify-content:center;font-weight:700;font-size:.95rem}"
         ".w-step-h{margin:0 0 4px;font-size:1.05rem}"
         /* faq */
         ".w-faq{margin:1.6em 0;border:1px solid var(--line);border-radius:12px;"
         "overflow:hidden;background:var(--surface)}"
         ".w-faq-i{border-bottom:1px solid var(--line)}"
         ".w-faq-i:last-child{border-bottom:0}"
         ".w-faq-i summary{cursor:pointer;padding:15px 18px;font-weight:620;list-style:none;"
         "display:flex;align-items:center;gap:11px}"
         ".w-faq-i summary:hover{background:var(--a-tint)}"
         ".w-faq-i[open] summary{background:var(--a-tint);color:var(--a-text)}"
         ".w-faq-i summary::-webkit-details-marker{display:none}"
         ".w-faq-i summary:before{content:'+';display:flex;align-items:center;"
         "justify-content:center;width:21px;height:21px;flex:none;border-radius:50%;"
         "background:var(--a);color:var(--on-a);font-weight:700;font-size:.95rem;line-height:1}"
         ".w-faq-i[open] summary:before{content:'\\2212'}"
         ".w-faq-i .w-p{padding:2px 18px 16px 50px;margin:0;color:var(--mut)}"
         /* ---- interaction states ----
            The site previously had NO focus rule anywhere, so every link,
            button and disclosure fell back to the browser default — which on a
            tinted bar or a dark ground is frequently invisible. A keyboard
            user could not see where they were.

            :focus-visible rather than :focus, so a mouse click does not leave
            a ring behind on something the user has already finished with. */
         "a:focus-visible,button:focus-visible,summary:focus-visible,"
         "[tabindex]:focus-visible{outline:3px solid var(--a);outline-offset:3px;"
         "border-radius:5px}"
         /* A text field is the exception: clicking into one must look like
            something happened, so it answers plain :focus. */
         ".w-form input:focus,.w-form textarea:focus,.w-form select:focus{"
         "outline:none;border-color:var(--a);box-shadow:0 0 0 3px var(--a-tint2)}"
         ".w-btn:active{transform:translateY(0);box-shadow:0 1px 2px rgba(0,0,0,.14)}"
         ".w-btn-ghost:active,.w-login:active{transform:translateY(1px)}"
         ".w-plan-feats li::marker{color:var(--a)}"
         "::selection{background:var(--a-tint2);color:var(--ink)}"
         "@media(prefers-reduced-motion:reduce){*{transition:none!important;"
         "animation:none!important}.w-btn:hover,.w-plan:hover{transform:none}}"
         /* footer: a tinted band, closed with the accent */
         "footer.w-foot{border-top:3px solid var(--a);background:var(--a-tint);"
         "padding:26px 0;color:var(--mut);font-size:.85rem}"
         "footer.w-foot a{color:var(--a-text)}"
         ".w-draft{background:#fbeedd;border-bottom:1px solid #e5c9a3;color:#8a4b06;"
         "padding:9px 0;font-size:.85rem}"
      << "</style></head><body>";

    if (!banner.empty()) h << banner;
    // The staff sign-in link. docs/120 argued for leaving it off — advertising
    // the admin door to every crawler buys nothing — and the owner asked for it
    // anyway, which is their call to make. `website.login_link` turns it off
    // again without a rebuild, and the login page's own rate limiting and
    // uniform "Invalid credentials" are what actually defend it.
    std::string loginHtml;
    if (s.showLogin)
        loginHtml = "<a class=\"w-login\" href=\"/login\" rel=\"nofollow\">Sign in</a>";

    h << "<header class=\"w-top\"><div class=\"w-wrap\">"
      << "<a class=\"w-brand\" href=\"/site\">" << R::esc(s.name) << "</a>"
      << "<div class=\"w-navwrap\">" << menuHtml << loginHtml << "</div>"
      << "</div></header>"
      << "<main class=\"w-wrap\">" << bodyHtml << "</main>"
      << "<footer class=\"w-foot\"><div class=\"w-wrap\">"
      << R::esc(s.footer.empty() ? s.name : s.footer)
      << "</div></footer>"
      << editor          // empty unless the caller may edit (docs/117)
      << "</body></html>";
    return h.str();
}

} // anonymous namespace

// ================================================================
// MODULE
// ================================================================
WebsiteModule::WebsiteModule(core::ModelFactory& m, core::ServiceFactory& s,
                             core::ViewModelFactory& vm, core::ViewFactory& v)
    : models_(m), services_(s), viewModels_(vm), views_(v) {}

std::string WebsiteModule::moduleName() const { return "website"; }
std::string WebsiteModule::version()    const { return "1.0"; }
std::vector<std::string> WebsiteModule::dependencies() const { return {"base", "ir"}; }

void WebsiteModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("website.page", [db]{ return std::make_shared<WebsitePage>(db); });
    models_.registerCreator("website.menu", [db]{ return std::make_shared<WebsiteMenu>(db); });
    WebsiteForm::registerModels(models_, db);
}
void WebsiteModule::registerServices() {}
void WebsiteModule::registerViews()    {}

void WebsiteModule::registerViewModels() {
    auto db = services_.db();
    viewModels_.registerCreator("website.page", [db]{ return std::make_shared<GenericViewModel<WebsitePage>>(db); });
    viewModels_.registerCreator("website.menu", [db]{ return std::make_shared<GenericViewModel<WebsiteMenu>>(db); });
    WebsiteForm::registerViewModels(viewModels_, db);
}

void WebsiteModule::registerRoutes() {
    auto db = services_.db();
    const bool devMode = services_.devMode();
    auto sessions = services_.sessions();

    // The public form-submission endpoint (docs/116 A1).
    WebsiteForm::registerRoutes(db, devMode, services_.trustedProxies());

    auto notFound = [](std::function<void(const drogon::HttpResponsePtr&)>& cb,
                       const SiteSettings& s, const std::string& menu) {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setStatusCode(drogon::k404NotFound);
        r->setContentTypeCode(drogon::CT_TEXT_HTML);
        r->addHeader("X-Content-Type-Options", "nosniff");
        r->setBody(pageShell(s, "Page not found", "", "", "/site", false, menu,
                             "<h1 class=\"w-h\">Page not found</h1>"
                             "<p class=\"w-p\">That page does not exist, or is not published.</p>"));
        cb(r);
    };

    // ---- the page renderer, shared by "/site" and "/site/<slug>" ----
    auto servePage = [db, notFound, devMode](const drogon::HttpRequestPtr& req,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                    std::string slug, bool homepage,
                                    bool staffPreview, bool canEdit, bool isAdmin)
    {
        try {
            auto conn = db->acquire();
            pqxx::work txn{conn.get()};
            const SiteSettings s = loadSettings(txn);
            const std::string menu = renderMenu(txn);

            if (!homepage && !WebsiteRender::isValidSlug(slug)) { auto c = cb; notFound(c, s, menu); return; }

            auto rows = homepage
                ? txn.exec("SELECT id, slug, title, blocks_json, is_published, is_indexed, "
                           "       meta_title, meta_description, meta_keywords, "
                           "       page_kind, COALESCE(author,'') AS author, "
                           "       to_char(publish_date,'YYYY-MM-DD') AS pdate "
                           "  FROM website_page WHERE is_homepage = TRUE "
                           " ORDER BY id LIMIT 1")
                : txn.exec("SELECT id, slug, title, blocks_json, is_published, is_indexed, "
                           "       meta_title, meta_description, meta_keywords, "
                           "       page_kind, COALESCE(author,'') AS author, "
                           "       to_char(publish_date,'YYYY-MM-DD') AS pdate "
                           "  FROM website_page WHERE slug = $1 LIMIT 1",
                           pqxx::params{slug});

            if (rows.empty()) { auto c = cb; notFound(c, s, menu); return; }
            const auto& p = rows[0];
            const bool published = !p["is_published"].is_null() && p["is_published"].as<bool>(false);

            // A draft page is 404 to the public — the SAME answer as a page
            // that does not exist, so the URL space cannot be probed for
            // unreleased content. Staff with a live session may preview it.
            if (!published && !staffPreview) { auto c = cb; notFound(c, s, menu); return; }

            const std::string blocksRaw = sOr(p["blocks_json"]);
            nlohmann::json blocks = nlohmann::json::array();
            try { auto v = nlohmann::json::parse(blocksRaw.empty() ? "[]" : blocksRaw);
                  if (v.is_array()) blocks = v; } catch (...) { /* render nothing */ }

            // A `form` block names a form by slug; the form's own HTML is
            // built from its stored field definitions, never from the page.
            // The renderer stays free of a database of its own — it calls
            // back here — while the form still lands in the block's position.
            auto formResolver = [&txn](const std::string& slug) {
                return WebsiteForm::renderForm(txn, slug);
            };

            const std::string title    = sOr(p["title"]);
            const std::string metaT    = sOr(p["meta_title"]);
            // The homepage's canonical URL is the BARE DOMAIN, not /site.
            //
            // Both paths serve it, so one of them has to be declared the real
            // one. Now that "/" is the front door (docs/126), pointing the
            // canonical at /site would tell every search engine to index
            // easylockerspace.com/site as the homepage and treat the domain
            // root — the URL on the business cards — as a duplicate of it.
            const std::string canonical = homepage ? "/" : "/site/" + sOr(p["slug"]);
            const bool indexed = published &&
                                 !p["is_indexed"].is_null() && p["is_indexed"].as<bool>(false);

            std::string banner;
            if (!published)
                banner = "<div class=\"w-draft\"><div class=\"w-wrap\">"
                         "Draft &mdash; visible to you because you are signed in. "
                         "The public sees a 404 until this page is published."
                         "</div></div>";

            auto res = drogon::HttpResponse::newHttpResponse();
            res->setStatusCode(drogon::k200OK);
            res->setContentTypeCode(drogon::CT_TEXT_HTML);
            res->addHeader("X-Content-Type-Options", "nosniff");
            res->addHeader("Referrer-Policy", "strict-origin-when-cross-origin");
            if (!published) res->addHeader("Cache-Control", "private, no-store");
            // A post carries its own title, date and author above the content;
            // an ordinary page does not, because a page is not dated.
            std::string head;
            if (sOr(p["page_kind"]) == "post") {
                head = "<p class=\"w-post-back\"><a href=\"/site/blog\">&larr; All news</a></p>"
                       "<h1 class=\"w-h\">" + WebsiteRender::esc(title) + "</h1>";
                const std::string d = sOr(p["pdate"]), a = sOr(p["author"]);
                if (!d.empty() || !a.empty()) {
                    head += "<p class=\"w-post-meta\">" + WebsiteRender::esc(d);
                    if (!a.empty()) head += (d.empty() ? "" : " &middot; ") + WebsiteRender::esc(a);
                    head += "</p>";
                }
            }

            // THE EDITOR IS EMITTED ONLY FOR SOMEONE WHO MAY USE IT (docs/117).
            // A visitor's HTML does not contain the bar, the script, or the
            // page id. That is not the security control — the save endpoint
            // is — but a feature nobody unauthorised can even see is a feature
            // nobody unauthorised goes looking for.
            std::string editor;
            if (canEdit) {
                editor = "<script>window.__WSITE_EDIT={page_id:" +
                         std::to_string(p["id"].as<int>()) +
                         ",admin:" + (isAdmin ? "true" : "false") + "};</script>"
                         "<script src=\"/website-editor.js\" defer=\"defer\"></script>";
            }

            // ---- JSON-LD (docs/118 E2) ----
            //
            // Built as a JSON VALUE and serialised by the library, never
            // concatenated. A page title containing a quote or the literal
            // "</script>" would break a hand-rolled string straight out of the
            // script block — which is the mistake hand-rolled JSON-LD always
            // makes. nlohmann escapes it; the `/` in any closing tag inside a
            // string stays inside the string.
            std::string jsonLd;
            {
                const std::string url = s.baseUrl + canonical;
                nlohmann::json org{
                    {"@type", "Organization"},
                    {"name", s.name},
                    {"url",  s.baseUrl + "/site"},
                };
                nlohmann::json site{
                    {"@type", "WebSite"},
                    {"name", s.name},
                    {"url",  s.baseUrl + "/site"},
                };
                nlohmann::json crumbs = nlohmann::json::array();
                crumbs.push_back({{"@type","ListItem"},{"position",1},
                                  {"name","Home"},{"item", s.baseUrl + "/site"}});
                const bool isPost = sOr(p["page_kind"]) == "post";
                if (isPost)
                    crumbs.push_back({{"@type","ListItem"},{"position",2},
                                      {"name","News"},{"item", s.baseUrl + "/site/blog"}});
                if (!homepage)
                    crumbs.push_back({{"@type","ListItem"},
                                      {"position", isPost ? 3 : 2},
                                      {"name", title},{"item", url}});

                nlohmann::json graph = nlohmann::json::array({org, site,
                    nlohmann::json{{"@type","BreadcrumbList"},{"itemListElement", crumbs}}});

                if (isPost) {
                    nlohmann::json art{
                        {"@type", "Article"},
                        {"headline", title},
                        {"mainEntityOfPage", url},
                        {"publisher", org},
                    };
                    const std::string d = sOr(p["pdate"]), a = sOr(p["author"]);
                    if (!d.empty()) art["datePublished"] = d;
                    if (!a.empty()) art["author"] = nlohmann::json{{"@type","Person"},{"name", a}};
                    const std::string ds = sOr(p["meta_description"]);
                    if (!ds.empty()) art["description"] = ds;
                    graph.push_back(art);
                }
                jsonLd = nlohmann::json{{"@context","https://schema.org"},
                                        {"@graph", graph}}.dump();

                // Serialising is necessary but NOT sufficient. nlohmann escapes
                // quotes and backslashes but leaves '/' alone — which is valid
                // JSON and fatal here: a page title containing the literal
                // "</script>" would close this block early and everything after
                // it would be parsed as markup.
                //
                // "\/" is a legal JSON escape for "/", so rewriting "</" keeps
                // the document valid while making the sequence inert. This is
                // the step hand-rolled AND library-serialised JSON-LD both miss.
                for (std::size_t pos = 0;
                     (pos = jsonLd.find("</", pos)) != std::string::npos;
                     pos += 3)
                    jsonLd.replace(pos, 2, "<\\/");
            }

            res->setBody(pageShell(s, metaT.empty() ? title : metaT,
                                   sOr(p["meta_description"]), sOr(p["meta_keywords"]),
                                   canonical, indexed, menu,
                                   head + WebsiteRender::blocks(blocks, formResolver),
                                   banner, editor, jsonLd));
            cb(res);
        } catch (const PoolExhaustedException& e) {
            LOG_ERROR << "[website] pool: " << e.what();
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setStatusCode(drogon::k503ServiceUnavailable);
            r->setContentTypeCode(drogon::CT_TEXT_HTML);
            r->setBody("<html><body><p>The site is busy. Please retry.</p></body></html>");
            cb(r);
        } catch (const std::exception& e) {
            LOG_ERROR << "[website] " << e.what();
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setStatusCode(drogon::k500InternalServerError);
            r->setContentTypeCode(drogon::CT_TEXT_HTML);
            r->setBody(std::string("<html><body><p>") +
                       (devMode ? e.what() : "An internal error occurred") +
                       "</p></body></html>");
            cb(r);
        }
    };

    // What the caller may do on these routes (docs/117 §3).
    //
    // the reference ERP decides this on the SERVER — `editable = uid and is_publisher()` —
    // and so do we. A visitor's HTML never contains the editor at all, and the
    // save endpoint re-checks the same thing rather than trusting that the
    // button was hidden.
    struct Caps { bool staff = false; bool edit = false; bool admin = false;
                  int uid = 0; std::string name; };
    auto capsOf = [sessions](const drogon::HttpRequestPtr& req) -> Caps {
        Caps c;
        if (!sessions) return c;
        const std::string sid = req->getCookie("session_id");
        if (sid.empty()) return c;
        auto s = sessions->get(sid);
        if (!s) return c;
        c.staff = true;
        c.admin = s->isAdmin;
        c.uid   = s->uid;
        // The display name if we have one, otherwise the login — a revision
        // list saying "who" is worth more than one saying "uid 7".
        c.name  = s->name.empty() ? s->login : s->name;
        // Editing the public website is a configuration act, so it rides on
        // the group that already gates the Website menus in Settings. A staff
        // login on its own is NOT enough — that is the distinction the whole
        // feature turns on.
        c.edit  = s->isAdmin || s->hasGroup(Groups::SETTINGS_CONFIGURATION);
        return c;
    };

    drogon::app().registerHandler("/site",
        [servePage, capsOf](const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        { const auto c = capsOf(req);
          servePage(req, std::move(cb), "", true, c.staff, c.edit, c.admin); },
        {drogon::Get});

    // ---- GET / — the same home page, at the front door (docs/126) ----
    //
    // The website is the product's public face, so the bare domain shows it.
    // The ERP moved to /login. /site keeps working: every menu, sitemap entry
    // and stored link already points there, and breaking them to save a path
    // would be a poor trade.
    drogon::app().registerHandler("/",
        [servePage, capsOf, db](const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            // An installation with no website at all — the ERP alone — must
            // not answer its own domain with a 404. If nothing is published as
            // the homepage, hand the front door back to the application.
            bool haveHome = false;
            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                auto r = txn.exec("SELECT 1 FROM website_page "
                                  " WHERE is_homepage = TRUE AND is_published = TRUE LIMIT 1");
                txn.commit();
                haveHome = !r.empty();
            } catch (const std::exception& e) {
                LOG_ERROR << "[website/root] " << e.what();
            }
            if (!haveHome) {
                auto r = drogon::HttpResponse::newRedirectionResponse("/login");
                cb(r);
                return;
            }
            const auto c = capsOf(req);
            servePage(req, std::move(cb), "", true, c.staff, c.edit, c.admin);
        },
        {drogon::Get});

    // ---- GET /site/blog — the post index (docs/116 A4) ----
    //
    // Registered BEFORE the catch-all regex below. Drogon matches the regex
    // ahead of an exact path here, so with the order reversed "/site/blog"
    // was resolved as a page slug and 404'd.
    //
    // Registered BEFORE the catch-all slug route so "blog" resolves here
    // rather than looking for a page with that slug.
    drogon::app().registerHandler("/site/blog",
        [db, devMode](const drogon::HttpRequestPtr&,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                const SiteSettings s = loadSettings(txn);
                const std::string menu = renderMenu(txn);

                // Published posts only, newest first. A post with a FUTURE
                // publish_date is held back — that is what the date is for.
                auto rows = txn.exec(
                    "SELECT slug, title, COALESCE(excerpt,'') AS excerpt, "
                    "       COALESCE(author,'') AS author, "
                    "       to_char(publish_date,'YYYY-MM-DD') AS d "
                    "  FROM website_page "
                    " WHERE page_kind = 'post' AND is_published = TRUE "
                    "   AND (publish_date IS NULL OR publish_date <= CURRENT_DATE) "
                    " ORDER BY publish_date DESC NULLS LAST, id DESC LIMIT 50");

                std::ostringstream body;
                body << "<h1 class=\"w-h\">News</h1>";
                if (rows.empty())
                    body << "<p class=\"w-p\">Nothing published yet.</p>";
                for (const auto& r : rows) {
                    body << "<article class=\"w-post\">"
                         << "<h2 class=\"w-h\"><a href=\"/site/"
                         << WebsiteRender::esc(sOr(r["slug"])) << "\">"
                         << WebsiteRender::esc(sOr(r["title"])) << "</a></h2>";
                    const std::string d = sOr(r["d"]), a = sOr(r["author"]);
                    if (!d.empty() || !a.empty()) {
                        body << "<p class=\"w-post-meta\">" << WebsiteRender::esc(d);
                        if (!a.empty()) body << (d.empty() ? "" : " &middot; ")
                                             << WebsiteRender::esc(a);
                        body << "</p>";
                    }
                    const std::string ex = sOr(r["excerpt"]);
                    if (!ex.empty()) body << "<p class=\"w-p\">" << WebsiteRender::esc(ex) << "</p>";
                    body << "</article>";
                }

                auto res = drogon::HttpResponse::newHttpResponse();
                res->setStatusCode(drogon::k200OK);
                res->setContentTypeCode(drogon::CT_TEXT_HTML);
                res->addHeader("X-Content-Type-Options", "nosniff");
                res->setBody(pageShell(s, "News", "Latest news and announcements.",
                                       "", "/site/blog", true, menu, body.str()));
                cb(res);
            } catch (const std::exception& e) {
                LOG_ERROR << "[website/blog] " << e.what();
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k500InternalServerError);
                r->setContentTypeCode(drogon::CT_TEXT_HTML);
                r->setBody(std::string("<html><body><p>") +
                           (devMode ? e.what() : "An internal error occurred") +
                           "</p></body></html>");
                cb(r);
            }
        },
        {drogon::Get});

    // ----------------------------------------------------------
    // GET/POST /site/api/page/{id}/blocks  (docs/117)
    //
    // Registered BEFORE the catch-all "^/site/(.*)$" below. With the order
    // reversed the GET was swallowed by the page route and answered with the
    // rendered page instead of the blocks — the same trap /site/blog hit.
    //
    // The ONE write path the inline editor has, and THE security control.
    // Hiding the toolbar from a visitor is presentation; this function is
    // what actually decides who may change the public website, so it repeats
    // every check rather than trusting that the button was hidden.
    // ----------------------------------------------------------
    drogon::app().registerHandlerViaRegex("^/site/api/page/([0-9]+)/blocks$",
        [db, devMode, capsOf](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& idStr)
        {
            auto res = drogon::HttpResponse::newHttpResponse();
            res->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            res->addHeader("X-Content-Type-Options", "nosniff");
            auto fail = [&](drogon::HttpStatusCode code, const std::string& m) {
                res->setStatusCode(code);
                res->setBody(nlohmann::json{{"error", m}}.dump());
                cb(res);
            };

            // 1. Authentication and AUTHORISATION, server-side. A staff login
            //    on its own is not enough — editing the public site is a
            //    configuration act.
            const auto caps = capsOf(req);
            if (!caps.staff) { fail(drogon::k401Unauthorized, "Not authenticated"); return; }
            if (!caps.edit)  {
                fail(drogon::k403Forbidden,
                     "You do not have permission to edit the website.");
                return;
            }

            int pageId = 0;
            try { pageId = std::stoi(idStr); } catch (...) {
                fail(drogon::k400BadRequest, "Invalid page"); return; }

            // GET — hand the editor the blocks to edit. Behind the SAME gate
            // as the write: a page's blocks include unpublished content, so
            // reading them is not a public act either.
            if (req->method() == drogon::Get) {
                try {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    auto r = txn.exec("SELECT blocks_json FROM website_page WHERE id=$1",
                                      pqxx::params{pageId});
                    if (r.empty()) { fail(drogon::k404NotFound, "No such page"); return; }
                    nlohmann::json blocks = nlohmann::json::array();
                    const std::string raw = sOr(r[0][0]);
                    try { auto v = nlohmann::json::parse(raw.empty() ? "[]" : raw);
                          if (v.is_array()) blocks = v; } catch (...) {}
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(nlohmann::json{{"blocks", blocks}}.dump());
                    cb(res);
                } catch (const std::exception& e) {
                    LOG_ERROR << "[website/edit-get] " << e.what();
                    fail(drogon::k500InternalServerError,
                         devMode ? e.what() : "An internal error occurred");
                }
                return;
            }

            // 2. A page is content, not a payload.
            constexpr std::size_t kMaxBody   = 256 * 1024;
            constexpr std::size_t kMaxBlocks = 200;
            if (req->body().size() > kMaxBody) {
                fail(drogon::k400BadRequest, "That page is too large to save."); return; }

            nlohmann::json body;
            try { body = nlohmann::json::parse(req->body()); }
            catch (...) { fail(drogon::k400BadRequest, "Invalid content"); return; }
            auto it = body.find("blocks");
            if (it == body.end() || !it->is_array()) {
                fail(drogon::k400BadRequest, "Content must be a list of blocks."); return; }
            if (it->size() > kMaxBlocks) {
                fail(drogon::k400BadRequest, "A page may hold at most 200 blocks."); return; }

            // 3. Every block must be one the renderer knows. An unknown type
            //    is REFUSED rather than stored: stored, it would render as
            //    nothing and read as data loss.
            static const std::set<std::string> kKnown = {
                "heading","text","image","button","divider","columns",
                "video","gallery","quote","stats","cta","table","spacer",
                "references","map","form","html",
                "hero","pricing","steps","faq"
            };
            for (const auto& b : *it) {
                if (!b.is_object()) {
                    fail(drogon::k400BadRequest, "Each block must be an object."); return; }
                auto ty = b.find("type");
                if (ty == b.end() || !ty->is_string()) {
                    fail(drogon::k400BadRequest, "Each block needs a type."); return; }
                const std::string t = ty->get<std::string>();
                if (!kKnown.count(t)) {
                    fail(drogon::k400BadRequest, "Unknown block type: " + t); return; }
                // 4. The raw-HTML block is the one that carries markup, so it
                //    is ADMIN ONLY — and refused loudly. Dropping it silently
                //    would look like a bug and invite a retry.
                if (t == "html" && !caps.admin) {
                    fail(drogon::k403Forbidden,
                         "Only an administrator may add a raw HTML block.");
                    return;
                }
            }

            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                auto prev = txn.exec(
                    "SELECT COALESCE(blocks_json,'[]') AS b, title "
                    "  FROM website_page WHERE id=$1",
                    pqxx::params{pageId});
                if (prev.empty()) { fail(drogon::k404NotFound, "No such page"); return; }

                // Snapshot what is being replaced, INSIDE the same transaction:
                // a history written separately can be missing exactly the
                // version somebody needs back (docs/118 E1).
                //
                // Only when the content actually differs — clicking Save twice
                // should not manufacture two identical versions to scroll past.
                if (sOr(prev[0]["b"]) != it->dump()) {
                    txn.exec(
                        "INSERT INTO website_page_revision "
                        "  (page_id, blocks_json, title, author_uid, author_name) "
                        "VALUES ($1, $2, $3, NULLIF($4,0), $5)",
                        pqxx::params{pageId, sOr(prev[0]["b"]), sOr(prev[0]["title"]),
                                     caps.uid, caps.name});
                    // Keep the last 20. An unbounded history on a table nobody
                    // vacuums is a slow leak, and nobody restores the 400th.
                    txn.exec(
                        "DELETE FROM website_page_revision "
                        " WHERE page_id=$1 AND id NOT IN ("
                        "   SELECT id FROM website_page_revision "
                        "    WHERE page_id=$1 ORDER BY id DESC LIMIT 20)",
                        pqxx::params{pageId});
                }

                txn.exec("UPDATE website_page SET blocks_json=$2, write_date=now() "
                         " WHERE id=$1",
                         pqxx::params{pageId, it->dump()});
                txn.commit();

                res->setStatusCode(drogon::k200OK);
                res->setBody(nlohmann::json{{"ok", true}, {"blocks", it->size()}}.dump());
                cb(res);
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[website/edit] pool: " << e.what();
                fail(drogon::k503ServiceUnavailable, "The server is busy. Please retry.");
            } catch (const std::exception& e) {
                LOG_ERROR << "[website/edit] " << e.what();
                fail(drogon::k500InternalServerError,
                     devMode ? e.what() : "An internal error occurred");
            }
        },
        {drogon::Get, drogon::Post});

    // ----------------------------------------------------------
    // GET/POST /site/api/theme  (docs/121)
    //
    // The palette. Via regex and BEFORE the catch-all for the same reason the
    // blocks route is — "^/site/(.*)$" would otherwise answer this with a
    // rendered 404 page instead of JSON.
    //
    // Same gate as editing a page: changing the site's colours is a
    // configuration act, so a staff login on its own is not enough. And the
    // same discipline about values — everything here ends up inside a <style>
    // block, so nothing is stored that has not been validated as a hex
    // literal or matched against the preset allowlist.
    // ----------------------------------------------------------
    drogon::app().registerHandlerViaRegex("^/site/api/theme$",
        [db, devMode, capsOf](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            auto res = drogon::HttpResponse::newHttpResponse();
            res->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            res->addHeader("X-Content-Type-Options", "nosniff");
            auto fail = [&](drogon::HttpStatusCode code, const std::string& m) {
                res->setStatusCode(code);
                res->setBody(nlohmann::json{{"error", m}}.dump());
                cb(res);
            };

            const auto caps = capsOf(req);
            if (!caps.staff) { fail(drogon::k401Unauthorized, "Not authenticated"); return; }
            if (!caps.edit)  {
                fail(drogon::k403Forbidden,
                     "You do not have permission to change the website theme.");
                return;
            }

            // The ten token keys a caller may override, and nothing else. The
            // config table is shared with the rest of the ERP, so an
            // unfiltered {key: value} write here would be a write primitive
            // for every setting in the system.
            static const std::map<std::string, std::string> kTokenKeys = {
                {"bg",           "website.color.bg"},
                {"surface",      "website.color.surface"},
                {"ink",          "website.color.ink"},
                {"muted",        "website.color.muted"},
                {"line",         "website.color.line"},
                {"dark.bg",      "website.color.dark.bg"},
                {"dark.surface", "website.color.dark.surface"},
                {"dark.ink",     "website.color.dark.ink"},
                {"dark.muted",   "website.color.dark.muted"},
                {"dark.line",    "website.color.dark.line"},
            };

            try {
                auto conn = db->acquire();

                if (req->method() == drogon::Get) {
                    pqxx::work txn{conn.get()};
                    const SiteSettings s = loadSettings(txn);
                    txn.commit();

                    nlohmann::json presets = nlohmann::json::array();
                    for (const auto& p : WebsitePalette::presets())
                        presets.push_back({
                            {"key", p.key}, {"label", p.label}, {"accent", p.accent},
                            {"bg", p.light.bg}, {"surface", p.light.surface},
                            {"ink", p.light.ink}, {"line", p.light.line},
                            {"dark_bg", p.dark.bg}, {"dark_ink", p.dark.ink},
                        });

                    res->setStatusCode(drogon::k200OK);
                    res->setBody(nlohmann::json{
                        {"theme",     s.themeKey},
                        {"accent",    s.accent},
                        {"on_accent", s.onAccent},
                        // "" means the ink is computed from the accent; a value
                        // means an owner pinned it.
                        {"on_accent_override", s.onAccentOverride},
                        {"dark_mode", WebsitePalette::darkModeToString(s.darkMode)},
                        {"presets",   presets},
                    }.dump());
                    cb(res);
                    return;
                }

                // ---- POST ----
                constexpr std::size_t kMaxBody = 8 * 1024;
                if (req->body().size() > kMaxBody) {
                    fail(drogon::k400BadRequest, "Theme payload too large."); return; }

                nlohmann::json body;
                try { body = nlohmann::json::parse(req->body()); }
                catch (...) { fail(drogon::k400BadRequest, "Invalid content"); return; }
                if (!body.is_object()) {
                    fail(drogon::k400BadRequest, "Invalid content"); return; }

                // Collect every write first, so a bad value rejects the whole
                // request rather than leaving a half-applied palette.
                std::vector<std::pair<std::string, std::string>> writes;

                if (auto it = body.find("theme"); it != body.end()) {
                    if (!it->is_string()) {
                        fail(drogon::k400BadRequest, "theme must be a string"); return; }
                    const std::string key = it->get<std::string>();
                    if (!WebsitePalette::preset(key)) {
                        fail(drogon::k400BadRequest, "Unknown theme: " + key); return; }
                    writes.emplace_back("website.theme", key);
                }

                if (auto it = body.find("accent"); it != body.end()) {
                    if (!it->is_string()) {
                        fail(drogon::k400BadRequest, "accent must be a string"); return; }
                    const std::string raw = it->get<std::string>();
                    if (raw.empty()) {
                        writes.emplace_back("website.accent", "");   // clear the override
                    } else {
                        const std::string hex = WebsitePalette::normalizeHex(raw);
                        if (hex.empty()) {
                            fail(drogon::k400BadRequest,
                                 "accent must be a hex colour like #2f6f9f"); return; }
                        writes.emplace_back("website.accent", hex);
                    }
                }

                if (auto it = body.find("on_accent"); it != body.end()) {
                    if (!it->is_string()) {
                        fail(drogon::k400BadRequest, "on_accent must be a string"); return; }
                    const std::string raw = it->get<std::string>();
                    if (raw.empty()) {
                        writes.emplace_back("website.on_accent", "");   // back to computed
                    } else {
                        const std::string hex = WebsitePalette::normalizeHex(raw);
                        if (hex.empty()) {
                            fail(drogon::k400BadRequest,
                                 "on_accent must be a hex colour"); return; }
                        writes.emplace_back("website.on_accent", hex);
                    }
                }

                if (auto it = body.find("dark_mode"); it != body.end()) {
                    if (!it->is_string()) {
                        fail(drogon::k400BadRequest, "dark_mode must be a string"); return; }
                    const std::string m = it->get<std::string>();
                    if (m != "auto" && m != "off" && m != "on") {
                        fail(drogon::k400BadRequest,
                             "dark_mode must be auto, off or on"); return; }
                    writes.emplace_back("website.dark_mode", m);
                }

                if (auto it = body.find("colors"); it != body.end()) {
                    if (!it->is_object()) {
                        fail(drogon::k400BadRequest, "colors must be an object"); return; }
                    for (const auto& [name, val] : it->items()) {
                        const auto k = kTokenKeys.find(name);
                        if (k == kTokenKeys.end()) {
                            fail(drogon::k400BadRequest, "Unknown colour: " + name); return; }
                        if (!val.is_string()) {
                            fail(drogon::k400BadRequest, "Colour " + name + " must be a string");
                            return; }
                        const std::string raw = val.get<std::string>();
                        if (raw.empty()) { writes.emplace_back(k->second, ""); continue; }
                        const std::string hex = WebsitePalette::normalizeHex(raw);
                        if (hex.empty()) {
                            fail(drogon::k400BadRequest,
                                 "Colour " + name + " must be a hex colour"); return; }
                        writes.emplace_back(k->second, hex);
                    }
                }

                if (writes.empty()) {
                    fail(drogon::k400BadRequest, "Nothing to change."); return; }

                pqxx::work txn{conn.get()};
                for (const auto& [key, value] : writes)
                    txn.exec("INSERT INTO ir_config_parameter (key, value) VALUES ($1,$2) "
                             "ON CONFLICT (key) DO UPDATE "
                             "   SET value = EXCLUDED.value, write_date = now()",
                             pqxx::params{key, value});
                const SiteSettings after = loadSettings(txn);
                txn.commit();

                LOG_INFO << "[website/theme] " << caps.name << " (uid " << caps.uid
                         << ") set theme=" << after.themeKey << " accent=" << after.accent
                         << " dark=" << WebsitePalette::darkModeToString(after.darkMode);

                res->setStatusCode(drogon::k200OK);
                res->setBody(nlohmann::json{
                    {"ok", true},
                    {"theme", after.themeKey},
                    {"accent", after.accent},
                    {"dark_mode", WebsitePalette::darkModeToString(after.darkMode)},
                }.dump());
                cb(res);
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[website/theme] pool: " << e.what();
                fail(drogon::k503ServiceUnavailable, "The server is busy. Please retry.");
            } catch (const std::exception& e) {
                LOG_ERROR << "[website/theme] " << e.what();
                fail(drogon::k500InternalServerError,
                     devMode ? e.what() : "An internal error occurred");
            }
        },
        {drogon::Get, drogon::Post});

    // ----------------------------------------------------------
    // The media library (docs/124)
    //
    //   POST /site/api/media   upload      (edit permission)
    //   GET  /site/api/media   list        (edit permission)
    //   GET  /site/media/{id}  serve       (PUBLIC)
    //
    // The serve route is the one to read carefully: it is the only route in
    // this module that hands attacker-supplied BYTES to a visitor, so it
    // re-checks everything rather than trusting that the upload route did.
    // ----------------------------------------------------------
    drogon::app().registerHandlerViaRegex("^/site/api/media$",
        [db, devMode, capsOf](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            auto res = drogon::HttpResponse::newHttpResponse();
            res->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            res->addHeader("X-Content-Type-Options", "nosniff");
            auto fail = [&](drogon::HttpStatusCode code, const std::string& m) {
                res->setStatusCode(code);
                res->setBody(nlohmann::json{{"error", m}}.dump());
                cb(res);
            };

            const auto caps = capsOf(req);
            if (!caps.staff) { fail(drogon::k401Unauthorized, "Not authenticated"); return; }
            if (!caps.edit)  { fail(drogon::k403Forbidden,
                                    "You do not have permission to manage website images."); return; }

            try {
                auto conn = db->acquire();

                if (req->method() == drogon::Get) {
                    pqxx::work txn{conn.get()};
                    auto rows = txn.exec(
                        "SELECT id, name, mimetype, file_size "
                        "  FROM ir_attachment "
                        " WHERE res_model = 'website' AND public = TRUE AND type = 'binary' "
                        " ORDER BY id DESC LIMIT 200");
                    txn.commit();
                    nlohmann::json list = nlohmann::json::array();
                    for (const auto& r : rows)
                        list.push_back({
                            {"id",   r["id"].as<int>()},
                            {"name", sOr(r["name"])},
                            {"mime", sOr(r["mimetype"])},
                            {"size", r["file_size"].as<long long>(0)},
                            {"url",  "/site/media/" + std::to_string(r["id"].as<int>())},
                        });
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(nlohmann::json{{"images", list}}.dump());
                    cb(res);
                    return;
                }

                // ---- POST: the body IS the file ----
                const std::string bytes{req->body()};
                if (bytes.empty()) { fail(drogon::k400BadRequest, "No file was sent."); return; }

                // THE check. Not the Content-Type header, not the extension —
                // the bytes. An SVG, an HTML file or a PDF renamed .png dies
                // here, before it is ever stored.
                const std::string mime = WebsiteMedia::sniff(bytes);
                if (mime.empty()) {
                    fail(drogon::k400BadRequest,
                         "That is not a PNG, JPEG, GIF, WebP, MP4 or WebM file. "
                         "SVG is not accepted because it can carry script.");
                    return;
                }
                // The ceiling depends on WHAT it is, which is why it is
                // checked after the sniff rather than before it.
                if (static_cast<long long>(bytes.size()) > WebsiteMedia::maxBytesFor(mime)) {
                    fail(drogon::k400BadRequest,
                         WebsiteMedia::isVideo(mime)
                             ? "That video is larger than 24 MB. Longer clips belong on "
                               "YouTube or Vimeo — use a video block with the link."
                             : "That image is larger than 8 MB.");
                    return;
                }
                const std::string name =
                    WebsiteMedia::safeName(req->getParameter("name"), mime);

                const auto stored = core::Filestore::put(bytes);
                pqxx::work txn{conn.get()};
                auto ins = txn.exec(
                    "INSERT INTO ir_attachment "
                    "  (name, res_model, type, mimetype, file_size, checksum, "
                    "   store_fname, public, company_id) "
                    "VALUES ($1,'website','binary',$2,$3,$4,$5,TRUE,1) RETURNING id",
                    pqxx::params{name, mime, static_cast<long long>(bytes.size()),
                                 stored.checksum, stored.storeFname});
                const int id = ins[0][0].as<int>();
                txn.commit();

                LOG_INFO << "[website/media] " << caps.name << " (uid " << caps.uid
                         << ") uploaded #" << id << " " << mime << " " << bytes.size() << "B";

                res->setStatusCode(drogon::k200OK);
                res->setBody(nlohmann::json{
                    {"ok", true}, {"id", id}, {"name", name}, {"mime", mime},
                    {"url", "/site/media/" + std::to_string(id)},
                }.dump());
                cb(res);
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[website/media] pool: " << e.what();
                fail(drogon::k503ServiceUnavailable, "The server is busy. Please retry.");
            } catch (const std::exception& e) {
                LOG_ERROR << "[website/media] " << e.what();
                fail(drogon::k500InternalServerError,
                     devMode ? e.what() : "An internal error occurred");
            }
        },
        {drogon::Get, drogon::Post});

    // ---- PUBLIC: serve one image ----
    drogon::app().registerHandlerViaRegex("^/site/media/([0-9]+)$",
        [db, devMode](
            const drogon::HttpRequestPtr&,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& idStr)
        {
            auto err = [&cb](int code, const std::string& m) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                r->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                r->addHeader("X-Content-Type-Options", "nosniff");
                r->setBody(m);
                cb(r);
            };
            int id = 0;
            try { id = std::stoi(idStr); } catch (...) { err(400, "Invalid id"); return; }

            try {
                std::string mime, store, name;
                {
                    auto conn = db->acquire();
                    pqxx::work txn{conn.get()};
                    // Every condition here is load-bearing. public=TRUE is the
                    // owner's decision to publish; res_model='website' keeps
                    // this route away from attachments belonging to invoices,
                    // expenses or anything else the ERP stores.
                    auto r = txn.exec(
                        "SELECT mimetype, COALESCE(store_fname,''), name "
                        "  FROM ir_attachment "
                        " WHERE id = $1 AND public = TRUE "
                        "   AND res_model = 'website' AND type = 'binary'",
                        pqxx::params{id});
                    txn.commit();
                    if (r.empty()) { err(404, "Not found"); return; }
                    mime  = sOr(r[0][0]);
                    store = sOr(r[0][1]);
                    name  = sOr(r[0][2]);
                }

                // Re-check the type on the way OUT as well. If a row ever
                // acquired a different mimetype by any route, this refuses to
                // be the thing that serves text/html from our own origin.
                if (WebsiteMedia::extensionFor(mime).empty()) {
                    err(404, "Not found"); return; }

                const std::string bytes = core::Filestore::get(store);
                if (bytes.empty()) { err(404, "Not found"); return; }

                // And once more from the bytes themselves, because the file on
                // disk is the thing actually being sent.
                if (WebsiteMedia::sniff(bytes) != mime) { err(404, "Not found"); return; }

                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                resp->setContentTypeString(mime);
                resp->addHeader("X-Content-Type-Options", "nosniff");
                resp->addHeader("Content-Disposition",
                                "inline; filename=\"" +
                                WebsiteMedia::safeName(name, mime) + "\"");
                resp->addHeader("Cache-Control", "public, max-age=86400");
                resp->setBody(bytes);
                cb(resp);
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[website/media-serve] pool: " << e.what();
                err(503, "The server is busy. Please retry.");
            } catch (const std::exception& e) {
                LOG_ERROR << "[website/media-serve] " << e.what();
                err(devMode ? 500 : 404, devMode ? e.what() : "Not found");
            }
        },
        {drogon::Get});

    // ----------------------------------------------------------
    // GET  /site/api/page/{id}/revisions        — list them
    // POST /site/api/page/{id}/revisions/{rid}  — restore one
    //
    // Behind the SAME group as editing: a revision holds page content,
    // including drafts, so reading the history is not a public act either.
    // ----------------------------------------------------------
    drogon::app().registerHandlerViaRegex("^/site/api/page/([0-9]+)/revisions/?([0-9]*)$",
        [db, devMode, capsOf](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& idStr, const std::string& revStr)
        {
            auto res = drogon::HttpResponse::newHttpResponse();
            res->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            res->addHeader("X-Content-Type-Options", "nosniff");
            auto fail = [&](drogon::HttpStatusCode code, const std::string& m) {
                res->setStatusCode(code);
                res->setBody(nlohmann::json{{"error", m}}.dump());
                cb(res);
            };

            const auto caps = capsOf(req);
            if (!caps.staff) { fail(drogon::k401Unauthorized, "Not authenticated"); return; }
            if (!caps.edit)  { fail(drogon::k403Forbidden,
                                    "You do not have permission to edit the website."); return; }

            int pageId = 0;
            try { pageId = std::stoi(idStr); } catch (...) {
                fail(drogon::k400BadRequest, "Invalid page"); return; }

            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                auto page = txn.exec("SELECT COALESCE(blocks_json,'[]') FROM website_page WHERE id=$1",
                                     pqxx::params{pageId});
                if (page.empty()) { fail(drogon::k404NotFound, "No such page"); return; }

                // ---- list ----
                if (req->method() == drogon::Get) {
                    auto rows = txn.exec(
                        "SELECT id, COALESCE(author_name,'') AS who, "
                        "       to_char(create_date,'YYYY-MM-DD HH24:MI') AS when_, "
                        "       length(blocks_json) AS size, "
                        "       COALESCE(note,'') AS note "
                        "  FROM website_page_revision WHERE page_id=$1 "
                        " ORDER BY id DESC",
                        pqxx::params{pageId});
                    nlohmann::json out = nlohmann::json::array();
                    for (const auto& r : rows)
                        out.push_back({{"id", r["id"].as<int>()},
                                       {"author", sOr(r["who"])},
                                       {"at", sOr(r["when_"])},
                                       {"size", r["size"].as<int>(0)},
                                       // "before restore" tells a reader why a
                                       // version exists — without it the entry
                                       // looks like an edit nobody remembers.
                                       {"note", sOr(r["note"])}});
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(nlohmann::json{{"revisions", out}}.dump());
                    cb(res);
                    return;
                }

                // ---- restore ----
                int revId = 0;
                try { revId = std::stoi(revStr); } catch (...) { revId = 0; }
                if (revId <= 0) { fail(drogon::k400BadRequest, "Which revision?"); return; }

                // Scoped by page_id as well as id: a revision id from another
                // page must not be restorable onto this one.
                auto rev = txn.exec(
                    "SELECT blocks_json FROM website_page_revision "
                    " WHERE id=$1 AND page_id=$2",
                    pqxx::params{revId, pageId});
                if (rev.empty()) { fail(drogon::k404NotFound, "No such revision"); return; }

                // Restoring is itself a change, so the CURRENT content is
                // snapshotted first — undoing an undo has to work too.
                txn.exec(
                    "INSERT INTO website_page_revision "
                    "  (page_id, blocks_json, title, author_uid, author_name, note) "
                    "SELECT id, COALESCE(blocks_json,'[]'), title, NULLIF($2,0), $3, 'before restore' "
                    "  FROM website_page WHERE id=$1",
                    pqxx::params{pageId, caps.uid, caps.name});
                txn.exec("UPDATE website_page SET blocks_json=$2, write_date=now() WHERE id=$1",
                         pqxx::params{pageId, sOr(rev[0][0])});
                txn.exec(
                    "DELETE FROM website_page_revision "
                    " WHERE page_id=$1 AND id NOT IN ("
                    "   SELECT id FROM website_page_revision "
                    "    WHERE page_id=$1 ORDER BY id DESC LIMIT 20)",
                    pqxx::params{pageId});
                txn.commit();

                res->setStatusCode(drogon::k200OK);
                res->setBody(nlohmann::json{{"ok", true}, {"restored", revId}}.dump());
                cb(res);
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[website/rev] pool: " << e.what();
                fail(drogon::k503ServiceUnavailable, "The server is busy. Please retry.");
            } catch (const std::exception& e) {
                LOG_ERROR << "[website/rev] " << e.what();
                fail(drogon::k500InternalServerError,
                     devMode ? e.what() : "An internal error occurred");
            }
        },
        {drogon::Get, drogon::Post});

    // ----------------------------------------------------------
    // GET /site/api/health — what is wrong with the site right now
    // (docs/118 E3). the reference ERP has no built-in site audit.
    //
    // Behind the editor group: the output names unpublished pages, so it is
    // not a public report.
    // ----------------------------------------------------------
    drogon::app().registerHandler("/site/api/health",
        [db, devMode, capsOf](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            auto res = drogon::HttpResponse::newHttpResponse();
            res->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            res->addHeader("X-Content-Type-Options", "nosniff");
            const auto caps = capsOf(req);
            if (!caps.staff) {
                res->setStatusCode(drogon::k401Unauthorized);
                res->setBody(nlohmann::json{{"error","Not authenticated"}}.dump());
                cb(res); return;
            }
            if (!caps.edit) {
                res->setStatusCode(drogon::k403Forbidden);
                res->setBody(nlohmann::json{
                    {"error","You do not have permission to edit the website."}}.dump());
                cb(res); return;
            }
            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                nlohmann::json issues = nlohmann::json::array();

                auto add = [&](const char* kind, const char* sev,
                               const std::string& what, const std::string& detail) {
                    issues.push_back({{"kind", kind}, {"severity", sev},
                                      {"subject", what}, {"detail", detail}});
                };

                // Published pages with no meta description — the text a search
                // engine shows under the link. Without one it invents one.
                for (const auto& r : txn.exec(
                        "SELECT slug, title FROM website_page "
                        " WHERE is_published AND is_indexed "
                        "   AND COALESCE(meta_description,'') = '' ORDER BY id"))
                    add("meta_description", "warning", sOr(r["slug"]),
                        "No meta description — search engines will invent the snippet.");

                // Titles too long for a search result.
                for (const auto& r : txn.exec(
                        "SELECT slug, length(title) AS n FROM website_page "
                        " WHERE is_published AND length(title) > 60 ORDER BY id"))
                    add("title_length", "info", sOr(r["slug"]),
                        "Title is " + sOr(r["n"]) + " characters; about 60 is shown.");

                // Published pages reachable from NO menu. Not an error — a
                // landing page is often deliberately unlinked — so it is
                // reported as information, not a fault.
                for (const auto& r : txn.exec(
                        "SELECT p.slug FROM website_page p "
                        " WHERE p.is_published AND NOT p.is_homepage "
                        "   AND p.page_kind = 'page' "
                        "   AND NOT EXISTS (SELECT 1 FROM website_menu m WHERE m.page_id = p.id) "
                        " ORDER BY p.id"))
                    add("orphan_page", "info", sOr(r["slug"]),
                        "Published but not in any menu — reachable only by its address.");

                // Menu entries pointing at a page that is a draft, so the menu
                // would link into a 404 if the renderer did not hide it.
                for (const auto& r : txn.exec(
                        "SELECT m.name, p.slug FROM website_menu m "
                        "  JOIN website_page p ON p.id = m.page_id "
                        " WHERE NOT p.is_published ORDER BY m.id"))
                    add("menu_to_draft", "warning", sOr(r["name"]),
                        "Points at the unpublished page '" + sOr(r["slug"]) +
                        "', so the entry is hidden from visitors.");

                // Image blocks with no alt text, and buttons pointing at a
                // /site/ slug that does not exist. Both need the blocks parsed,
                // which is why this is a loop rather than one clever query.
                for (const auto& r : txn.exec(
                        "SELECT slug, COALESCE(blocks_json,'[]') AS b FROM website_page "
                        " WHERE is_published ORDER BY id")) {
                    const std::string slug = sOr(r["slug"]);
                    nlohmann::json blocks;
                    try { blocks = nlohmann::json::parse(sOr(r["b"])); } catch (...) { continue; }
                    if (!blocks.is_array()) continue;
                    for (const auto& b : blocks) {
                        if (!b.is_object()) continue;
                        const std::string t = b.value("type", std::string{});
                        if (t == "image") {
                            if (b.value("alt", std::string{}).empty())
                                add("image_no_alt", "warning", slug,
                                    "An image has no alt text — a screen reader and "
                                    "an image search both read that.");
                        } else if (t == "button") {
                            const std::string href = b.value("href", std::string{});
                            if (href.rfind("/site/", 0) == 0) {
                                const std::string target = href.substr(6);
                                if (target != "blog" && WebsiteRender::isValidSlug(target)) {
                                    auto e = txn.exec(
                                        "SELECT 1 FROM website_page WHERE slug=$1 AND is_published",
                                        pqxx::params{target});
                                    if (e.empty())
                                        add("broken_link", "error", slug,
                                            "A button links to /site/" + target +
                                            ", which is not a published page.");
                                }
                            }
                        }
                    }
                }

                // Posts with no excerpt read as a bare headline on the index.
                for (const auto& r : txn.exec(
                        "SELECT slug FROM website_page "
                        " WHERE page_kind='post' AND is_published "
                        "   AND COALESCE(excerpt,'') = '' ORDER BY id"))
                    add("post_no_excerpt", "info", sOr(r["slug"]),
                        "No excerpt — the news index will show only its title.");

                // A form with no fields cannot be submitted.
                for (const auto& r : txn.exec(
                        "SELECT f.slug FROM website_form f "
                        " WHERE f.active AND NOT EXISTS "
                        "   (SELECT 1 FROM website_form_field x WHERE x.form_id = f.id) "
                        " ORDER BY f.id"))
                    add("form_no_fields", "error", sOr(r["slug"]),
                        "This form has no fields, so nothing can be submitted.");

                int err = 0, warn = 0, info = 0;
                for (const auto& i : issues) {
                    const std::string s = i.value("severity", std::string{});
                    if      (s == "error")   ++err;
                    else if (s == "warning") ++warn;
                    else                     ++info;
                }
                txn.commit();

                res->setStatusCode(drogon::k200OK);
                res->setBody(nlohmann::json{
                    {"issues", issues},
                    {"counts", {{"error", err}, {"warning", warn}, {"info", info},
                                {"total", static_cast<int>(issues.size())}}},
                }.dump());
                cb(res);
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[website/health] pool: " << e.what();
                res->setStatusCode(drogon::k503ServiceUnavailable);
                res->setBody(nlohmann::json{{"error","The server is busy."}}.dump());
                cb(res);
            } catch (const std::exception& e) {
                LOG_ERROR << "[website/health] " << e.what();
                res->setStatusCode(drogon::k500InternalServerError);
                res->setBody(nlohmann::json{
                    {"error", devMode ? e.what() : "An internal error occurred"}}.dump());
                cb(res);
            }
        },
        {drogon::Get});

    // Slugs may nest ("about/team"), so this is a regex route rather than a
    // segment parameter.
    drogon::app().registerHandlerViaRegex("^/site/(.*)$",
        [servePage, capsOf](const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                            const std::string& slug)
        { const auto c = capsOf(req);
          servePage(req, std::move(cb), slug, false, c.staff, c.edit, c.admin); },
        {drogon::Get});

    // ---- robots.txt ----
    drogon::app().registerHandler("/robots.txt",
        [db](const drogon::HttpRequestPtr&,
             std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            std::string base = "http://localhost:8069";
            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                base = loadSettings(txn).baseUrl;
            } catch (...) { /* fall back to the default */ }
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            // The private areas are named one by one rather than blanketed.
            //
            // This used to end `Disallow: /`, which was right when "/" was the
            // ERP login and everything public lived under /site. Now "/" IS
            // the homepage (docs/126), and that one line would have quietly
            // excluded the site's front page from every search engine — the
            // kind of regression nobody notices until the traffic goes.
            //
            // Each entry below is a route prefix that exists: the application
            // and its API, the customer portal, the clock-in kiosk, the rental
            // screens, and the editor's own endpoints.
            r->setBody("User-agent: *\n"
                       "Allow: /\n"
                       "Allow: /site\n"
                       "Disallow: /login\n"
                       "Disallow: /web\n"
                       "Disallow: /portal\n"
                       "Disallow: /kiosk\n"
                       "Disallow: /rental\n"
                       "Disallow: /site/api\n"
                       "Sitemap: " + base + "/sitemap.xml\n");
            cb(r);
        },
        {drogon::Get});

    // ---- sitemap.xml — published AND indexed pages only ----
    drogon::app().registerHandler("/sitemap.xml",
        [db](const drogon::HttpRequestPtr&,
             std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setContentTypeString("application/xml");
            try {
                auto conn = db->acquire();
                pqxx::work txn{conn.get()};
                const SiteSettings s = loadSettings(txn);
                auto rows = txn.exec(
                    "SELECT slug, is_homepage, to_char(write_date,'YYYY-MM-DD') AS d "
                    "  FROM website_page "
                    " WHERE is_published = TRUE AND is_indexed = TRUE "
                    // A post dated in the future is scheduled, not published —
                    // it must not be advertised to a crawler before its date.
                    "   AND (page_kind <> 'post' OR publish_date IS NULL "
                    "        OR publish_date <= CURRENT_DATE) "
                    " ORDER BY sequence, id");
                std::ostringstream x;
                x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                  << "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">";
                for (const auto& p : rows) {
                    const bool home = !p["is_homepage"].is_null() && p["is_homepage"].as<bool>(false);
                    // Same rule as the canonical: the homepage is listed as
                    // the bare domain, so the sitemap and the canonical agree.
                    // A sitemap that offers /site while the page declares /
                    // is a contradiction crawlers resolve by ignoring one.
                    x << "<url><loc>" << WebsiteRender::esc(
                            s.baseUrl + (home ? "/" : "/site/" + sOr(p["slug"])))
                      << "</loc><lastmod>" << WebsiteRender::esc(sOr(p["d"]))
                      << "</lastmod></url>";
                }
                // The blog index is a route rather than a page row, so it has
                // to be added by hand — and only when there is something on it.
                auto anyPost = txn.exec(
                    "SELECT 1 FROM website_page WHERE page_kind='post' AND is_published=TRUE "
                    "   AND (publish_date IS NULL OR publish_date <= CURRENT_DATE) LIMIT 1");
                if (!anyPost.empty())
                    x << "<url><loc>" << WebsiteRender::esc(s.baseUrl + "/site/blog")
                      << "</loc></url>";
                x << "</urlset>";
                r->setBody(x.str());
            } catch (const std::exception& e) {
                LOG_ERROR << "[website/sitemap] " << e.what();
                r->setBody("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                           "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\"/>");
            }
            cb(r);
        },
        {drogon::Get});
}

void WebsiteModule::initialize() {
    ensureSchema_();
    seedContent_();
    seedMenus_();
}

void WebsiteModule::ensureSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS website_page (
            id               SERIAL PRIMARY KEY,
            slug             VARCHAR NOT NULL,
            title            VARCHAR NOT NULL,
            blocks_json      TEXT NOT NULL DEFAULT '[]',
            is_published     BOOLEAN NOT NULL DEFAULT FALSE,
            is_indexed       BOOLEAN NOT NULL DEFAULT TRUE,
            is_homepage      BOOLEAN NOT NULL DEFAULT FALSE,
            sequence         INTEGER NOT NULL DEFAULT 10,
            meta_title       VARCHAR,
            meta_description TEXT,
            meta_keywords    VARCHAR,
            create_date      TIMESTAMP NOT NULL DEFAULT now(),
            write_date       TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT website_page_slug_uniq UNIQUE (slug)
        )
    )");
    // At most one homepage. Two would make "/site" depend on insertion order.
    txn.exec("CREATE UNIQUE INDEX IF NOT EXISTS website_page_homepage_uniq "
             "ON website_page ((is_homepage)) WHERE is_homepage = TRUE");
    // Blog fields (docs/116 A4) — a post is a page with a kind.
    txn.exec("ALTER TABLE website_page ADD COLUMN IF NOT EXISTS page_kind VARCHAR NOT NULL DEFAULT 'page'");
    // page_kind selects a RENDERING PATH, so it is constrained in the
    // database rather than only in deserializeFields(). BaseModel::write()
    // writes registered fields straight from the payload, so a check that
    // lives only in the model is a check the generic write path walks past —
    // the same way worked_hours was writable on hr_attendance (docs/113).
    txn.exec("UPDATE website_page SET page_kind='page' "
             " WHERE page_kind NOT IN ('page','post')");
    txn.exec(
        "DO $$ BEGIN "
        "  IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname='website_page_kind_chk') THEN "
        "    ALTER TABLE website_page ADD CONSTRAINT website_page_kind_chk "
        "      CHECK (page_kind IN ('page','post')); "
        "  END IF; "
        "END $$;");
    txn.exec("ALTER TABLE website_page ADD COLUMN IF NOT EXISTS publish_date DATE");
    txn.exec("ALTER TABLE website_page ADD COLUMN IF NOT EXISTS author VARCHAR");
    txn.exec("ALTER TABLE website_page ADD COLUMN IF NOT EXISTS excerpt TEXT");
    txn.exec("CREATE INDEX IF NOT EXISTS website_page_blog_idx "
             "ON website_page (page_kind, is_published, publish_date DESC)");

    // Revision history (docs/118 E1). the reference ERP keeps exactly ONE previous
    // version in ir.ui.view.arch_prev, overwritten on every write; there is no
    // list, no author, and no way back past one step. Since docs/117 gave
    // people a click-to-edit button on a LIVE PUBLIC PAGE, one step of undo is
    // not enough — this keeps the last 20 with who and when.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS website_page_revision (
            id          SERIAL PRIMARY KEY,
            page_id     INTEGER NOT NULL REFERENCES website_page(id) ON DELETE CASCADE,
            blocks_json TEXT NOT NULL,
            title       VARCHAR,
            author_uid  INTEGER,
            author_name VARCHAR,
            note        VARCHAR,
            create_date TIMESTAMP NOT NULL DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS website_page_revision_idx "
             "ON website_page_revision (page_id, id DESC)");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS website_menu (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            url         VARCHAR,
            page_id     INTEGER REFERENCES website_page(id) ON DELETE CASCADE,
            parent_id   INTEGER REFERENCES website_menu(id) ON DELETE CASCADE,
            sequence    INTEGER NOT NULL DEFAULT 10,
            new_window  BOOLEAN NOT NULL DEFAULT FALSE,
            create_date TIMESTAMP NOT NULL DEFAULT now(),
            write_date  TIMESTAMP NOT NULL DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS website_menu_parent_idx ON website_menu (parent_id, sequence)");
    txn.exec(R"(
        INSERT INTO ir_config_parameter (key, value) VALUES
            ('website.site_name', 'Our Company'),
            ('website.accent',    '#0a6f7d'),
            ('website.footer',    ''),
            ('website.theme',     'paper'),
            ('website.dark_mode', 'auto')
        ON CONFLICT (key) DO NOTHING
    )");
    WebsiteForm::ensureSchema(txn);
    txn.commit();
}

void WebsiteModule::seedContent_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    // A starter homepage, UNPUBLISHED. Shipping a published page would put
    // placeholder text on a live site the moment someone deploys.
    txn.exec(R"(
        INSERT INTO website_page (slug, title, blocks_json, is_published, is_homepage, sequence,
                                  meta_description)
        SELECT 'home', 'Welcome',
               '[{"type":"heading","level":"1","text":"Welcome"},'
               '{"type":"text","text":"This is your website''s home page.\n\nEdit it under Settings, add blocks, then publish it when you are ready."},'
               '{"type":"divider"},'
               '{"type":"columns","items":[{"title":"What we do","text":"Describe your business here."},{"title":"Get in touch","text":"Add your contact details."}]}]',
               FALSE, TRUE, 10,
               'Welcome to our website.'
         WHERE NOT EXISTS (SELECT 1 FROM website_page)
    )");
    txn.commit();
}

void WebsiteModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    // Backend menus: Settings → Website. Ids 409/410, actions 123/124 —
    // verified free (docs/113 recorded the survey; these continue that block).
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (123, 'Website Pages', 'website.page', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
            view_mode=EXCLUDED.view_mode
    )");
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (124, 'Website Menu', 'website.menu', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
            view_mode=EXCLUDED.view_mode
    )");
    auto parent = txn.exec("SELECT id FROM ir_ui_menu WHERE name='Settings' AND parent_id IS NULL LIMIT 1");
    if (!parent.empty()) {
        const int pid = parent[0][0].as<int>();
        txn.exec("INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) "
                 "VALUES (409, 'Website Pages', $1, 70, 123) "
                 "ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id, "
                 "  sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id",
                 pqxx::params{pid});
        txn.exec("INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) "
                 "VALUES (410, 'Website Menu', $1, 71, 124) "
                 "ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id, "
                 "  sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id",
                 pqxx::params{pid});
        WebsiteForm::seedMenus(txn);
    }
    txn.commit();
}

} // namespace cerp::modules::website

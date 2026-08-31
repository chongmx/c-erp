// ============================================================
// tests/unit/website/test_render.cpp — the CMS sanitiser and escaper
//
//   ./tests/run.sh --unit --filter Website
//
// These three functions guard a PUBLIC page: whatever they let through is
// executed in a visitor's browser. The integration suite exercises them
// through HTTP, which proves the wiring but can only afford a handful of
// vectors. Here they cost microseconds each, so this is where the whole
// catalogue of XSS tricks belongs — encoding games, case games, nesting
// games, and the ones that only work because a sanitiser tried to be clever.
//
// Soft checks throughout: a sanitiser bug is usually systematic, and seeing
// every vector that got through in one run beats stopping at the first.
// ============================================================
#include "WebsiteRender.hpp"
#include "TestHarness.hpp"

#include <nlohmann/json.hpp>
#include <string>

using cerp::modules::website::WebsiteRender;
using erptest::section;

namespace {

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Everything between a real `<` and `>` in the OUTPUT — i.e. the live markup.
//
// Searching the raw output string cannot tell a live tag from escaped text:
// `&lt;img src=x onerror=alert(1)` contains "onerror" and is completely inert,
// because a browser renders it as visible characters. Judging it dangerous
// would be a false alarm; judging the raw string safe when a real tag is
// present would be worse. So the check looks only where markup can execute.
std::string liveMarkup(const std::string& out) {
    std::string tags;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] != '<') continue;
        const auto gt = out.find('>', i);
        if (gt == std::string::npos) break;
        tags += out.substr(i, gt - i + 1);
        tags += '\n';
        i = gt;
    }
    return tags;
}

// The core assertion for this file: after sanitising, no LIVE tag may carry
// anything that executes.
void mustNotExecute(const std::string& input, const std::string& what) {
    const std::string out  = WebsiteRender::sanitize(input);
    const std::string tags = liveMarkup(out);
    const bool clean =
        !contains(tags, "<script") && !contains(tags, "</script") &&
        !contains(tags, "javascript:") && !contains(tags, "JaVaScRiPt:") &&
        !contains(tags, "onerror") && !contains(tags, "onload") &&
        !contains(tags, "onclick") && !contains(tags, "onmouseover") &&
        !contains(tags, "onfocus") && !contains(tags, "<iframe") &&
        !contains(tags, "<object") && !contains(tags, "<embed") &&
        !contains(tags, "<svg") && !contains(tags, "vbscript:") &&
        // A handler may never be re-emitted in any case form.
        !contains(tags, "OnErRoR") && !contains(tags, "onbegin");
    erptest::check(clean, clean ? what
                                : what + "\n            live markup was: " + tags +
                                  "            full output: " + out);
}

void keeps(const std::string& input, const std::string& needle,
           const std::string& what) {
    const std::string out = WebsiteRender::sanitize(input);
    erptest::check(contains(out, needle),
        contains(out, needle) ? what
                              : what + "\n            got: " + out);
}

} // namespace

// ------------------------------------------------------------------
ERP_TEST(Website, escaping) {
    section("esc() neutralises every character that can start markup");
    CHECK(WebsiteRender::esc("<b>")      == "&lt;b&gt;",        "angle brackets");
    CHECK(WebsiteRender::esc("a & b")    == "a &amp; b",        "ampersand");
    CHECK(WebsiteRender::esc("\"quoted\"") == "&quot;quoted&quot;", "double quote");
    CHECK(WebsiteRender::esc("it's")     == "it&#39;s",         "single quote");
    CHECK(WebsiteRender::esc("")         == "",                 "empty string");

    // The ampersand must be escaped FIRST, or "&lt;" becomes "&amp;lt;"→"<".
    const std::string once = WebsiteRender::esc("<");
    CHECK(WebsiteRender::esc(once) == "&amp;lt;", "escaping is not idempotent-by-accident");

    section("a quoted attribute cannot be broken out of");
    // The classic: close the quote, add a handler.
    const std::string attack = "\" onmouseover=\"alert(1)";
    const std::string safe   = WebsiteRender::esc(attack);
    CHECK(!contains(safe, "\""), "no bare double quote survives escaping");
}

// ------------------------------------------------------------------
ERP_TEST(Website, sanitizeDropsExecutable) {
    section("script, in every disguise");
    mustNotExecute("<script>alert(1)</script>",              "plain script");
    mustNotExecute("<SCRIPT>alert(1)</SCRIPT>",              "upper-case script");
    mustNotExecute("<ScRiPt>alert(1)</ScRiPt>",              "mixed-case script");
    mustNotExecute("<script src='//evil/x.js'></script>",    "external script");
    mustNotExecute("<script\n>alert(1)</script>",            "newline in the tag");
    mustNotExecute("<script >alert(1)</script >",            "spaces in the tags");
    mustNotExecute("<p>before</p><script>alert(1)</script><p>after</p>",
                                                             "script between good content");

    section("event handlers");
    mustNotExecute("<img src=x onerror=alert(1)>",           "onerror, unquoted");
    mustNotExecute("<img src=\"x\" onerror=\"alert(1)\">",   "onerror, quoted");
    mustNotExecute("<img src=x OnErRoR=alert(1)>",           "onerror, mixed case");
    mustNotExecute("<div onclick='x()'>t</div>",             "onclick");
    mustNotExecute("<body onload=alert(1)>",                 "onload on a dropped element");
    mustNotExecute("<a href='#' onmouseover='x()'>t</a>",    "onmouseover on an allowed element");
    mustNotExecute("<input autofocus onfocus=alert(1)>",     "onfocus");

    section("dangerous URL schemes");
    mustNotExecute("<a href=\"javascript:alert(1)\">x</a>",  "javascript:");
    mustNotExecute("<a href=\"JaVaScRiPt:alert(1)\">x</a>",  "javascript:, mixed case");
    mustNotExecute("<a href=\"vbscript:msgbox(1)\">x</a>",   "vbscript:");
    mustNotExecute("<a href=\" javascript:alert(1)\">x</a>", "leading space before the scheme");

    section("framing and foreign content");
    mustNotExecute("<iframe src=\"//evil\"></iframe>",       "iframe");
    mustNotExecute("<object data=\"x.swf\"></object>",       "object");
    mustNotExecute("<embed src=\"x.swf\">",                  "embed");
    mustNotExecute("<svg onload=alert(1)>",                  "svg with a handler");
    mustNotExecute("<svg><animate onbegin=alert(1)/></svg>", "svg child element");

    section("the content of a dropped container goes with it");
    // Unwrapping <script> but keeping its body puts the source on the page as
    // text; worse, it re-enters the parser for raw-text elements.
    const std::string out = WebsiteRender::sanitize("<script>alert(1)</script>");
    CHECK(!contains(out, "alert(1)"), "script body is discarded, not unwrapped");
    const std::string st = WebsiteRender::sanitize("<style>body{x:y}</style>");
    CHECK(!contains(st, "body{x:y}"), "style body is discarded");

    section("malformed input does not fall open");
    mustNotExecute("<script>alert(1)",                       "unterminated script");
    mustNotExecute("<<script>alert(1)</script>",             "doubled angle bracket");
    mustNotExecute("<img src=x onerror=alert(1)",            "unterminated tag");
    mustNotExecute("<!-- <script>alert(1)</script> -->",     "script inside a comment");
    CHECK(WebsiteRender::sanitize("").empty(),               "empty input");
    CHECK(!WebsiteRender::sanitize("<").empty(),             "a lone < is escaped, not dropped");
}

// ------------------------------------------------------------------
ERP_TEST(Website, sanitizeKeepsContent) {
    section("ordinary markup survives");
    keeps("<p>hello</p>",                    "<p>",       "paragraph");
    keeps("<strong>bold</strong>",           "<strong>",  "strong");
    keeps("<ul><li>one</li></ul>",           "<li>",      "list");
    keeps("<h2>Title</h2>",                  "<h2>",      "heading");
    keeps("<table><tr><td>c</td></tr></table>", "<td>",   "table cell");
    keeps("<blockquote>q</blockquote>",      "<blockquote>", "blockquote");

    section("safe links and images survive, with their text");
    keeps("<a href=\"https://example.com\">go</a>", "https://example.com", "https link");
    keeps("<a href=\"http://example.com\">go</a>",  "http://example.com",  "http link");
    keeps("<a href=\"mailto:a@b.c\">mail</a>",      "mailto:a@b.c",        "mailto");
    keeps("<a href=\"tel:+60123\">call</a>",        "tel:+60123",          "tel");
    keeps("<a href=\"/site/about\">about</a>",      "/site/about",         "relative link");
    keeps("<img src=\"https://x/y.png\" alt=\"a\">", "y.png",              "https image");

    section("every outgoing link is detached from the page");
    keeps("<a href=\"https://example.com\">go</a>", "rel=\"noopener", "rel=noopener is added");

    section("data: URLs — images only, and only real image types");
    keeps("<img src=\"data:image/png;base64,AAAA\">", "data:image/png", "data image is kept");
    const std::string dl = WebsiteRender::sanitize("<a href=\"data:text/html,<script>alert(1)</script>\">x</a>");
    CHECK(!contains(dl, "data:text/html"), "data:text/html on a link is dropped");

    section("unknown attributes are dropped, the element is not");
    const std::string o = WebsiteRender::sanitize("<p foo=\"bar\" class=\"keep\">t</p>");
    CHECK(!contains(o, "foo"),        "unknown attribute dropped");
    CHECK(contains(o, "class=\"keep\""), "known attribute kept");
    CHECK(contains(o, "t"),           "text kept");

    section("style is not an allowed attribute");
    const std::string s = WebsiteRender::sanitize("<p style=\"position:fixed;top:0\">t</p>");
    CHECK(!contains(s, "style"), "style is dropped — it is a layout escape hatch");
}

// ------------------------------------------------------------------
ERP_TEST(Website, slugs) {
    section("accepted");
    CHECK(WebsiteRender::isValidSlug("about"),         "a simple slug");
    CHECK(WebsiteRender::isValidSlug("our-services"),  "hyphens");
    CHECK(WebsiteRender::isValidSlug("about/team"),    "a nested slug");
    CHECK(WebsiteRender::isValidSlug("a1/b2/c3"),      "digits and depth");

    section("refused — a slug names a database row, never a file");
    CHECK(!WebsiteRender::isValidSlug(""),             "empty");
    CHECK(!WebsiteRender::isValidSlug("/leading"),     "leading slash");
    CHECK(!WebsiteRender::isValidSlug("trailing/"),    "trailing slash");
    CHECK(!WebsiteRender::isValidSlug("../etc/passwd"),"parent traversal");
    CHECK(!WebsiteRender::isValidSlug("a/../b"),       "traversal in the middle");
    CHECK(!WebsiteRender::isValidSlug("a//b"),         "empty segment");
    CHECK(!WebsiteRender::isValidSlug("Upper"),        "upper case");
    CHECK(!WebsiteRender::isValidSlug("has space"),    "space");
    CHECK(!WebsiteRender::isValidSlug("semi;colon"),   "semicolon");
    CHECK(!WebsiteRender::isValidSlug("q?x=1"),        "query string");
    CHECK(!WebsiteRender::isValidSlug("hash#frag"),    "fragment");
    CHECK(!WebsiteRender::isValidSlug("per%2Fcent"),   "percent encoding");
    CHECK(!WebsiteRender::isValidSlug("back\\slash"),  "backslash");
    CHECK(!WebsiteRender::isValidSlug(std::string(200, 'a')), "over the length cap");
}

// ------------------------------------------------------------------
ERP_TEST(Website, blocks) {
    using nlohmann::json;

    section("author text is escaped, never interpreted");
    json b = json::array({
        {{"type","heading"},{"level","1"},{"text","<script>alert(1)</script>"}},
        {{"type","text"},{"text","<img src=x onerror=alert(1)>"}},
    });
    const std::string out  = WebsiteRender::blocks(b);
    const std::string live = liveMarkup(out);
    CHECK(!contains(live, "<script"),  "heading text cannot become a script");
    CHECK(!contains(live, "onerror"),  "paragraph text cannot become a handler");
    CHECK(!contains(live, "<img"),     "nor an element of any kind");
    CHECK(contains(out, "&lt;script"), "it is escaped instead");

    section("text blocks: blank line is a paragraph, newline is a break");
    json t = json::array({{{"type","text"},{"text","one\ntwo\n\nthree"}}});
    const std::string tv = WebsiteRender::blocks(t);
    CHECK(contains(tv, "one<br/>two"), "single newline becomes a break");
    CHECK(contains(tv, "<p class=\"w-p\">three</p>"), "blank line starts a paragraph");

    section("heading level is constrained to 1-3");
    json h = json::array({{{"type","heading"},{"level","9"},{"text","x"}}});
    CHECK(contains(WebsiteRender::blocks(h), "<h2"), "an out-of-range level falls back to h2");
    json h2 = json::array({{{"type","heading"},{"level","<script>"},{"text","x"}}});
    CHECK(!contains(WebsiteRender::blocks(h2), "<script"), "the level cannot inject a tag");

    section("unsafe URLs drop the whole block rather than render half of it");
    json img = json::array({{{"type","image"},{"src","javascript:alert(1)"},{"alt","a"}}});
    CHECK(WebsiteRender::blocks(img).empty(), "an image with a javascript: src renders nothing");
    json btn = json::array({{{"type","button"},{"href","javascript:alert(1)"},{"text","go"}}});
    CHECK(WebsiteRender::blocks(btn).empty(), "a button with a javascript: href renders nothing");

    section("unknown and malformed blocks render nothing");
    json u = json::array({{{"type","evil"},{"html","<script>alert(1)</script>"}}});
    CHECK(WebsiteRender::blocks(u).empty(), "an unknown block type is skipped");
    CHECK(WebsiteRender::blocks(json::array()).empty(), "an empty list renders nothing");
    CHECK(WebsiteRender::blocks(json("not an array")).empty(), "a non-array renders nothing");
    CHECK(WebsiteRender::blocks(json::array({json("string")})).empty(),
          "a non-object block is skipped");

    section("the html block is sanitised, not trusted");
    json raw = json::array({{{"type","html"},{"html","<p>ok</p><script>alert(1)</script>"}}});
    const std::string rv = WebsiteRender::blocks(raw);
    CHECK(contains(rv, "ok"),        "its safe markup survives");
    CHECK(!contains(rv, "<script"),  "its script does not");

    section("a missing field is absent, not a crash");
    json m = json::array({{{"type","heading"}}, {{"type","text"}}, {{"type","image"}}});
    WebsiteRender::blocks(m);   // must not throw
    CHECK(true, "blocks with missing fields render without throwing");
}

// ------------------------------------------------------------------
// The blocks a marketing site is actually built from. Every one of them takes
// author text, so every one of them is an escaping question.
// ------------------------------------------------------------------
ERP_TEST(Website, siteBlocks) {
    using nlohmann::json;
    const std::string X = "<script>alert(1)</script>";

    section("hero");
    json hero = json::array({{{"type","hero"},{"eyebrow",X},{"headline",X},
                              {"subheadline",X},{"cta_text",X},{"cta_href","/site/x"}}});
    std::string out = WebsiteRender::blocks(hero);
    CHECK(!contains(liveMarkup(out), "<script"), "hero text cannot become a script");
    CHECK(contains(out, "&lt;script&gt;"),        "it is escaped");
    CHECK(contains(out, "w-hero"),                "the hero renders");

    section("a hero CTA with an unsafe href is dropped, not half-rendered");
    json badcta = json::array({{{"type","hero"},{"headline","hi"},
                                {"cta_text","go"},{"cta_href","javascript:alert(1)"}}});
    const std::string bo = WebsiteRender::blocks(badcta);
    CHECK(!contains(bo, "javascript:"), "the javascript: CTA is gone");
    CHECK(contains(bo, "hi"),           "but the headline still renders");

    section("pricing");
    json price = json::array({{{"type","pricing"},{"items", json::array({
        {{"name",X},{"size",X},{"price",X},{"period",X},{"badge",X},
         {"features", json::array({X, "safe feature"})},
         {"cta_text",X},{"cta_href","/site/y"}}})}}});
    out = WebsiteRender::blocks(price);
    CHECK(!contains(liveMarkup(out), "<script"), "no plan field can become a script");
    CHECK(contains(out, "safe feature"),          "a normal feature renders");
    CHECK(contains(out, "w-plan"),                "the plan card renders");

    section("a feature list that is not a list of strings is ignored, not crashed on");
    json weird = json::array({{{"type","pricing"},{"items", json::array({
        {{"name","P"},{"features", json::array({json::object(), 42, "ok"})}}})}}});
    const std::string wo = WebsiteRender::blocks(weird);
    CHECK(contains(wo, "ok"), "the string feature survives");
    CHECK(true,               "non-string features do not throw");

    section("steps — the number is generated, never taken from the data");
    json steps = json::array({{{"type","steps"},{"items", json::array({
        {{"title",X},{"text",X}}, {{"title","Second"},{"text","t"}}})}}});
    out = WebsiteRender::blocks(steps);
    CHECK(!contains(liveMarkup(out), "<script"), "step text cannot become a script");
    CHECK(contains(out, "w-steps"),               "the list renders");
    CHECK(contains(out, "Second"),                "both steps render");

    section("faq");
    json faq = json::array({{{"type","faq"},{"items", json::array({
        {{"q",X},{"a",X}}, {{"q","Real question?"},{"a","Real answer."}}})}}});
    out = WebsiteRender::blocks(faq);
    CHECK(!contains(liveMarkup(out), "<script"), "question and answer are escaped");
    CHECK(contains(out, "<details"),              "it uses <details>, so it works without JS");
    CHECK(contains(out, "Real answer."),          "the answer renders");
    json noq = json::array({{{"type","faq"},{"items", json::array({{{"a","orphan"}}})}}});
    CHECK(!contains(WebsiteRender::blocks(noq), "orphan"),
          "an answer with no question is skipped");

    section("map — coordinates give an embed, a bare name gives a link");
    json mapc = json::array({{{"type","map"},{"lat","3.139"},{"lon","101.6869"},
                              {"query","KL"},{"label","Open"}}});
    out = WebsiteRender::blocks(mapc);
    CHECK(contains(out, "bbox="),   "coordinates produce a bounding box");
    CHECK(contains(out, "marker="), "and a marker");
    CHECK(contains(out, "<iframe"), "so a real map is embedded");

    json mapq = json::array({{{"type","map"},{"query","Somewhere"},{"label","Find"}}});
    out = WebsiteRender::blocks(mapq);
    CHECK(!contains(out, "<iframe"),
          "a place name alone renders NO iframe — an empty grey box is worse than none");
    CHECK(contains(out, "openstreetmap.org/search"), "it renders a link instead");

    section("map coordinates are numbers or nothing");
    json mapbad = json::array({{{"type","map"},{"lat","0\" onload=alert(1)"},
                                {"lon","1"},{"query","Q"}}});
    out = WebsiteRender::blocks(mapbad);
    CHECK(!contains(out, "onload"), "a non-numeric latitude cannot inject an attribute");
    json mapoor = json::array({{{"type","map"},{"lat","999"},{"lon","999"},{"query","Q"}}});
    CHECK(!contains(WebsiteRender::blocks(mapoor), "<iframe"),
          "an out-of-range coordinate falls back to the link");

    section("a form block renders where it sits, via the caller's resolver");
    json withForm = json::array({
        {{"type","heading"},{"level","2"},{"text","Above"}},
        {{"type","form"},{"slug","enquiry"}},
        {{"type","text"},{"text","Below"}}});
    out = WebsiteRender::blocks(withForm, [](const std::string& s) {
        return "<form data-slug=\"" + s + "\"></form>";
    });
    const auto pAbove = out.find("Above");
    const auto pForm  = out.find("data-slug=\"enquiry\"");
    const auto pBelow = out.find("Below");
    CHECK(pAbove != std::string::npos && pForm != std::string::npos &&
          pBelow != std::string::npos, "all three rendered");
    CHECK(pAbove < pForm && pForm < pBelow,
          "the form sits BETWEEN them — not appended after the last block");
    CHECK(WebsiteRender::blocks(withForm).find("data-slug") == std::string::npos,
          "with no resolver the form block renders nothing rather than a broken shell");
}

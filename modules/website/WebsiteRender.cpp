// =============================================================
// modules/website/WebsiteRender.cpp — implementation (docs/115)
// =============================================================
#include "WebsiteRender.hpp"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>

namespace cerp::modules::website {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Elements a page author may use. Everything structural and textual; nothing
// that executes, loads, or frames.
const std::set<std::string>& allowedTags() {
    static const std::set<std::string> t = {
        "p","br","hr","span","div","section",
        "h1","h2","h3","h4","h5","h6",
        "strong","b","em","i","u","s","small","sub","sup","mark",
        "ul","ol","li","dl","dt","dd",
        "blockquote","pre","code",
        "a","img","figure","figcaption",
        "table","thead","tbody","tfoot","tr","th","td","caption",
    };
    return t;
}

// Attributes, per element. `style` is deliberately absent: it is a scripting
// surface in older engines (expression(), url(javascript:)) and a layout
// escape hatch that lets one block cover the rest of the page.
const std::set<std::string>& allowedAttrs(const std::string& tag) {
    static const std::set<std::string> common = {"class","title","id"};
    static const std::set<std::string> aAttrs =
        {"class","title","id","href","target","rel"};
    static const std::set<std::string> imgAttrs =
        {"class","title","id","src","alt","width","height","loading"};
    static const std::set<std::string> tdAttrs =
        {"class","title","id","colspan","rowspan"};
    if (tag == "a")   return aAttrs;
    if (tag == "img") return imgAttrs;
    if (tag == "td" || tag == "th") return tdAttrs;
    return common;
}

const std::set<std::string>& voidTags() {
    static const std::set<std::string> v = {"br","hr","img"};
    return v;
}

// Elements whose CONTENT must be discarded along with the tags, not just
// unwrapped. Dropping `<script>` but keeping what is between the tags puts the
// script's source on the page as visible text — and for the raw-text elements
// (script, style, textarea, title) the browser's own parsing of that content
// differs from ordinary markup, which is exactly the corner a sanitiser must
// not leave open. Every real sanitiser skips these wholesale.
const std::set<std::string>& dropContentTags() {
    static const std::set<std::string> d = {
        "script","style","textarea","title","noscript","template",
        "iframe","object","embed","applet","frame","frameset","svg","math",
    };
    return d;
}

// A URL is safe if its scheme is one we allow. A scheme-relative "//host" is
// treated as a URL (inherits the page scheme, which is fine); anything with a
// colon before the first slash must name an allowed scheme.
bool safeUrl(const std::string& raw, bool isImage) {
    const std::string u = trim(raw);
    if (u.empty()) return false;
    // Reject control characters outright — they are used to smuggle
    // "java\tscript:" past naive checks.
    for (unsigned char c : u) if (c < 0x20 || c == 0x7f) return false;

    const auto colon = u.find(':');
    const auto slash = u.find('/');
    if (colon == std::string::npos) return true;            // relative
    if (slash != std::string::npos && slash < colon) return true;  // path with a colon later

    const std::string scheme = lower(u.substr(0, colon));
    if (scheme == "http" || scheme == "https" ||
        scheme == "mailto" || scheme == "tel") return true;
    // data: only for images, and only real image types.
    if (isImage && scheme == "data") {
        const std::string head = lower(u.substr(0, std::min<std::size_t>(u.size(), 32)));
        return head.rfind("data:image/png",  0) == 0 ||
               head.rfind("data:image/jpeg", 0) == 0 ||
               head.rfind("data:image/gif",  0) == 0 ||
               head.rfind("data:image/webp", 0) == 0;
    }
    return false;   // javascript:, vbscript:, file:, anything unknown
}

} // anonymous namespace

std::string WebsiteRender::esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  o += "&amp;";  break;
            case '<':  o += "&lt;";   break;
            case '>':  o += "&gt;";   break;
            case '"':  o += "&quot;"; break;
            case '\'': o += "&#39;";  break;
            default:   o += c;
        }
    }
    return o;
}

bool WebsiteRender::isValidSlug(const std::string& slug) {
    if (slug.empty() || slug.size() > 120) return false;
    if (slug.front() == '/' || slug.back() == '/') return false;
    if (slug.find("..") != std::string::npos) return false;
    if (slug.find("//") != std::string::npos) return false;
    for (char c : slug) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '/';
        if (!ok) return false;
    }
    return true;
}

// ---------------------------------------------------------------
// The sanitiser. A tiny, deliberately unclever tag-level pass: it walks the
// input, and for each `<...>` decides whether to emit a rebuilt tag or drop
// it. It never tries to "fix" markup, and it re-emits only attributes it
// recognises with values it has escaped — so the output is built from the
// allowlist rather than filtered from the input.
// ---------------------------------------------------------------
std::string WebsiteRender::sanitize(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    std::size_t i = 0;
    const std::size_t n = html.size();

    while (i < n) {
        if (html[i] != '<') {
            // Text. Escape < > & that are not part of a tag we are handling.
            if (html[i] == '&')      out += "&amp;";
            else if (html[i] == '>') out += "&gt;";
            else                     out += html[i];
            ++i;
            continue;
        }

        // Comments and CDATA / doctype: drop entirely.
        if (html.compare(i, 4, "<!--") == 0) {
            const auto end = html.find("-->", i + 4);
            i = (end == std::string::npos) ? n : end + 3;
            continue;
        }
        if (html.compare(i, 2, "<!") == 0 || html.compare(i, 2, "<?") == 0) {
            const auto end = html.find('>', i);
            i = (end == std::string::npos) ? n : end + 1;
            continue;
        }

        const auto gt = html.find('>', i);
        if (gt == std::string::npos) { out += "&lt;"; ++i; continue; }
        std::string inner = html.substr(i + 1, gt - i - 1);
        i = gt + 1;

        bool closing = false;
        if (!inner.empty() && inner.front() == '/') { closing = true; inner.erase(0, 1); }
        bool selfClose = false;
        if (!inner.empty() && inner.back() == '/') { selfClose = true; inner.pop_back(); }

        // tag name
        std::size_t p = 0;
        while (p < inner.size() && !std::isspace(static_cast<unsigned char>(inner[p]))) ++p;
        const std::string tag = lower(inner.substr(0, p));

        // A dangerous container: skip its whole content, not just its tags.
        if (!closing && dropContentTags().count(tag)) {
            const std::string close = "</" + tag;
            // Case-insensitive search for the matching close tag.
            const std::string hay = lower(html);
            const auto end = hay.find(close, i);
            if (end == std::string::npos) { i = n; }         // unterminated → drop the rest
            else {
                const auto endGt = html.find('>', end);
                i = (endGt == std::string::npos) ? n : endGt + 1;
            }
            continue;
        }

        if (!allowedTags().count(tag)) continue;   // unknown element → dropped

        if (closing) {
            if (!voidTags().count(tag)) out += "</" + tag + ">";
            continue;
        }

        // attributes
        std::string rebuilt = "<" + tag;
        const auto& allow = allowedAttrs(tag);
        while (p < inner.size()) {
            while (p < inner.size() && std::isspace(static_cast<unsigned char>(inner[p]))) ++p;
            if (p >= inner.size()) break;
            const std::size_t nameStart = p;
            while (p < inner.size() && inner[p] != '=' &&
                   !std::isspace(static_cast<unsigned char>(inner[p]))) ++p;
            const std::string aname = lower(inner.substr(nameStart, p - nameStart));

            std::string aval;
            while (p < inner.size() && std::isspace(static_cast<unsigned char>(inner[p]))) ++p;
            if (p < inner.size() && inner[p] == '=') {
                ++p;
                while (p < inner.size() && std::isspace(static_cast<unsigned char>(inner[p]))) ++p;
                if (p < inner.size() && (inner[p] == '"' || inner[p] == '\'')) {
                    const char q = inner[p++];
                    const std::size_t vs = p;
                    while (p < inner.size() && inner[p] != q) ++p;
                    aval = inner.substr(vs, p - vs);
                    if (p < inner.size()) ++p;
                } else {
                    const std::size_t vs = p;
                    while (p < inner.size() && !std::isspace(static_cast<unsigned char>(inner[p]))) ++p;
                    aval = inner.substr(vs, p - vs);
                }
            }

            if (aname.empty()) continue;
            // Event handlers go regardless of the element's allowlist. This is
            // belt-and-braces: no `on*` is on any list above.
            if (aname.rfind("on", 0) == 0) continue;
            if (!allow.count(aname)) continue;
            if ((aname == "href" || aname == "src") && !safeUrl(aval, tag == "img")) continue;

            rebuilt += " " + aname + "=\"" + esc(aval) + "\"";
        }

        // Any link that leaves the site opens detached from it. rel=noopener
        // stops window.opener reaching back into the page.
        if (tag == "a") rebuilt += " rel=\"noopener nofollow ugc\"";

        rebuilt += (voidTags().count(tag) || selfClose) ? "/>" : ">";
        out += rebuilt;
    }
    return out;
}

// ---------------------------------------------------------------
// Video embeds (docs/125).
//
// An embed is an iframe: somebody else's document, running in a frame on our
// page. So none of the author's URL survives into the markup. The host is
// matched EXACTLY against a short list, the id is extracted and
// charset-checked, and the embed URL is rebuilt from the id alone.
// ---------------------------------------------------------------
namespace {

// Split "https://host/path?query" into its parts without a URL library.
// Anything without an explicit http/https scheme has no host, which is how
// `javascript:`, `data:` and a bare path all end up rejected.
struct SplitUrl { std::string host, path, query; bool ok = false; };

SplitUrl splitUrl(const std::string& url) {
    SplitUrl s;
    std::string rest;
    if (url.rfind("https://", 0) == 0)      rest = url.substr(8);
    else if (url.rfind("http://", 0) == 0)  rest = url.substr(7);
    else return s;                           // no scheme we accept → no host

    const auto q = rest.find('?');
    if (q != std::string::npos) { s.query = rest.substr(q + 1); rest = rest.substr(0, q); }
    const auto f = s.query.find('#');
    if (f != std::string::npos) s.query = s.query.substr(0, f);

    const auto slash = rest.find('/');
    if (slash == std::string::npos) { s.host = rest; s.path = "/"; }
    else { s.host = rest.substr(0, slash); s.path = rest.substr(slash); }

    // Strip credentials and port. "user@host" is how a host is faked in a
    // string comparison, so the part BEFORE the @ is discarded, not kept.
    const auto at = s.host.find('@');
    if (at != std::string::npos) s.host = s.host.substr(at + 1);
    const auto colon = s.host.find(':');
    if (colon != std::string::npos) s.host = s.host.substr(0, colon);

    for (auto& c : s.host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    s.ok = !s.host.empty();
    return s;
}

// A source we are willing to put in a <video> element: something we host
// (a same-origin path), or an absolute URL that at least names itself a video
// file. Anything else is somebody's web page, and pointing a media element at
// it just makes the visitor fetch it.
bool playableFile(const std::string& raw) {
    const std::string u = trim(raw);
    if (u.empty()) return false;
    for (unsigned char c : u) if (c < 0x20 || c == 0x7f) return false;

    // Same-origin path. "//host/..." is scheme-relative, i.e. another origin.
    if (u.rfind("//", 0) != 0 && u.front() == '/') return true;

    const std::string low = lower(u);
    if (low.rfind("https://", 0) != 0 && low.rfind("http://", 0) != 0) return false;
    const auto q = low.find_first_of("?#");
    const std::string path = q == std::string::npos ? low : low.substr(0, q);
    return path.size() > 4 &&
           (path.compare(path.size() - 4, 4, ".mp4")  == 0 ||
            path.compare(path.size() - 5, 5, ".webm") == 0);
}

bool idOk(const std::string& id, std::size_t maxLen, bool digitsOnly) {
    if (id.empty() || id.size() > maxLen) return false;
    for (char c : id) {
        const auto u = static_cast<unsigned char>(c);
        if (digitsOnly) { if (!std::isdigit(u)) return false; }
        else if (!std::isalnum(u) && c != '_' && c != '-') return false;
    }
    return true;
}

std::string queryParam(const std::string& query, const std::string& key) {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        const auto amp = query.find('&', pos);
        const std::string pair = query.substr(pos, amp == std::string::npos
                                                   ? std::string::npos : amp - pos);
        const auto eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key)
            return pair.substr(eq + 1);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}

// The segment after a known prefix, e.g. "/embed/<id>".
std::string segmentAfter(const std::string& path, const std::string& prefix) {
    if (path.rfind(prefix, 0) != 0) return {};
    std::string rest = path.substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    return rest;
}

} // namespace

VideoRef WebsiteRender::parseVideo(const std::string& url) {
    const SplitUrl u = splitUrl(url);
    if (!u.ok) return {};

    // Exact host matches only. A prefix or suffix test would accept
    // `youtube.com.evil.example` and `notyoutube.com` respectively.
    const bool yt = (u.host == "youtube.com"  || u.host == "www.youtube.com" ||
                     u.host == "m.youtube.com" || u.host == "youtu.be" ||
                     u.host == "www.youtube-nocookie.com" ||
                     u.host == "youtube-nocookie.com");
    const bool vm = (u.host == "vimeo.com" || u.host == "www.vimeo.com" ||
                     u.host == "player.vimeo.com");
    if (!yt && !vm) return {};

    std::string id;
    if (yt) {
        if (u.host == "youtu.be")                      id = segmentAfter(u.path, "/");
        else if (u.path.rfind("/embed/", 0) == 0)      id = segmentAfter(u.path, "/embed/");
        else if (u.path.rfind("/shorts/", 0) == 0)     id = segmentAfter(u.path, "/shorts/");
        else if (u.path.rfind("/live/", 0) == 0)       id = segmentAfter(u.path, "/live/");
        else if (u.path == "/watch")                   id = queryParam(u.query, "v");
        if (!idOk(id, 24, false)) return {};
        return { "youtube", id };
    }

    if (u.path.rfind("/video/", 0) == 0) id = segmentAfter(u.path, "/video/");
    else                                 id = segmentAfter(u.path, "/");
    if (!idOk(id, 24, true)) return {};
    return { "vimeo", id };
}

std::string WebsiteRender::videoEmbedUrl(const VideoRef& v) {
    // youtube-nocookie is the privacy-preserving host: it does not set
    // tracking cookies until the visitor actually plays the video.
    if (v.provider == "youtube") return "https://www.youtube-nocookie.com/embed/" + v.id;
    if (v.provider == "vimeo")   return "https://player.vimeo.com/video/" + v.id;
    return {};
}

// ---------------------------------------------------------------
// The block renderer.
// ---------------------------------------------------------------
std::string WebsiteRender::blocks(const nlohmann::json& blocks,
                                  const FormResolver& formResolver) {
    if (!blocks.is_array()) return {};
    std::ostringstream h;

    auto str = [](const nlohmann::json& b, const char* k) -> std::string {
        auto it = b.find(k);
        return (it != b.end() && it->is_string()) ? it->get<std::string>() : std::string{};
    };

    for (const auto& b : blocks) {
        if (!b.is_object()) continue;
        const std::string type = str(b, "type");

        if (type == "heading") {
            std::string lvl = str(b, "level");
            if (lvl != "1" && lvl != "2" && lvl != "3") lvl = "2";
            h << "<h" << lvl << " class=\"w-h\">" << esc(str(b, "text"))
              << "</h" << lvl << ">";

        } else if (type == "text") {
            // Paragraphs from blank-line-separated text. Newlines become <br>,
            // so an author gets line breaks without needing markup.
            const std::string t = str(b, "text");
            std::istringstream is(t);
            std::string line, para;
            auto flush = [&]{
                if (!para.empty()) { h << "<p class=\"w-p\">" << para << "</p>"; para.clear(); }
            };
            while (std::getline(is, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (trim(line).empty()) { flush(); continue; }
                if (!para.empty()) para += "<br/>";
                para += esc(line);
            }
            flush();

        } else if (type == "image") {
            const std::string src = str(b, "src");
            if (safeUrl(src, true)) {
                h << "<figure class=\"w-fig\"><img src=\"" << esc(src)
                  << "\" alt=\"" << esc(str(b, "alt")) << "\" loading=\"lazy\"/>";
                const std::string cap = str(b, "caption");
                if (!cap.empty()) h << "<figcaption>" << esc(cap) << "</figcaption>";
                h << "</figure>";
            }

        } else if (type == "button") {
            const std::string href = str(b, "href");
            if (safeUrl(href, false))
                h << "<p class=\"w-btn-wrap\"><a class=\"w-btn\" href=\"" << esc(href)
                  << "\" rel=\"noopener\">" << esc(str(b, "text")) << "</a></p>";

        } else if (type == "divider") {
            h << "<hr class=\"w-hr\"/>";

        } else if (type == "columns") {
            auto it = b.find("items");
            if (it != b.end() && it->is_array()) {
                h << "<div class=\"w-cols\">";
                for (const auto& c : *it) {
                    if (!c.is_object()) continue;
                    h << "<div class=\"w-col\">";
                    const std::string ttl = str(c, "title");
                    if (!ttl.empty()) h << "<h3 class=\"w-col-h\">" << esc(ttl) << "</h3>";
                    const std::string bod = str(c, "text");
                    if (!bod.empty()) h << "<p class=\"w-p\">" << esc(bod) << "</p>";
                    h << "</div>";
                }
                h << "</div>";
            }

        } else if (type == "hero") {
            // A landing headline. Same rule as everything else: the author
            // supplies text, the server supplies the markup.
            h << "<section class=\"w-hero\">";
            const std::string eyebrow = str(b, "eyebrow");
            if (!eyebrow.empty()) h << "<p class=\"w-hero-eyebrow\">" << esc(eyebrow) << "</p>";
            h << "<h1 class=\"w-hero-h\">" << esc(str(b, "headline")) << "</h1>";
            const std::string sub = str(b, "subheadline");
            if (!sub.empty()) h << "<p class=\"w-hero-sub\">" << esc(sub) << "</p>";
            const std::string ct = str(b, "cta_text"), ch = str(b, "cta_href");
            const std::string st = str(b, "alt_text"),  sh = str(b, "alt_href");
            if ((!ct.empty() && safeUrl(ch, false)) || (!st.empty() && safeUrl(sh, false))) {
                h << "<p class=\"w-hero-cta\">";
                if (!ct.empty() && safeUrl(ch, false))
                    h << "<a class=\"w-btn\" href=\"" << esc(ch) << "\">" << esc(ct) << "</a>";
                if (!st.empty() && safeUrl(sh, false))
                    h << "<a class=\"w-btn-ghost\" href=\"" << esc(sh) << "\">" << esc(st) << "</a>";
                h << "</p>";
            }
            h << "</section>";

        } else if (type == "pricing") {
            // Unit types / plans. `features` is a list of plain strings, so a
            // feature line can never smuggle markup either.
            auto it = b.find("items");
            if (it != b.end() && it->is_array()) {
                h << "<div class=\"w-plans\">";
                for (const auto& c : *it) {
                    if (!c.is_object()) continue;
                    const bool feat = c.contains("featured") && c["featured"].is_boolean()
                                      && c["featured"].get<bool>();
                    h << "<div class=\"w-plan" << (feat ? " is-feat" : "") << "\">";
                    const std::string badge = str(c, "badge");
                    if (!badge.empty()) h << "<span class=\"w-plan-badge\">" << esc(badge) << "</span>";
                    h << "<h3 class=\"w-plan-name\">" << esc(str(c, "name")) << "</h3>";
                    const std::string size = str(c, "size");
                    if (!size.empty()) h << "<p class=\"w-plan-size\">" << esc(size) << "</p>";
                    const std::string price = str(c, "price");
                    if (!price.empty()) {
                        h << "<p class=\"w-plan-price\">" << esc(price);
                        const std::string per = str(c, "period");
                        if (!per.empty()) h << "<span class=\"w-plan-per\">" << esc(per) << "</span>";
                        h << "</p>";
                    }
                    auto fs = c.find("features");
                    if (fs != c.end() && fs->is_array()) {
                        h << "<ul class=\"w-plan-feats\">";
                        for (const auto& f : *fs)
                            if (f.is_string())
                                h << "<li>" << esc(f.get<std::string>()) << "</li>";
                        h << "</ul>";
                    }
                    const std::string ct = str(c, "cta_text"), ch = str(c, "cta_href");
                    if (!ct.empty() && safeUrl(ch, false))
                        h << "<p class=\"w-plan-cta\"><a class=\"w-btn\" href=\"" << esc(ch)
                          << "\">" << esc(ct) << "</a></p>";
                    h << "</div>";
                }
                h << "</div>";
            }

        } else if (type == "steps") {
            // Numbered steps. The number is generated, never taken from the
            // data — so a "step 3" cannot claim to be anything else.
            auto it = b.find("items");
            if (it != b.end() && it->is_array()) {
                h << "<ol class=\"w-steps\">";
                for (const auto& c : *it) {
                    if (!c.is_object()) continue;
                    h << "<li class=\"w-step\"><h3 class=\"w-step-h\">"
                      << esc(str(c, "title")) << "</h3>";
                    const std::string t = str(c, "text");
                    if (!t.empty()) h << "<p class=\"w-p\">" << esc(t) << "</p>";
                    h << "</li>";
                }
                h << "</ol>";
            }

        } else if (type == "faq") {
            // <details> gives expand/collapse with no JavaScript at all, which
            // also means it still works for a crawler and with JS disabled.
            auto it = b.find("items");
            if (it != b.end() && it->is_array()) {
                h << "<div class=\"w-faq\">";
                for (const auto& c : *it) {
                    if (!c.is_object()) continue;
                    const std::string q = str(c, "q");
                    if (q.empty()) continue;
                    h << "<details class=\"w-faq-i\"><summary>" << esc(q) << "</summary>"
                      << "<p class=\"w-p\">" << esc(str(c, "a")) << "</p></details>";
                }
                h << "</div>";
            }

        } else if (type == "references") {
            // A customer-reference list (the reference ERP's website_customer, #9). Names
            // and blurbs are author text, and the optional logo goes through
            // the same URL check as any other image.
            auto it = b.find("items");
            if (it != b.end() && it->is_array()) {
                h << "<ul class=\"w-refs\">";
                for (const auto& c : *it) {
                    if (!c.is_object()) continue;
                    const std::string nm = str(c, "name");
                    if (nm.empty()) continue;
                    h << "<li class=\"w-ref\">";
                    const std::string logo = str(c, "logo");
                    if (!logo.empty() && safeUrl(logo, true))
                        h << "<img class=\"w-ref-logo\" src=\"" << esc(logo)
                          << "\" alt=\"" << esc(nm) << "\" loading=\"lazy\"/>";
                    h << "<span class=\"w-ref-name\">" << esc(nm) << "</span>";
                    const std::string note = str(c, "note");
                    if (!note.empty()) h << "<span class=\"w-ref-note\">" << esc(note) << "</span>";
                    h << "</li>";
                }
                h << "</ul>";
            }

        } else if (type == "map") {
            // A location map (the reference ERP's website_google_map, #8).
            //
            // Deliberately NOT a raw iframe an author can paste: an iframe can
            // host anything, so that would be a hole straight through the
            // sanitiser. The server builds the URL against a fixed provider.
            //
            // An embed needs a BOUNDING BOX; a place name alone produces an
            // empty grey rectangle, which is worse than no map. So coordinates
            // give a real map, and a bare place name gives an honest link
            // instead of a broken frame.
            const std::string q     = str(b, "query");
            const std::string label = str(b, "label").empty() ? "Open in maps"
                                                              : str(b, "label");
            const std::string latS = str(b, "lat"), lonS = str(b, "lon");
            double lat = 0, lon = 0;
            bool haveCoords = false;
            if (!latS.empty() && !lonS.empty()) {
                try {
                    lat = std::stod(latS); lon = std::stod(lonS);
                    haveCoords = (lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180);
                } catch (...) { haveCoords = false; }
            }

            std::string enc;
            {
                static const char* hexd = "0123456789ABCDEF";
                for (unsigned char c : q) {
                    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                        enc += static_cast<char>(c);
                    else if (c == ' ') enc += '+';
                    else { enc += '%'; enc += hexd[c >> 4]; enc += hexd[c & 0x0F]; }
                }
            }

            if (haveCoords) {
                // A small box around the point. Numbers only — nothing an
                // author typed reaches this URL unparsed.
                const double d = 0.008;
                std::ostringstream bbox;
                bbox << std::fixed << std::setprecision(5)
                     << (lon - d) << "," << (lat - d) << ","
                     << (lon + d) << "," << (lat + d);
                std::ostringstream mk;
                mk << std::fixed << std::setprecision(5) << lat << "," << lon;
                const std::string src =
                    "https://www.openstreetmap.org/export/embed.html?bbox=" +
                    bbox.str() + "&layer=mapnik&marker=" + mk.str();
                h << "<div class=\"w-map\"><iframe src=\"" << esc(src)
                  << "\" title=\"Map\" loading=\"lazy\" referrerpolicy=\"no-referrer\""
                  << " sandbox=\"allow-scripts allow-same-origin\""
                  << " style=\"border:0;width:100%;height:340px\"></iframe>"
                  << "<p class=\"w-map-link\"><a href=\"https://www.openstreetmap.org/?mlat="
                  << esc(mk.str().substr(0, mk.str().find(',')))
                  << "&amp;mlon=" << esc(mk.str().substr(mk.str().find(',') + 1))
                  << "\" rel=\"noopener noreferrer\" target=\"_blank\">"
                  << esc(label) << "</a></p></div>";
            } else if (!q.empty() && q.size() <= 200) {
                h << "<p class=\"w-map-link\"><a class=\"w-btn-ghost\" "
                  << "href=\"https://www.openstreetmap.org/search?query=" << esc(enc)
                  << "\" rel=\"noopener noreferrer\" target=\"_blank\">"
                  << esc(label) << " &mdash; " << esc(q) << "</a></p>";
            }

        } else if (type == "video") {
            // Two kinds of video from one block. A recognised provider URL
            // becomes an iframe built from the ID; anything else that is a
            // safe URL is treated as a file we host and played inline.
            const std::string src = str(b, "src");
            const VideoRef v = parseVideo(src);
            const std::string cap = str(b, "caption");
            if (!v.provider.empty()) {
                h << "<figure class=\"w-video\"><div class=\"w-video-frame\">"
                  << "<iframe src=\"" << esc(videoEmbedUrl(v)) << "\""
                  << " title=\"" << esc(cap.empty() ? std::string("Video") : cap) << "\""
                  << " loading=\"lazy\" referrerpolicy=\"no-referrer\""
                  << " allow=\"accelerometer; clipboard-write; encrypted-media; picture-in-picture\""
                  << " allowfullscreen sandbox=\"allow-scripts allow-same-origin"
                  << " allow-presentation allow-popups\"></iframe></div>";
                if (!cap.empty()) h << "<figcaption>" << esc(cap) << "</figcaption>";
                h << "</figure>";
            } else if (playableFile(src)) {
                // NOT simply "any safe URL". Falling through to <video src>
                // for anything that parsed as a URL meant an unrecognised
                // provider link — `https://youtube.com.evil.example/watch?v=x`
                // — became a request the visitor's browser made to an
                // attacker's host, handing over an IP and a referrer. A file
                // we play is one we host, or one that at least names itself a
                // video file.
                const std::string poster = str(b, "poster");
                h << "<figure class=\"w-video\"><video class=\"w-video-el\" controls"
                  << " preload=\"metadata\" playsinline";
                if (safeUrl(poster, true)) h << " poster=\"" << esc(poster) << "\"";
                h << " src=\"" << esc(src) << "\"></video>";
                if (!cap.empty()) h << "<figcaption>" << esc(cap) << "</figcaption>";
                h << "</figure>";
            }

        } else if (type == "gallery") {
            auto it = b.find("items");
            if (it != b.end() && it->is_array() && !it->empty()) {
                h << "<div class=\"w-gal\">";
                for (const auto& g : *it) {
                    if (!g.is_object()) continue;
                    const std::string src = str(g, "src");
                    if (!safeUrl(src, true)) continue;
                    h << "<figure class=\"w-gal-i\"><img src=\"" << esc(src)
                      << "\" alt=\"" << esc(str(g, "alt")) << "\" loading=\"lazy\"/>";
                    const std::string c = str(g, "caption");
                    if (!c.empty()) h << "<figcaption>" << esc(c) << "</figcaption>";
                    h << "</figure>";
                }
                h << "</div>";
            }

        } else if (type == "quote") {
            const std::string q = str(b, "text");
            if (!q.empty()) {
                h << "<blockquote class=\"w-quote\"><p>" << esc(q) << "</p>";
                const std::string who = str(b, "author"), role = str(b, "role");
                if (!who.empty() || !role.empty()) {
                    h << "<footer><span class=\"w-quote-who\">" << esc(who) << "</span>";
                    if (!role.empty()) h << "<span class=\"w-quote-role\">" << esc(role) << "</span>";
                    h << "</footer>";
                }
                h << "</blockquote>";
            }

        } else if (type == "stats") {
            auto it = b.find("items");
            if (it != b.end() && it->is_array() && !it->empty()) {
                h << "<dl class=\"w-stats\">";
                for (const auto& s : *it) {
                    if (!s.is_object()) continue;
                    h << "<div class=\"w-stat\"><dt class=\"w-stat-v\">"
                      << esc(str(s, "value")) << "</dt><dd class=\"w-stat-l\">"
                      << esc(str(s, "label")) << "</dd></div>";
                }
                h << "</dl>";
            }

        } else if (type == "cta") {
            const std::string head = str(b, "headline");
            if (!head.empty()) {
                h << "<aside class=\"w-cta\"><div class=\"w-cta-t\">"
                  << "<p class=\"w-cta-h\">" << esc(head) << "</p>";
                const std::string sub = str(b, "text");
                if (!sub.empty()) h << "<p class=\"w-cta-s\">" << esc(sub) << "</p>";
                h << "</div>";
                const std::string ct = str(b, "cta_text"), ch = str(b, "cta_href");
                if (!ct.empty() && safeUrl(ch, false))
                    h << "<a class=\"w-btn\" href=\"" << esc(ch) << "\">" << esc(ct) << "</a>";
                h << "</aside>";
            }

        } else if (type == "table") {
            auto rows = b.find("items");
            if (rows != b.end() && rows->is_array() && !rows->empty()) {
                // Wrapped so a wide table scrolls inside its own box rather
                // than making the whole page scroll sideways.
                h << "<div class=\"w-table-wrap\"><table class=\"w-table\">";
                bool first = true;
                const bool hasHead = b.value("header", true);
                for (const auto& r : *rows) {
                    if (!r.is_object()) continue;
                    auto cells = r.find("cells");
                    if (cells == r.end() || !cells->is_array()) continue;
                    const bool th = first && hasHead;
                    h << (th ? "<thead><tr>" : "<tr>");
                    for (const auto& c : *cells)
                        h << (th ? "<th>" : "<td>")
                          << esc(c.is_string() ? c.get<std::string>() : std::string{})
                          << (th ? "</th>" : "</td>");
                    h << (th ? "</tr></thead><tbody>" : "</tr>");
                    first = false;
                }
                h << "</tbody></table></div>";
            }

        } else if (type == "spacer") {
            // Vertical rhythm without inventing a unit: three named sizes.
            const std::string s = str(b, "size");
            const char* cls = s == "small" ? "w-sp-s" : (s == "large" ? "w-sp-l" : "w-sp-m");
            h << "<div class=\"" << cls << "\"></div>";

        } else if (type == "form") {
            // Rendered HERE, in the block's own position, using the resolver
            // the caller supplied. Appending forms after the last block meant
            // a page could never put a form above anything else.
            if (formResolver) {
                const std::string slug = str(b, "slug");
                if (!slug.empty()) h << formResolver(slug);
            }

        } else if (type == "html") {
            // The one block that carries markup. Sanitised here; the caller is
            // responsible for only letting an administrator author it.
            h << "<div class=\"w-raw\">" << sanitize(str(b, "html")) << "</div>";
        }
        // Unknown type: skipped. A block the renderer does not understand is
        // content it cannot vouch for.
    }
    return h.str();
}

} // namespace cerp::modules::website

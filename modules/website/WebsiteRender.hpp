#pragma once
// =============================================================
// modules/website/WebsiteRender.hpp — blocks → HTML (docs/115 §2b)
//
// A CMS is content written by one person and executed in another person's
// browser, on a page anyone can reach. Stored XSS is not a corner case here;
// it is the default failure mode.
//
// So content is BLOCKS, not markup. A page is a JSON array of typed blocks and
// the SERVER builds the HTML. The author supplies text; every value goes out
// escaped by construction rather than by somebody remembering to escape it.
// For heading / text / image / button / columns / divider there is no
// injection surface at all, because no author string is ever treated as markup.
//
// The single exception is the `html` block, for the embed nobody can live
// without. It is run through an ALLOWLIST sanitiser and — enforced by the
// caller — may only be authored by an administrator.
// =============================================================
#include <nlohmann/json.hpp>
#include <functional>
#include <string>

namespace cerp::modules::website {

/// A recognised third-party video. `provider` is "youtube", "vimeo", or "" when
/// the URL is not one we are willing to embed.
struct VideoRef {
    std::string provider;
    std::string id;
};

class WebsiteRender {
public:
    /// HTML-escape a value for element text or a quoted attribute.
    static std::string esc(const std::string& s);

    /**
     * Recognise a video URL and pull out its id.
     *
     * An embed is an iframe, which is somebody else's document running in a
     * frame on our page — so the URL is never passed through. The HOST is
     * matched exactly against a short list, the id is extracted and
     * charset-checked, and the embed URL is then BUILT from the id. A host of
     * `youtube.com.evil.example` matches nothing, and a `javascript:` URL has
     * no host at all.
     *
     * Returns an empty provider for anything not recognised.
     */
    static VideoRef parseVideo(const std::string& url);

    /// The embed URL for a parsed video, or "" if the ref is empty.
    static std::string videoEmbedUrl(const VideoRef& v);

    /**
     * Render a page's block array to HTML.
     *
     * Unknown block types are SKIPPED rather than rendered as anything — a
     * block the renderer does not understand is content it cannot vouch for.
     *
     * @param formResolver  turns a form slug into that form's HTML. A `form`
     *        block needs the database, and this function must not have one —
     *        so the CALLER supplies the lookup and the block still renders in
     *        its right place. Without this, forms were appended after the last
     *        block and a page could not put a form above anything.
     */
    using FormResolver = std::function<std::string(const std::string& slug)>;
    static std::string blocks(const nlohmann::json& blocks,
                              const FormResolver& formResolver = nullptr);

    /**
     * Allowlist sanitiser for the one raw-HTML block.
     *
     * Allowlist, never blocklist: an element not on the list is dropped with
     * its attributes, an attribute not on the list is dropped, and any URL
     * whose scheme is not http/https/mailto/tel (or a data: image) is dropped.
     * `on*` handlers are dropped unconditionally.
     *
     * A blocklist ("strip <script>") fails the day somebody writes
     * `<img onerror=…>`; an allowlist fails closed on everything it has not
     * been told about.
     */
    static std::string sanitize(const std::string& html);

    /// `[a-z0-9-/]`, no `..`, no leading or trailing `/`, length-capped.
    /// A slug selects a database row; it must never be able to name a file.
    static bool isValidSlug(const std::string& slug);
};

} // namespace cerp::modules::website

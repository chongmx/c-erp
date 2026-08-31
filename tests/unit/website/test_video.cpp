// ============================================================
// tests/unit/website/test_video.cpp — video embeds (docs/125)
//
//   ./tests/run.sh --unit --filter Video
//
// An embed is an iframe: somebody else's document running in a frame on our
// page. The author supplies a URL, so the URL is the attack surface, and the
// defence is that NONE of it survives — the host is matched exactly, the id is
// charset-checked, and the embed URL is rebuilt from the id alone.
//
// The case that makes or breaks that is host matching. A prefix test accepts
// `youtube.com.evil.example`; a suffix test accepts `notyoutube.com`; a
// "contains" test accepts both. Each is asserted here.
// ============================================================
#include "WebsiteRender.hpp"
#include "TestHarness.hpp"

#include <string>

using cerp::modules::website::WebsiteRender;
using cerp::modules::website::VideoRef;
using erptest::section;

namespace {
std::string prov(const std::string& u) { return WebsiteRender::parseVideo(u).provider; }
std::string vid(const std::string& u)  { return WebsiteRender::parseVideo(u).id; }
} // namespace

ERP_TEST(Video, youtubeForms) {
    section("every shape a YouTube link arrives in");
    CHECK(vid("https://www.youtube.com/watch?v=dQw4w9WgXcQ") == "dQw4w9WgXcQ", "watch?v=");
    CHECK(vid("https://youtube.com/watch?v=dQw4w9WgXcQ")     == "dQw4w9WgXcQ", "no www");
    CHECK(vid("https://m.youtube.com/watch?v=dQw4w9WgXcQ")   == "dQw4w9WgXcQ", "mobile");
    CHECK(vid("https://youtu.be/dQw4w9WgXcQ")                == "dQw4w9WgXcQ", "short link");
    CHECK(vid("https://www.youtube.com/embed/dQw4w9WgXcQ")   == "dQw4w9WgXcQ", "embed link");
    CHECK(vid("https://www.youtube.com/shorts/dQw4w9WgXcQ")  == "dQw4w9WgXcQ", "shorts");
    CHECK(vid("https://www.youtube.com/live/dQw4w9WgXcQ")    == "dQw4w9WgXcQ", "live");
    CHECK(vid("http://www.youtube.com/watch?v=dQw4w9WgXcQ")  == "dQw4w9WgXcQ", "plain http");

    section("the id survives its neighbours in the query string");
    CHECK(vid("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=42")  == "dQw4w9WgXcQ", "before &t");
    CHECK(vid("https://www.youtube.com/watch?list=PL1&v=dQw4w9WgXcQ") == "dQw4w9WgXcQ",
          "not the first parameter");
    CHECK(vid("https://youtu.be/dQw4w9WgXcQ?t=42")  == "dQw4w9WgXcQ", "short link with a time");
    CHECK(vid("https://youtu.be/dQw4w9WgXcQ/")      == "dQw4w9WgXcQ", "trailing slash");

    section("the embed URL is BUILT, not echoed");
    const VideoRef v = WebsiteRender::parseVideo("https://youtu.be/dQw4w9WgXcQ");
    CHECK(WebsiteRender::videoEmbedUrl(v) ==
          "https://www.youtube-nocookie.com/embed/dQw4w9WgXcQ",
          "…on the no-cookie host, from the id alone");
}

ERP_TEST(Video, vimeoForms) {
    section("vimeo");
    CHECK(prov("https://vimeo.com/123456789")               == "vimeo", "canonical");
    CHECK(vid("https://vimeo.com/123456789")                == "123456789", "…its id");
    CHECK(vid("https://player.vimeo.com/video/123456789")   == "123456789", "player link");
    CHECK(vid("https://www.vimeo.com/123456789")            == "123456789", "www");
    CHECK(WebsiteRender::videoEmbedUrl(WebsiteRender::parseVideo("https://vimeo.com/123456789"))
          == "https://player.vimeo.com/video/123456789", "embed URL");

    section("a vimeo id is digits");
    CHECK(prov("https://vimeo.com/notanumber").empty(),  "letters are refused");
    CHECK(prov("https://vimeo.com/123abc").empty(),      "so is a mixture");
    CHECK(prov("https://vimeo.com/").empty(),            "and an empty path");
}

ERP_TEST(Video, hostSpoofing) {
    section("THE case — a host that merely LOOKS like the real one");
    // Each of these passes a naive prefix, suffix or contains test.
    CHECK(prov("https://youtube.com.evil.example/watch?v=dQw4w9WgXcQ").empty(),
          "youtube.com as a PREFIX of the real host");
    CHECK(prov("https://notyoutube.com/watch?v=dQw4w9WgXcQ").empty(),
          "youtube.com as a SUFFIX");
    CHECK(prov("https://evil.example/youtube.com/watch?v=dQw4w9WgXcQ").empty(),
          "the name in the PATH");
    CHECK(prov("https://evil.example/?host=youtube.com&v=dQw4w9WgXcQ").empty(),
          "the name in the QUERY");
    CHECK(prov("https://vimeo.com.evil.example/123456789").empty(),
          "the same trick on vimeo");

    section("credentials and ports cannot fake a host either");
    // "user@host" is the classic: everything before the @ is userinfo, so a
    // check that keeps it would read the wrong string as the host.
    CHECK(prov("https://www.youtube.com@evil.example/watch?v=dQw4w9WgXcQ").empty(),
          "youtube.com as USERINFO in front of an attacker's host");
    CHECK(prov("https://evil.example@www.youtube.com/watch?v=dQw4w9WgXcQ") == "youtube",
          "…while a real host behind userinfo is still that host");
    CHECK(prov("https://WWW.YOUTUBE.COM/watch?v=dQw4w9WgXcQ") == "youtube",
          "the host is compared case-insensitively");
}

ERP_TEST(Video, refusesEverythingElse) {
    section("schemes that are not a video at all");
    CHECK(prov("javascript:alert(1)").empty(),                  "javascript:");
    CHECK(prov("data:text/html,<script>alert(1)</script>").empty(), "data:");
    CHECK(prov("file:///etc/passwd").empty(),                   "file:");
    CHECK(prov("//www.youtube.com/watch?v=dQw4w9WgXcQ").empty(),
          "scheme-relative — an embed needs a scheme we chose");
    CHECK(prov("").empty(),                                     "empty");
    CHECK(prov("not a url at all").empty(),                     "not a URL");
    CHECK(prov("https://").empty(),                             "a scheme and nothing else");

    section("the right host but nothing usable on it");
    CHECK(prov("https://www.youtube.com/").empty(),             "no video");
    CHECK(prov("https://www.youtube.com/watch").empty(),        "watch with no v");
    CHECK(prov("https://www.youtube.com/watch?v=").empty(),     "an empty v");
    CHECK(prov("https://www.youtube.com/feed/subscriptions").empty(), "some other page");

    section("an id is charset-checked, because it is pasted into a URL");
    CHECK(prov("https://www.youtube.com/watch?v=abc\"onload=x").empty(), "a quote");
    CHECK(prov("https://www.youtube.com/watch?v=abc<script>").empty(),   "a tag");
    CHECK(prov("https://www.youtube.com/watch?v=../../secret").empty(),  "traversal");
    CHECK(prov("https://www.youtube.com/watch?v=" + std::string(80, 'a')).empty(),
          "an absurdly long id");
    CHECK(prov("https://www.youtube.com/embed/a_b-C9").empty() == false,
          "underscore and dash are legitimate in a YouTube id");

    section("a ref that parsed to nothing produces no URL");
    CHECK(WebsiteRender::videoEmbedUrl(VideoRef{}).empty(), "empty ref, empty URL");
    CHECK(WebsiteRender::videoEmbedUrl(VideoRef{"vine", "x"}).empty(),
          "an unknown provider is not guessed at");
}

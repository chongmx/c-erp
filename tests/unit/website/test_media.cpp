// ============================================================
// tests/unit/website/test_media.cpp — uploaded images (docs/124)
//
//   ./tests/run.sh --unit --filter Media
//
// An uploaded file is attacker-controlled bytes that will later be served from
// OUR origin. The only thing standing between "somebody uploaded a file" and
// "we serve it to every visitor" is sniff(): if it accepts a document, the
// site has a stored-XSS delivery mechanism with an <img> tag on the front.
//
// So the cases here are mostly things that must be REFUSED, and the most
// important one is SVG — the format everybody calls an image and browsers
// treat as a script-bearing document.
// ============================================================
#include "WebsiteMedia.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <string>

using cerp::modules::website::WebsiteMedia;
using erptest::section;

namespace {

// Real signatures, then enough filler that a length check cannot be what
// passes or fails a case by accident.
const std::string PNG  = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(64, '\0');
const std::string JPEG = std::string("\xFF\xD8\xFF\xE0", 4) + std::string(64, '\0');
const std::string JPG2 = std::string("\xFF\xD8\xFF\xE1", 4) + std::string(64, '\0');  // Exif
const std::string GIF7 = "GIF87a" + std::string(64, '\0');
const std::string GIF9 = "GIF89a" + std::string(64, '\0');
const std::string WEBP = "RIFF" + std::string(4, '\x20') + "WEBP" + std::string(64, '\0');

} // namespace

ERP_TEST(Media, sniffAccepts) {
    section("the four raster formats the site will serve");
    CHECK(WebsiteMedia::sniff(PNG)  == "image/png",  "PNG");
    CHECK(WebsiteMedia::sniff(JPEG) == "image/jpeg", "JPEG (JFIF)");
    CHECK(WebsiteMedia::sniff(JPG2) == "image/jpeg", "JPEG (Exif) — same three leading bytes");
    CHECK(WebsiteMedia::sniff(GIF7) == "image/gif",  "GIF87a");
    CHECK(WebsiteMedia::sniff(GIF9) == "image/gif",  "GIF89a");
    CHECK(WebsiteMedia::sniff(WEBP) == "image/webp", "WebP");
}

ERP_TEST(Media, sniffRefuses) {
    section("SVG — the one that matters");
    // Every shape a real SVG upload takes. None of them may be accepted: an
    // SVG is XML, it can carry <script>, and served from our origin it would
    // run with our origin's privileges.
    CHECK(WebsiteMedia::sniff("<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>").empty(),
          "a bare svg element");
    CHECK(WebsiteMedia::sniff("<?xml version=\"1.0\"?><svg><script>alert(1)</script></svg>").empty(),
          "an svg with a declaration and a script");
    CHECK(WebsiteMedia::sniff("\xEF\xBB\xBF<svg onload=\"alert(1)\">").empty(),
          "an svg behind a UTF-8 BOM");
    CHECK(WebsiteMedia::sniff("   \n\t<svg>").empty(), "an svg behind whitespace");

    section("documents wearing an image's name");
    CHECK(WebsiteMedia::sniff("<!DOCTYPE html><html><script>alert(1)</script>").empty(), "HTML");
    CHECK(WebsiteMedia::sniff("%PDF-1.7\n").empty(),                    "PDF");
    CHECK(WebsiteMedia::sniff("PK\x03\x04").empty(),                    "ZIP / docx");
    CHECK(WebsiteMedia::sniff("#!/bin/sh\nrm -rf /\n").empty(),         "a shell script");
    CHECK(WebsiteMedia::sniff("\x7F""ELF").empty(),                     "an ELF binary");
    CHECK(WebsiteMedia::sniff("GIF89a is what this text file claims").size() > 0,
          "…though a file that BEGINS with a real GIF signature is a GIF, "
          "which is why the serve route re-checks rather than trusting the row");

    section("near misses");
    CHECK(WebsiteMedia::sniff("").empty(),                    "empty input");
    CHECK(WebsiteMedia::sniff("\x89PNG").empty(),             "a truncated PNG signature");
    CHECK(WebsiteMedia::sniff("\xFF\xD8").empty(),            "two of JPEG's three bytes");
    CHECK(WebsiteMedia::sniff("GIF88a" + std::string(64, 0)).empty(), "a GIF version that does not exist");
    // RIFF alone is a container: a .wav starts this way. Both halves matter.
    CHECK(WebsiteMedia::sniff("RIFF" + std::string(4, '\x20') + "WAVE" +
                              std::string(64, '\0')).empty(), "RIFF/WAVE is not WebP");
    CHECK(WebsiteMedia::sniff("RIFF").empty(),                "RIFF with nothing after it");
    CHECK(WebsiteMedia::sniff(std::string(64, '\0')).empty(), "all zeroes");
}

ERP_TEST(Media, extensions) {
    section("an accepted type has an extension; nothing else does");
    CHECK(WebsiteMedia::extensionFor("image/png")  == "png",  "png");
    CHECK(WebsiteMedia::extensionFor("image/jpeg") == "jpg",  "jpeg");
    CHECK(WebsiteMedia::extensionFor("image/gif")  == "gif",  "gif");
    CHECK(WebsiteMedia::extensionFor("image/webp") == "webp", "webp");
    // The serve route uses this as its second gate, so these MUST be empty.
    CHECK(WebsiteMedia::extensionFor("image/svg+xml").empty(), "svg has no extension here");
    CHECK(WebsiteMedia::extensionFor("text/html").empty(),     "nor html");
    CHECK(WebsiteMedia::extensionFor("application/pdf").empty(), "nor pdf");
    CHECK(WebsiteMedia::extensionFor("").empty(),              "nor an empty type");
}

ERP_TEST(Media, filenames) {
    section("the extension comes from the BYTES, not from the name");
    CHECK(WebsiteMedia::safeName("photo.svg", "image/png")  == "photo.png",
          "a claimed .svg on PNG bytes is stored as .png");
    CHECK(WebsiteMedia::safeName("shell.php", "image/jpeg") == "shell.jpg",
          "…and a claimed .php as .jpg");
    // Stronger than "the last extension is replaced": every dot is stripped
    // from the base, so the ONLY dot in the result is the one before the real
    // extension. A double extension has nothing to smuggle with.
    CHECK(WebsiteMedia::safeName("a.jpg.exe", "image/png")  == "ajpg.png",
          "a double extension collapses — one dot survives, and it is ours");
    // One call, one string: begin() and end() from two separate temporaries
    // are iterators into two different objects, which is undefined behaviour
    // and happened to compare unequal here.
    const std::string many = WebsiteMedia::safeName("x.y.z.php", "image/gif");
    CHECK(std::count(many.begin(), many.end(), '.') == 1,
          "…exactly one dot, whatever went in");

    section("a name is a basename, not a path");
    CHECK(WebsiteMedia::safeName("../../etc/passwd", "image/png") == "passwd.png", "unix traversal");
    CHECK(WebsiteMedia::safeName("..\\..\\win.ini", "image/png")  == "win.png",    "windows traversal");
    CHECK(WebsiteMedia::safeName("/absolute/path.png", "image/png") == "path.png",  "absolute path");

    section("nothing that could break a header survives");
    // S-39: this lands in Content-Disposition. A quote or a CRLF there is a
    // header injection.
    const std::string bad = WebsiteMedia::safeName("a\"b\r\nX-Evil: 1.png", "image/png");
    CHECK(bad.find('"')  == std::string::npos, "no double quote");
    CHECK(bad.find('\r') == std::string::npos, "no carriage return");
    CHECK(bad.find('\n') == std::string::npos, "no newline");
    CHECK(bad.find(';')  == std::string::npos, "no semicolon");

    section("it is never empty, and never unbounded");
    CHECK(WebsiteMedia::safeName("", "image/png")      == "image.png", "an empty name");
    CHECK(WebsiteMedia::safeName(".png", "image/png")  == "png.png", "a leading dot is not treated as an extension, so a dotfile keeps a name");
    CHECK(WebsiteMedia::safeName("...", "image/png")   == "image.png", "a name of only dots");
    CHECK(WebsiteMedia::safeName("!!!@@@", "image/png") == "image.png", "a name of only punctuation");
    CHECK(WebsiteMedia::safeName(std::string(500, 'a'), "image/png").size() <= 65,
          "a 500-character name is capped");

    section("ordinary names survive intact");
    CHECK(WebsiteMedia::safeName("unit-50.png", "image/png")   == "unit-50.png", "a normal name");
    CHECK(WebsiteMedia::safeName("Front Door.JPG", "image/jpeg") == "Front-Door.jpg",
          "spaces become dashes, case is kept");
}

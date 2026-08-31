#include "WebsiteMedia.hpp"

#include <algorithm>
#include <cctype>

namespace cerp::modules::website {
namespace {

bool startsWith(const std::string& s, const char* sig, std::size_t n) {
    return s.size() >= n && std::equal(sig, sig + n, s.begin());
}

} // namespace

std::string WebsiteMedia::sniff(const std::string& b) {
    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (startsWith(b, "\x89PNG\r\n\x1a\n", 8)) return "image/png";

    // JPEG: FF D8 FF — every variant (JFIF, Exif, raw) shares these three.
    if (b.size() >= 3 &&
        static_cast<unsigned char>(b[0]) == 0xFF &&
        static_cast<unsigned char>(b[1]) == 0xD8 &&
        static_cast<unsigned char>(b[2]) == 0xFF) return "image/jpeg";

    // GIF: "GIF87a" or "GIF89a"
    if (startsWith(b, "GIF87a", 6) || startsWith(b, "GIF89a", 6)) return "image/gif";

    // WebP: "RIFF" .... "WEBP" — the size field sits between the two, so both
    // halves have to be checked or a plain RIFF (a .wav, say) passes.
    if (b.size() >= 12 && startsWith(b, "RIFF", 4) &&
        std::equal(b.begin() + 8, b.begin() + 12, "WEBP")) return "image/webp";

    // --- video (docs/125) ---
    // MP4 and friends: a 4-byte size, then "ftyp", then a brand. The brand
    // decides whether this is video we will serve or some other ISO-BMFF
    // container, so it is checked rather than assumed.
    if (b.size() >= 12 && std::equal(b.begin() + 4, b.begin() + 8, "ftyp")) {
        const std::string brand = b.substr(8, 4);
        if (brand == "isom" || brand == "iso2" || brand == "mp41" || brand == "mp42" ||
            brand == "avc1" || brand == "M4V " || brand == "dash" || brand == "iso5" ||
            brand == "iso6" || brand == "mmp4")
            return "video/mp4";
        return "";                       // ISO-BMFF, but not one we serve
    }

    // WebM / Matroska: the EBML header. Both use it, and a browser that can
    // play one plays the other, so they share a type here.
    if (b.size() >= 4 &&
        static_cast<unsigned char>(b[0]) == 0x1A &&
        static_cast<unsigned char>(b[1]) == 0x45 &&
        static_cast<unsigned char>(b[2]) == 0xDF &&
        static_cast<unsigned char>(b[3]) == 0xA3) return "video/webm";

    // Everything else, explicitly including SVG: an SVG is XML and can carry
    // script. Serving one from our origin would be stored XSS wearing an
    // <img> tag. It has no signature to match here, and that is deliberate.
    return "";
}

bool WebsiteMedia::isVideo(const std::string& mime) {
    return mime == "video/mp4" || mime == "video/webm";
}

long long WebsiteMedia::maxBytesFor(const std::string& mime) {
    // Video is bigger by nature, and still bounded: anything longer than a
    // short clip belongs on a provider, which is what the embed is for.
    return isVideo(mime) ? kMaxVideoBytes : kMaxBytes;
}

std::string WebsiteMedia::extensionFor(const std::string& mime) {
    if (mime == "image/png")  return "png";
    if (mime == "image/jpeg") return "jpg";
    if (mime == "image/gif")  return "gif";
    if (mime == "image/webp") return "webp";
    if (mime == "video/mp4")  return "mp4";
    if (mime == "video/webm") return "webm";
    return "";
}

std::string WebsiteMedia::safeName(const std::string& proposed, const std::string& mime) {
    // Basename only. A path separator in an uploaded name is either a
    // traversal attempt or a browser quirk; neither is wanted.
    std::string base = proposed;
    const auto cut = base.find_last_of("/\\");
    if (cut != std::string::npos) base.erase(0, cut + 1);

    // Drop whatever extension was claimed — the real one comes from the bytes.
    const auto dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base.erase(dot);

    std::string out;
    for (char c : base) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '_' || c == ' ')
            out.push_back(c == ' ' ? '-' : c);
        if (out.size() >= 60) break;
    }
    // Trim leading/trailing dashes so "  .png" does not become "-".
    while (!out.empty() && out.front() == '-') out.erase(out.begin());
    while (!out.empty() && out.back()  == '-') out.pop_back();
    if (out.empty()) out = "image";

    const std::string ext = extensionFor(mime);
    return ext.empty() ? out : out + "." + ext;
}

} // namespace cerp::modules::website

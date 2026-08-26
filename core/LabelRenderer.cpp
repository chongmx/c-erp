#include "LabelRenderer.hpp"
#include "qrcodegen.hpp"

#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>

namespace odoo::core {

using qrcodegen::QrCode;

std::string xmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (const char c : in) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // Control characters are not legal in XML and silently kill the
                // parse — the same class of bug as the NUL byte in docs/093.
                if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n') break;
                out += c;
        }
    }
    return out;
}

QrMatrix encodeQr(const std::string& payload) {
    if (payload.empty())
        throw std::invalid_argument("Cannot encode an empty QR payload.");
    // Medium error correction: the usual choice for labels. Low is fragile once
    // a label is scuffed in a parts drawer; High costs modules a small label
    // cannot spare. The library picks the smallest version that fits.
    const QrCode qr = QrCode::encodeText(payload.c_str(), QrCode::Ecc::MEDIUM);

    QrMatrix m;
    m.size    = qr.getSize();
    m.version = qr.getVersion();
    m.dark.assign(m.size, std::vector<bool>(m.size, false));
    for (int y = 0; y < m.size; ++y)
        for (int x = 0; x < m.size; ++x)
            m.dark[y][x] = qr.getModule(x, y);
    return m;
}

namespace {

std::string fmt(double v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << v;
    std::string s = os.str();
    // Trim the trailing zeros a fixed precision leaves behind, so the markup
    // stays readable — and so a label sheet is not needlessly large.
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s.empty() ? "0" : s;
}

/// The QR as a <g> of unit-square rects in MODULE coordinates, with the scale
/// carried by the group's transform. Keeping the rects on an integer module
/// grid is what makes the output machine-checkable: a test can read the symbol
/// straight back out of the markup and compare it to another encoder.
std::string qrGroup(const QrMatrix& m, double x, double y, double sizeMm, int quiet) {
    const int    span  = m.size + 2 * quiet;
    const double scale = sizeMm / static_cast<double>(span);

    std::ostringstream os;
    os << "<g class=\"qr\" data-qr-size=\"" << m.size
       << "\" data-qr-version=\"" << m.version
       << "\" data-qr-quiet=\"" << quiet << "\""
       << " transform=\"translate(" << fmt(x) << "," << fmt(y) << ") scale(" << fmt(scale) << ")\""
       << " shape-rendering=\"crispEdges\">";
    // The quiet zone is part of the symbol, not decoration: without it a scanner
    // cannot find the finder patterns against surrounding print.
    os << "<rect class=\"qr-quiet\" x=\"0\" y=\"0\" width=\"" << span << "\" height=\"" << span
       << "\" fill=\"#fff\"/>";
    for (int r = 0; r < m.size; ++r) {
        for (int c = 0; c < m.size; ++c) {
            if (!m.dark[r][c]) continue;
            os << "<rect x=\"" << (c + quiet) << "\" y=\"" << (r + quiet)
               << "\" width=\"1\" height=\"1\" fill=\"#000\"/>";
        }
    }
    os << "</g>";
    return os.str();
}

constexpr double kPerChar   = 0.62;   ///< rough advance/size ratio for Helvetica
constexpr double kMinFontMm = 1.7;    ///< below ~5pt a printed label is unreadable

/// Shrink a line towards a floor, then truncate. Without font metrics the
/// options are to estimate or to overflow; estimating is the honest one. But
/// shrinking without limit produces a line that technically fits and cannot be
/// read, so past the floor the text is cut instead — a truncated part name is
/// still useful, a 3pt one is not.
double fitFontMm(const std::string& text, double widthMm, double preferredMm) {
    if (text.empty()) return preferredMm;
    const double needed = widthMm / (text.size() * kPerChar);
    return std::max(kMinFontMm, std::min(preferredMm, needed));
}

/// Cut `text` to what fits `widthMm` at `fontMm`, marking the cut with an
/// ellipsis. Operates on bytes but only ever cuts at a UTF-8 lead byte, so a
/// multi-byte character is never split into invalid output.
std::string ellipsise(const std::string& text, double widthMm, double fontMm) {
    const size_t maxChars = static_cast<size_t>(widthMm / (fontMm * kPerChar));
    if (maxChars < 2 || text.size() <= maxChars) return text;
    size_t cut = maxChars - 1;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + "\xE2\x80\xA6";   // U+2026
}

} // namespace

std::string renderQrSvg(const std::string& payload, double sizeMm, int quietZone) {
    const QrMatrix m = encodeQr(payload);
    std::ostringstream os;
    os << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << fmt(sizeMm) << "mm\" height=\""
       << fmt(sizeMm) << "mm\" viewBox=\"0 0 " << fmt(sizeMm) << " " << fmt(sizeMm) << "\">"
       << qrGroup(m, 0, 0, sizeMm, quietZone) << "</svg>";
    return os.str();
}

std::string renderLabelSvg(const LabelSpec& spec) {
    const double w = std::max(10.0, spec.widthMm);
    const double h = std::max(8.0,  spec.heightMm);
    const double pad = std::min(1.5, h * 0.06);

    std::ostringstream os;
    os << "<svg xmlns=\"http://www.w3.org/2000/svg\" class=\"label\""
       << " width=\"" << fmt(w) << "mm\" height=\"" << fmt(h) << "mm\""
       << " viewBox=\"0 0 " << fmt(w) << " " << fmt(h) << "\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << fmt(w) << "\" height=\"" << fmt(h)
       << "\" fill=\"#fff\"/>";

    double textX = pad;
    double textW = w - 2 * pad;

    if (spec.showQr && !spec.payload.empty()) {
        // The QR is square and as tall as the label allows; the text takes what
        // is left. On a 50x25 label that is a 22mm symbol and 26mm of text.
        const double qrSize = h - 2 * pad;
        os << qrGroup(encodeQr(spec.payload), pad, pad, qrSize, spec.quietZone);
        textX = pad + qrSize + pad;
        textW = w - textX - pad;
    }

    // Lines, top to bottom, in decreasing prominence. Each is clipped to the
    // text column so a long name cannot run over the QR or off the label.
    struct Line { std::string text; double size; const char* weight; const char* fill; };
    std::vector<Line> lines;
    if (!spec.title.empty())
        lines.push_back({spec.title, std::min(4.2, h * 0.24), "700", "#000"});
    if (!spec.subtitle.empty())
        lines.push_back({spec.subtitle, std::min(2.6, h * 0.15), "400", "#222"});
    if (!spec.extra.empty())
        lines.push_back({spec.extra, std::min(2.2, h * 0.12), "400", "#444"});
    // The payload printed as characters: a scanner reads the QR, a human reads
    // this, and when the symbol is damaged this is what saves the label.
    if (spec.showText && !spec.payload.empty() && spec.payload != spec.title)
        lines.push_back({spec.payload, std::min(2.2, h * 0.12), "400", "#000"});

    double totalH = 0;
    for (auto& ln : lines) {
        ln.size = fitFontMm(ln.text, textW, ln.size);
        ln.text = ellipsise(ln.text, textW, ln.size);
        totalH += ln.size * 1.35;
    }
    double cursor = (h - totalH) / 2.0;
    if (cursor < pad) cursor = pad;

    os << "<g font-family=\"Helvetica,Arial,sans-serif\">";
    for (const auto& ln : lines) {
        cursor += ln.size;
        os << "<text x=\"" << fmt(textX) << "\" y=\"" << fmt(cursor)
           << "\" font-size=\"" << fmt(ln.size) << "\" font-weight=\"" << ln.weight
           << "\" fill=\"" << ln.fill << "\">" << xmlEscape(ln.text) << "</text>";
        cursor += ln.size * 0.35;
    }
    os << "</g></svg>";
    return os.str();
}

std::string renderLabelSheetHtml(const std::vector<LabelSpec>& labels,
                                 int columns, double gapMm, const std::string& pageTitle) {
    const int cols = std::max(1, std::min(12, columns));
    std::ostringstream os;
    os << "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>"
       << xmlEscape(pageTitle) << "</title><style>"
       << "@page { size: A4; margin: 8mm; }"
       << "body { margin: 0; font-family: Helvetica, Arial, sans-serif; background: #fff; color: #000; }"
       << ".sheet { display: grid; grid-template-columns: repeat(" << cols
       << ", max-content); gap: " << fmt(gapMm) << "mm; }"
       << ".cell { break-inside: avoid; }"
       // A hairline cut guide, hidden when printing: useful on screen for
       // checking alignment, and noise on the actual label.
       << ".cell svg { display: block; outline: .1mm dashed #bbb; }"
       << ".bar { padding: 6mm 0; }"
       << "@media print { .bar { display: none; } .cell svg { outline: none; } }"
       << "</style></head><body>"
       << "<div class=\"bar\"><b>" << xmlEscape(pageTitle) << "</b> — "
       << labels.size() << " label(s). Print at 100% scale; do not \"fit to page\".</div>"
       << "<div class=\"sheet\">";
    for (const auto& l : labels)
        os << "<div class=\"cell\">" << renderLabelSvg(l) << "</div>";
    os << "</div></body></html>";
    return os.str();
}

} // namespace odoo::core

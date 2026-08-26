#pragma once
//
// LabelRenderer — part/product labels as SVG (docs/099).
//
// SVG rather than a raster: a label is printed, and print is where resolution
// matters most. Vector output is crisp at any DPI, needs no image library in
// the build, prints straight from the browser, and — usefully — is inspectable
// as text, which is what lets the QR output be verified module-by-module
// against an independent encoder instead of taken on trust.
//
#include <string>
#include <vector>

namespace odoo::core {

/// One label's content. Sizes are millimetres, because labels are bought in
/// millimetres and every printer dialog speaks them.
struct LabelSpec {
    std::string payload;        ///< what the QR encodes (a code, or a URL)
    std::string title;          ///< the big line — usually the part's code
    std::string subtitle;       ///< the small line — usually its name
    std::string extra;          ///< an optional third line (package, location…)
    double widthMm  = 50.0;
    double heightMm = 25.0;
    bool   showQr   = true;
    bool   showText = true;     ///< print the payload as readable characters too
    int    quietZone = 2;       ///< QR quiet zone, in modules (the spec says 4)
};

/// The QR module matrix, exposed so callers (and tests) can reason about the
/// symbol itself rather than about pixels.
struct QrMatrix {
    int size = 0;                       ///< modules per side
    int version = 0;                    ///< 1..40
    std::vector<std::vector<bool>> dark;
    bool at(int x, int y) const {
        return x >= 0 && y >= 0 && y < size && x < size && dark[y][x];
    }
};

/// Encode `payload` as a QR symbol. Throws std::length_error if it will not fit.
QrMatrix encodeQr(const std::string& payload);

/// A complete, standalone <svg> document for one label.
std::string renderLabelSvg(const LabelSpec& spec);

/// Just the QR, as a standalone <svg> of `sizeMm` square.
std::string renderQrSvg(const std::string& payload, double sizeMm, int quietZone = 2);

/// A print-ready HTML page laying labels out on a page-sized grid.
std::string renderLabelSheetHtml(const std::vector<LabelSpec>& labels,
                                 int columns, double gapMm, const std::string& pageTitle);

/// XML-escape text destined for SVG/HTML content.
std::string xmlEscape(const std::string& in);

} // namespace odoo::core

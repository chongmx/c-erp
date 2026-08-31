#include "WebsitePalette.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace cerp::modules::website {
namespace {

/// The one dark ink used for text on a pale accent. Not pure black: on a
/// saturated pale ground pure black reads as a hole, and this still clears
/// 7:1 against everything onColor() would choose it for.
constexpr const char* kDarkInk  = "#111318";
constexpr const char* kLightInk = "#ffffff";

double channel(int v) {
    const double c = v / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double contrastRatio(double a, double b) {
    const double hi = std::max(a, b), lo = std::min(a, b);
    return (hi + 0.05) / (lo + 0.05);
}

// The presets. Each pairs a light and a dark scheme deliberately — a "light"
// site still has to answer a visitor whose OS is dark, and picking those
// tokens by inverting the light ones gives muddy text on a washed ground.
// `midnight` is committed dark: its light scheme IS its dark scheme, so it
// stays dark for every visitor regardless of their OS.
const std::vector<Palette>& table() {
    static const std::vector<Palette> kPresets = {
        { "paper", "Paper", "#0a6f7d",
          { "#ffffff", "#ffffff", "#16202a", "#5d6f7e", "#e2e8ee" },
          { "#101820", "#16212b", "#e7eef4", "#93a5b4", "#26333f" } },

        { "slate", "Slate", "#2f6f9f",
          { "#f6f8fa", "#ffffff", "#16202a", "#5a6b7a", "#dde4ea" },
          { "#0f151b", "#18212a", "#e6edf3", "#8fa1b0", "#232e39" } },

        { "sand", "Sand", "#b0552f",
          { "#faf7f3", "#ffffff", "#241f1a", "#6d6157", "#e7ded3" },
          { "#171310", "#201b17", "#f0e9e1", "#a89a8c", "#2f2822" } },

        { "midnight", "Midnight", "#4cc9c0",
          { "#0e151d", "#151f29", "#e8eff5", "#90a3b3", "#243240" },
          { "#0e151d", "#151f29", "#e8eff5", "#90a3b3", "#243240" } },

        // The ERP's own palette, so the public site and the application a
        // visitor signs into are recognisably the same product. Taken from
        // web/static/src/app.css :root — bg, surface, border, accent, text,
        // muted — not approximated. Committed dark, like the backend.
        { "console", "Backend", "#e94560",
          { "#1a1a2e", "#16213e", "#eaeaea", "#8899aa", "#0f3460" },
          { "#1a1a2e", "#16213e", "#eaeaea", "#8899aa", "#0f3460" } },

        { "contrast", "High contrast", "#0b57d0",
          { "#ffffff", "#ffffff", "#000000", "#3a3a3a", "#767676" },
          { "#000000", "#0a0a0a", "#ffffff", "#c8c8c8", "#8a8a8a" } },
    };
    return kPresets;
}

} // namespace

bool WebsitePalette::isHex(const std::string& v) {
    if (v.size() != 4 && v.size() != 7) return false;
    if (v[0] != '#') return false;
    for (std::size_t i = 1; i < v.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(v[i]))) return false;
    return true;
}

std::string WebsitePalette::normalizeHex(const std::string& v) {
    if (!isHex(v)) return {};
    std::string out;
    out.reserve(7);
    out.push_back('#');
    if (v.size() == 4)                       // #abc -> #aabbcc
        for (std::size_t i = 1; i < 4; ++i) { out.push_back(v[i]); out.push_back(v[i]); }
    else
        out.append(v, 1, 6);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

double WebsitePalette::luminance(const std::string& hex) {
    const std::string h = normalizeHex(hex);
    if (h.empty()) return 0.0;
    const int r = std::stoi(h.substr(1, 2), nullptr, 16);
    const int g = std::stoi(h.substr(3, 2), nullptr, 16);
    const int b = std::stoi(h.substr(5, 2), nullptr, 16);
    return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b);
}

std::string WebsitePalette::onColor(const std::string& hex) {
    if (normalizeHex(hex).empty()) return kLightInk;
    const double a = luminance(hex);
    // Whichever ink is more readable ON this accent actually wins — a bare
    // luminance threshold gets mid-tone accents wrong in both directions.
    return contrastRatio(a, luminance(kDarkInk)) > contrastRatio(a, luminance(kLightInk))
           ? kDarkInk : kLightInk;
}

double WebsitePalette::contrast(const std::string& a, const std::string& b) {
    return contrastRatio(luminance(a), luminance(b));
}

std::string WebsitePalette::mix(const std::string& a, const std::string& b, double ratio) {
    const std::string ha = normalizeHex(a), hb = normalizeHex(b);
    if (ha.empty() || hb.empty()) return ha.empty() ? hb : ha;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;

    char out[8];
    out[0] = '#';
    for (int i = 0; i < 3; ++i) {
        const int ca = std::stoi(ha.substr(1 + i * 2, 2), nullptr, 16);
        const int cb = std::stoi(hb.substr(1 + i * 2, 2), nullptr, 16);
        const int v  = static_cast<int>(ca + (cb - ca) * ratio + 0.5);
        static const char* kHex = "0123456789abcdef";
        out[1 + i * 2]     = kHex[(v >> 4) & 0xF];
        out[1 + i * 2 + 1] = kHex[v & 0xF];
    }
    out[7] = '\0';
    return std::string(out);
}

std::string WebsitePalette::readableOn(const std::string& accent, const std::string& bg,
                                       double minRatio) {
    if (normalizeHex(accent).empty() || normalizeHex(bg).empty()) return accent;
    if (contrast(accent, bg) >= minRatio) return accent;

    // Move AWAY from the ground: darken the accent on a light page, lighten it
    // on a dark one. Twenty 5% steps is enough to reach black or white from
    // anywhere, and stopping at the first step that clears keeps as much of
    // the brand hue as the ratio allows.
    const std::string target = luminance(bg) > 0.5 ? "#000000" : "#ffffff";
    std::string best = accent;
    for (int i = 1; i <= 20; ++i) {
        best = mix(accent, target, i * 0.05);
        if (contrast(best, bg) >= minRatio) return best;
    }
    return best;
}

Accents WebsitePalette::derive(const Scheme& sc, const std::string& accent) {
    Accents a;
    a.tint  = mix(sc.surface, accent, 0.08);
    a.tint2 = mix(sc.surface, accent, 0.16);
    a.soft  = mix(sc.bg,      accent, 0.12);
    a.deep  = mix(accent, "#000000", 0.22);
    a.rule  = mix(sc.line,    accent, 0.45);
    a.text  = readableOn(accent, sc.bg);
    return a;
}

const std::vector<Palette>& WebsitePalette::presets() { return table(); }

const Palette* WebsitePalette::preset(const std::string& key) {
    for (const auto& p : table())
        if (p.key == key) return &p;
    return nullptr;
}

const Palette& WebsitePalette::fallback() { return table().front(); }   // "paper"

DarkMode WebsitePalette::darkModeFromString(const std::string& v) {
    if (v == "off") return DarkMode::Off;
    if (v == "on")  return DarkMode::On;
    return DarkMode::Auto;
}

std::string WebsitePalette::darkModeToString(DarkMode m) {
    switch (m) {
        case DarkMode::Off: return "off";
        case DarkMode::On:  return "on";
        default:            return "auto";
    }
}

} // namespace cerp::modules::website

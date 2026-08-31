#pragma once
// =============================================================
// modules/website/WebsitePalette.hpp — the site's colour tokens (docs/121)
//
// Before this, the public site had exactly one configurable colour. The
// stylesheet carried five tokens and four of them were literals:
//
//     :root{--a:<accent>;--ink:#16202a;--mut:#5d6f7e;--line:#e2e8ee;--bg:#fff}
//
// so every site was black on white no matter what, and the accent was the only
// thing an owner could move. That is not a palette, it is a highlight colour.
//
// A palette here is a PRESET plus overrides. The preset carries a complete,
// deliberately-paired set of tokens for BOTH schemes — a light site still has
// to answer a visitor whose OS is in dark mode — and any single token can be
// overridden without having to restate the other nine.
//
// Two rules this file exists to enforce:
//
//   * Every colour that reaches the stylesheet is a validated literal. A
//     config value is not author-trusted just because an administrator set it;
//     an unvalidated one lands inside a <style> block, which is a CSS
//     injection. Anything that does not parse as a hex colour falls back.
//
//   * Text on the accent is COMPUTED, never assumed. The old CSS hardcoded
//     `color:#fff` on buttons and badges, which silently fails the moment an
//     owner picks a pale accent — white on #ffd166 is unreadable. onColor()
//     picks white or near-black by relative luminance.
// =============================================================
#include <string>
#include <vector>

namespace cerp::modules::website {

/// One scheme's worth of tokens. Every field is a validated `#rrggbb`.
struct Scheme {
    std::string bg;       ///< page ground
    std::string surface;  ///< cards, panels, form fields — may equal bg
    std::string ink;      ///< body text
    std::string mut;      ///< secondary text
    std::string line;     ///< rules and borders
};

/**
 * The accent, spread across a scheme.
 *
 * A single accent gives you one colour to put on buttons. A design that uses
 * colour — a tinted top bar, a highlighted pricing card, a warm footer — needs
 * the accent at several strengths against THIS scheme's ground, and they are
 * not the same colours in light and dark: an 8% wash over white is nearly
 * white, and over #0e151d it is nearly black. So these are derived per scheme
 * rather than written once.
 */
struct Accents {
    std::string tint;   ///< ~8% accent over the surface — a card wash
    std::string tint2;  ///< ~16% — a band, a hovered row
    std::string soft;   ///< ~12% over the GROUND — the hero
    std::string deep;   ///< the accent darkened — gradient end, hover
    std::string rule;   ///< the accent mixed into the border colour
    std::string text;   ///< the accent, adjusted until it is READABLE on bg
};

/// A named pairing of two schemes plus the accent it was designed around.
struct Palette {
    std::string key;
    std::string label;
    std::string accent;   ///< the preset's own accent; `website.accent` overrides
    Scheme light;
    Scheme dark;
};

/// How the dark scheme is reached.
enum class DarkMode {
    Auto,  ///< follow the visitor's OS (a prefers-color-scheme block)
    Off,   ///< always light — no dark block emitted at all
    On,    ///< always dark — the dark tokens ARE the :root tokens
};

class WebsitePalette {
public:
    /**
     * Is this a colour we are willing to paste into a stylesheet?
     * Accepts `#rgb` and `#rrggbb`, case-insensitive. Nothing else — no
     * named colours, no rgb(), no var(), no `red;} body{...`.
     */
    static bool isHex(const std::string& v);

    /// `#abc` → `#aabbcc`; an already-6-digit value is lowercased. UB-free on
    /// anything isHex() rejects: returns the empty string.
    static std::string normalizeHex(const std::string& v);

    /**
     * The readable text colour to place ON `hex` — near-black for a light
     * accent, white for a dark one, chosen by sRGB relative luminance.
     * Returns white for anything that is not a hex colour, matching the
     * behaviour of the accent fallback.
     */
    static std::string onColor(const std::string& hex);

    /// Relative luminance (WCAG), 0.0–1.0. Exposed for testing the threshold.
    static double luminance(const std::string& hex);

    /// WCAG contrast ratio between two colours, 1.0–21.0.
    static double contrast(const std::string& a, const std::string& b);

    /// Linear mix in sRGB. ratio 0.0 → a, 1.0 → b. Clamped.
    static std::string mix(const std::string& a, const std::string& b, double ratio);

    /**
     * `accent` nudged toward the scheme's ink until it clears `minRatio`
     * against `bg`. A brand accent picked for buttons is frequently
     * unreadable as TEXT on the same page — #e94560 on white is 3.9:1 — and
     * the honest fix is to darken it for that one use rather than to ship
     * unreadable links.
     */
    static std::string readableOn(const std::string& accent, const std::string& bg,
                                  double minRatio = 4.5);

    /// Every derived accent tone for one scheme.
    static Accents derive(const Scheme& sc, const std::string& accent);

    /// Every preset, in the order the picker should show them.
    static const std::vector<Palette>& presets();

    /// Look one up by key; nullptr when the key is not a preset.
    static const Palette* preset(const std::string& key);

    /// The preset used when `website.theme` is unset or unrecognised.
    static const Palette& fallback();

    /// "auto" / "off" / "on" → DarkMode; anything else is Auto.
    static DarkMode darkModeFromString(const std::string& v);
    static std::string darkModeToString(DarkMode m);
};

} // namespace cerp::modules::website

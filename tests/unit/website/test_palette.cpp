// ============================================================
// tests/unit/website/test_palette.cpp — the site palette (docs/121)
//
//   ./tests/run.sh --unit --filter Palette
//
// Two jobs here, and they fail in different directions.
//
// The first is SECURITY. Every colour these functions bless is pasted into a
// <style> block on a public page. A validator that lets `red;}body{display:none`
// through is a CSS injection, and one that lets an empty string through paints
// `--bg:;` and takes the site out. So isHex/normalizeHex are tested the way the
// sanitiser is: with the whole catalogue, not a happy path.
//
// The second is READABILITY, which no HTTP test can check. A preset whose muted
// text sits at 3:1 on its own ground renders perfectly, returns 200, and is
// unreadable. Contrast is arithmetic, so it is asserted here rather than left
// to whoever looks at the screenshot.
// ============================================================
#include "WebsitePalette.hpp"
#include "TestHarness.hpp"

#include <string>

using cerp::modules::website::WebsitePalette;
using cerp::modules::website::Scheme;
using cerp::modules::website::DarkMode;
using erptest::section;

namespace {

/// WCAG contrast ratio between two hex colours, 1.0 – 21.0.
double contrast(const std::string& a, const std::string& b) {
    const double la = WebsitePalette::luminance(a);
    const double lb = WebsitePalette::luminance(b);
    const double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

} // namespace

ERP_TEST(Palette, hexValidation) {
    section("what a colour is allowed to be");
    CHECK(WebsitePalette::isHex("#abc"),      "3-digit shorthand");
    CHECK(WebsitePalette::isHex("#aabbcc"),   "6-digit");
    CHECK(WebsitePalette::isHex("#ABCDEF"),   "uppercase");
    CHECK(WebsitePalette::isHex("#000000"),   "black");

    section("everything a colour is not — each of these would reach a <style> block");
    CHECK(!WebsitePalette::isHex(""),                       "empty string");
    CHECK(!WebsitePalette::isHex("#"),                      "bare hash");
    CHECK(!WebsitePalette::isHex("aabbcc"),                 "missing hash");
    CHECK(!WebsitePalette::isHex("#aabbc"),                 "5 digits");
    CHECK(!WebsitePalette::isHex("#aabbccd"),               "7 digits");
    CHECK(!WebsitePalette::isHex("#aabbccdd"),              "8 digits (no alpha)");
    CHECK(!WebsitePalette::isHex("#gggggg"),                "non-hex digits");
    CHECK(!WebsitePalette::isHex("red"),                    "named colour");
    CHECK(!WebsitePalette::isHex("rgb(1,2,3)"),             "rgb()");
    CHECK(!WebsitePalette::isHex("var(--x)"),               "a var reference");
    CHECK(!WebsitePalette::isHex("#fff;}body{display:none"), "the injection");
    CHECK(!WebsitePalette::isHex("#ff f"),                  "embedded space");
    CHECK(!WebsitePalette::isHex("#ff\nff"),                "embedded newline");
    CHECK(!WebsitePalette::isHex("#１２３"),                 "fullwidth digits");

    section("normalisation");
    CHECK(WebsitePalette::normalizeHex("#abc")    == "#aabbcc", "shorthand expands");
    CHECK(WebsitePalette::normalizeHex("#AABBCC") == "#aabbcc", "case is folded");
    CHECK(WebsitePalette::normalizeHex("#A1b2C3") == "#a1b2c3", "mixed case");
    CHECK(WebsitePalette::normalizeHex("nope").empty(),         "a reject yields empty, not garbage");
    CHECK(WebsitePalette::normalizeHex("").empty(),             "empty stays empty");
}

ERP_TEST(Palette, luminanceAndInk) {
    section("the endpoints of the luminance scale");
    CHECK(WebsitePalette::luminance("#ffffff") > 0.99, "white is ~1.0");
    CHECK(WebsitePalette::luminance("#000000") < 0.01, "black is ~0.0");
    CHECK(WebsitePalette::luminance("#ffffff") > WebsitePalette::luminance("#808080"),
          "white outranks mid grey");

    section("text on the accent is chosen, not assumed");
    CHECK(WebsitePalette::onColor("#ffffff") == "#111318", "dark ink on white");
    CHECK(WebsitePalette::onColor("#000000") == "#ffffff", "white ink on black");
    CHECK(WebsitePalette::onColor("#ffd166") == "#111318", "dark ink on a pale yellow");
    CHECK(WebsitePalette::onColor("#0a6f7d") == "#ffffff", "white ink on a deep teal");
    // The case that motivated computing this at all: the old CSS hardcoded
    // white here, which is the LOWER-contrast choice on this pink.
    CHECK(WebsitePalette::onColor("#e94560") == "#111318", "dark ink on #e94560");
    CHECK(contrast("#e94560", "#111318") > contrast("#e94560", "#ffffff"),
          "…and that is the arithmetic reason why");

    section("a value that is not a colour does not produce a broken declaration");
    CHECK(WebsitePalette::onColor("")        == "#ffffff", "empty falls back to white");
    CHECK(WebsitePalette::onColor("nonsense") == "#ffffff", "so does nonsense");

    section("whatever is chosen is actually readable");
    for (const auto& p : WebsitePalette::presets())
        CHECK(contrast(p.accent, WebsitePalette::onColor(p.accent)) >= 4.5,
              p.key + ": ink on the preset accent clears AA");
}

ERP_TEST(Palette, presetsAreLegible) {
    section("every preset colour is a colour");
    for (const auto& p : WebsitePalette::presets()) {
        CHECK(!p.key.empty() && !p.label.empty(), p.key + ": has a key and a label");
        CHECK(WebsitePalette::isHex(p.accent),    p.key + ": accent is hex");
        for (const auto* sc : { &p.light, &p.dark }) {
            CHECK(WebsitePalette::isHex(sc->bg),      p.key + ": bg is hex");
            CHECK(WebsitePalette::isHex(sc->surface), p.key + ": surface is hex");
            CHECK(WebsitePalette::isHex(sc->ink),     p.key + ": ink is hex");
            CHECK(WebsitePalette::isHex(sc->mut),     p.key + ": muted is hex");
            CHECK(WebsitePalette::isHex(sc->line),    p.key + ": line is hex");
        }
    }

    // The check a screenshot cannot make. Body text is AAA (7:1) because it is
    // the whole page; secondary text is held to AA (4.5:1) because it is still
    // text somebody has to read, not decoration.
    section("body text clears AAA on its own ground, in both schemes");
    for (const auto& p : WebsitePalette::presets()) {
        CHECK(contrast(p.light.ink, p.light.bg) >= 7.0,      p.key + ": light ink on bg");
        CHECK(contrast(p.light.ink, p.light.surface) >= 7.0, p.key + ": light ink on surface");
        CHECK(contrast(p.dark.ink,  p.dark.bg) >= 7.0,       p.key + ": dark ink on bg");
        CHECK(contrast(p.dark.ink,  p.dark.surface) >= 7.0,  p.key + ": dark ink on surface");
    }

    section("secondary text clears AA on its own ground, in both schemes");
    for (const auto& p : WebsitePalette::presets()) {
        CHECK(contrast(p.light.mut, p.light.bg) >= 4.5,      p.key + ": light muted on bg");
        CHECK(contrast(p.light.mut, p.light.surface) >= 4.5, p.key + ": light muted on surface");
        CHECK(contrast(p.dark.mut,  p.dark.bg) >= 4.5,       p.key + ": dark muted on bg");
        CHECK(contrast(p.dark.mut,  p.dark.surface) >= 4.5,  p.key + ": dark muted on surface");
    }

    section("a surface is visible against its ground, or is deliberately the same");
    for (const auto& p : WebsitePalette::presets()) {
        const bool same = p.light.surface == p.light.bg;
        CHECK(same || contrast(p.light.surface, p.light.bg) > 1.0,
              p.key + ": light surface either equals bg or differs from it");
    }
}

ERP_TEST(Palette, derivedAccents) {
    section("mixing is a mix");
    CHECK(WebsitePalette::mix("#000000", "#ffffff", 0.0) == "#000000", "ratio 0 is the first");
    CHECK(WebsitePalette::mix("#000000", "#ffffff", 1.0) == "#ffffff", "ratio 1 is the second");
    CHECK(WebsitePalette::mix("#000000", "#ffffff", 0.5) == "#808080", "halfway is halfway");
    CHECK(WebsitePalette::mix("#ff0000", "#0000ff", 0.5) == "#800080", "per channel");
    CHECK(WebsitePalette::mix("#000000", "#ffffff", -3.0) == "#000000", "a negative ratio clamps");
    CHECK(WebsitePalette::mix("#000000", "#ffffff", 9.0)  == "#ffffff", "so does an over-large one");
    CHECK(WebsitePalette::isHex(WebsitePalette::mix("#123456", "#abcdef", 0.37)),
          "an arbitrary mix is still a valid colour");
    // A mix that produced 5 or 7 digits would inject a broken declaration.
    for (int i = 0; i <= 10; ++i)
        CHECK(WebsitePalette::mix("#010203", "#fdfeff", i / 10.0).size() == 7,
              "every mix is exactly #rrggbb");

    section("accent-as-TEXT is darkened until it is readable");
    // The case this exists for: the brand pink is fine on a button and fails
    // as text on white (3.83:1).
    CHECK(WebsitePalette::contrast("#e94560", "#ffffff") < 4.5, "…the raw accent does fail");
    CHECK(WebsitePalette::contrast(WebsitePalette::readableOn("#e94560", "#ffffff"),
                                  "#ffffff") >= 4.5,           "…and the adjusted one does not");
    // On a dark ground it must go the other way.
    const std::string onDark = WebsitePalette::readableOn("#0a6f7d", "#101820");
    CHECK(WebsitePalette::contrast(onDark, "#101820") >= 4.5, "a deep teal is lightened on dark");
    CHECK(WebsitePalette::luminance(onDark) > WebsitePalette::luminance("#0a6f7d"),
          "…lightened, not darkened");
    // An accent that already passes is left alone: adjusting it would throw
    // away brand colour for nothing.
    CHECK(WebsitePalette::readableOn("#0a6f7d", "#ffffff") == "#0a6f7d",
          "an accent that already clears AA is untouched");

    section("every preset derives a usable set, in both schemes");
    for (const auto& p : WebsitePalette::presets()) {
        for (const auto* sc : { &p.light, &p.dark }) {
            const auto a = WebsitePalette::derive(*sc, p.accent);
            CHECK(WebsitePalette::isHex(a.tint),  p.key + ": tint is a colour");
            CHECK(WebsitePalette::isHex(a.tint2), p.key + ": tint2 is a colour");
            CHECK(WebsitePalette::isHex(a.soft),  p.key + ": soft is a colour");
            CHECK(WebsitePalette::isHex(a.deep),  p.key + ": deep is a colour");
            CHECK(WebsitePalette::isHex(a.rule),  p.key + ": rule is a colour");
            CHECK(WebsitePalette::isHex(a.text),  p.key + ": text is a colour");
            // A tint is a BACKGROUND, so the body ink has to survive on it.
            CHECK(contrast(sc->ink, a.tint)  >= 4.5, p.key + ": ink is readable on the tint");
            CHECK(contrast(sc->ink, a.tint2) >= 4.5, p.key + ": ink is readable on tint2");
            CHECK(contrast(sc->ink, a.soft)  >= 4.5, p.key + ": ink is readable on the hero wash");
            // And accent text has to survive on the ground it is placed on.
            CHECK(contrast(a.text, sc->bg)   >= 4.5, p.key + ": accent text on the ground");
        }
    }

    section("a tint is a wash, not a repaint");
    // If the tint were the accent itself, the top bar and the featured card
    // would be solid brand colour and the text on them would be wrong.
    for (const auto& p : WebsitePalette::presets()) {
        const auto a = WebsitePalette::derive(p.light, p.accent);
        CHECK(contrast(a.tint, p.light.surface) < 1.6,
              p.key + ": the tint stays close to its surface");
        CHECK(a.tint != p.accent, p.key + ": …and is not simply the accent");
    }

    section("a mix involving a non-colour degrades instead of emitting junk");
    CHECK(WebsitePalette::mix("nope", "#ffffff", 0.5) == "#ffffff", "one bad side");
    CHECK(WebsitePalette::mix("#ffffff", "", 0.5)     == "#ffffff", "the other");
}

ERP_TEST(Palette, lookupAndModes) {
    section("presets are addressable and distinct");
    CHECK(WebsitePalette::presets().size() >= 4, "there is a real choice to make");
    for (const auto& p : WebsitePalette::presets())
        CHECK(WebsitePalette::preset(p.key) != nullptr, p.key + ": found by its own key");
    for (std::size_t i = 0; i < WebsitePalette::presets().size(); ++i)
        for (std::size_t j = i + 1; j < WebsitePalette::presets().size(); ++j)
            CHECK(WebsitePalette::presets()[i].key != WebsitePalette::presets()[j].key,
                  "keys are unique");

    section("an unknown key is a miss, not a crash — this is the SQL/config allowlist");
    CHECK(WebsitePalette::preset("")            == nullptr, "empty");
    CHECK(WebsitePalette::preset("../../etc")   == nullptr, "traversal");
    CHECK(WebsitePalette::preset("PAPER")       == nullptr, "case is significant");
    CHECK(WebsitePalette::preset("paper'; --")  == nullptr, "sql-ish");
    CHECK(WebsitePalette::fallback().key == "paper", "the fallback is the original look");

    section("dark mode is a closed set");
    CHECK(WebsitePalette::darkModeFromString("auto") == DarkMode::Auto, "auto");
    CHECK(WebsitePalette::darkModeFromString("off")  == DarkMode::Off,  "off");
    CHECK(WebsitePalette::darkModeFromString("on")   == DarkMode::On,   "on");
    CHECK(WebsitePalette::darkModeFromString("yes")  == DarkMode::Auto, "junk means auto");
    CHECK(WebsitePalette::darkModeFromString("")     == DarkMode::Auto, "empty means auto");
    CHECK(WebsitePalette::darkModeToString(DarkMode::On) == "on", "round-trips");

    section("midnight is committed dark — it does not change with the visitor's OS");
    const auto* mid = WebsitePalette::preset("midnight");
    CHECK(mid != nullptr, "midnight exists");
    if (mid) CHECK(mid->light.bg == mid->dark.bg, "its light ground IS its dark ground");
}

#include "theme/import.h"
#include "theme/theme.h"
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kt = krait::app::theme;

namespace {

// An iTerm2 file with the minimum a real one has: sixteen "Ansi N Color" dicts
// plus foreground/background. Components are 0..1 reals, which is the part that
// separates this format from every other one.
std::string itermSample() {
    std::string xml =
        R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>)";
    for (int i = 0; i < 16; ++i) {
        // A ramp, so a swapped index shows up as a wrong value rather than as
        // sixteen identical colours that any mapping would pass.
        const double level = static_cast<double>(i) / 15.0;
        xml += "<key>Ansi " + std::to_string(i) + " Color</key><dict>";
        xml += "<key>Blue Component</key><real>0</real>";
        xml += "<key>Green Component</key><real>" + std::to_string(level) + "</real>";
        xml += "<key>Red Component</key><real>1</real>";
        xml += "</dict>";
    }
    xml += R"(<key>Background Color</key><dict>
        <key>Red Component</key><real>0</real>
        <key>Green Component</key><real>0</real>
        <key>Blue Component</key><real>0.2</real></dict>
      <key>Foreground Color</key><dict>
        <key>Red Component</key><real>1</real>
        <key>Green Component</key><real>1</real>
        <key>Blue Component</key><real>1</real></dict>
      <key>Cursor Color</key><dict>
        <key>Red Component</key><real>1</real>
        <key>Green Component</key><real>0</real>
        <key>Blue Component</key><real>0</real></dict>
    </dict></plist>)";
    return xml;
}

std::string wtSample() {
    return R"({
      "schemes": [
        {
          "name": "Campbell",
          "black": "#0C0C0C", "red": "#C50F1F", "green": "#13A10E", "yellow": "#C19C00",
          "blue": "#0037DA", "purple": "#881798", "cyan": "#3A96DD", "white": "#CCCCCC",
          "brightBlack": "#767676", "brightRed": "#E74856", "brightGreen": "#16C60C",
          "brightYellow": "#F9F1A5", "brightBlue": "#3B78FF", "brightPurple": "#B4009E",
          "brightCyan": "#61D6D6", "brightWhite": "#F2F2F2",
          "background": "#0C0C0C", "foreground": "#CCCCCC",
          "cursorColor": "#FFFFFF", "selectionBackground": "#FFFFFF"
        },
        { "name": "Broken", "black": "#000000" }
      ]
    })";
}

std::string base16Sample() {
    return R"(scheme: "Ocean"
author: "Chris Kempson"
base00: "2b303b"
base01: "343d46"
base02: "4f5b66"
base03: "65737e"
base04: "a7adba"
base05: "c0c5ce"
base06: "dfe1e8"
base07: "eff1f5"
base08: "bf616a"
base09: "d08770"
base0A: "ebcb8b"
base0B: "a3be8c"
base0C: "96b5b4"
base0D: "8fa1b3"
base0E: "b48ead"
base0F: "ab7967"
)";
}

}  // namespace

TEST_CASE("colours parse in every form the config and the wire use", "[theme]") {
    CHECK(kt::parseColor("#abc") == 0xAABBCC);
    CHECK(kt::parseColor("#AABBCC") == 0xAABBCC);
    CHECK(kt::parseColor("  #aabbcc ") == 0xAABBCC);
    // XParseColor scales by FIELD WIDTH: one 'f' is full intensity, not 0x0F.
    CHECK(kt::parseColor("rgb:f/f/f") == 0xFFFFFF);
    CHECK(kt::parseColor("rgb:ffff/0000/8080") == 0xFF0080);
    CHECK(kt::parseColor("rgb:ff/00/80") == 0xFF0080);

    CHECK(kt::parseColor("") == std::nullopt);
    CHECK(kt::parseColor("#ab") == std::nullopt);
    CHECK(kt::parseColor("#abcde") == std::nullopt);
    CHECK(kt::parseColor("#gggggg") == std::nullopt);
    CHECK(kt::parseColor("rgb:ff/00") == std::nullopt);
    CHECK(kt::parseColor("rgb:ff/00/80/40") == std::nullopt);
    CHECK(kt::parseColor("rgb:fffff/0/0") == std::nullopt);
    CHECK(kt::parseColor("cornflowerblue") == std::nullopt);
}

TEST_CASE("formatColor round-trips", "[theme]") {
    CHECK(kt::formatColor(0x1E1E2E) == "#1e1e2e");
    CHECK(kt::formatColor(0) == "#000000");
    CHECK(kt::formatColor(0xFFFFFF) == "#ffffff");
    CHECK(kt::parseColor(kt::formatColor(0x89B4FA)) == 0x89B4FA);
}

TEST_CASE("every builtin is complete and named", "[theme]") {
    const std::span<const kt::Theme> themes = kt::builtins();
    REQUIRE(themes.size() >= 2);
    for (const kt::Theme& theme : themes) {
        INFO(theme.name);
        CHECK_FALSE(theme.name.empty());
        // A builtin with an all-zero chrome would paint the whole window black
        // — which is what the derivation pass in themeTable() exists to
        // prevent, and it is invisible in any test that only reads a palette.
        CHECK(theme.chrome != kt::Chrome{});
        CHECK(theme.chrome.text != theme.chrome.bg);
    }
    CHECK(kt::find(themes, "default-dark") != nullptr);
    CHECK(kt::find(themes, "default-light") != nullptr);
    CHECK(kt::find(themes, "nope") == nullptr);
}

TEST_CASE("light and dark are told apart by the background", "[theme]") {
    const std::span<const kt::Theme> themes = kt::builtins();
    CHECK(kt::isDark(*kt::find(themes, "default-dark")));
    CHECK(kt::isDark(*kt::find(themes, "solarized-dark")));
    CHECK_FALSE(kt::isDark(*kt::find(themes, "default-light")));
    // The Solarized light background (#fdf6e3) is the one that matters: it is
    // not white, and a naive threshold on the raw byte value gets it wrong.
    CHECK_FALSE(kt::isDark(*kt::find(themes, "solarized-light")));
}

TEST_CASE("a theme survives a TOML round trip", "[theme]") {
    for (const kt::Theme& original : kt::builtins()) {
        INFO(original.name);
        std::string error;
        const std::optional<kt::Theme> reread = kt::parseToml(kt::toToml(original), &error);
        REQUIRE(reread.has_value());
        CHECK(error.empty());
        // Full equality, chrome included. Writing a token that cannot be read
        // back is how the live editor silently loses somebody's accent colour.
        CHECK(*reread == original);
    }
}

TEST_CASE("a theme file may declare only the chrome it wants to change", "[theme]") {
    const kt::Theme& dark = *kt::find(kt::builtins(), "solarized-dark");
    std::string toml = kt::toToml(dark);
    // Strip the chrome table entirely: what is left must still derive.
    toml = toml.substr(0, toml.find("[chrome]"));
    std::string error;
    const std::optional<kt::Theme> derived = kt::parseToml(toml, &error);
    REQUIRE(derived.has_value());
    CHECK(derived->chrome == dark.chrome);

    // Now one override on top of the derivation.
    const std::optional<kt::Theme> tweaked =
        kt::parseToml(toml + "\n[chrome]\naccent = \"#ff0000\"\n", &error);
    REQUIRE(tweaked.has_value());
    CHECK(tweaked->chrome.accent == 0xFF0000);
    CHECK(tweaked->chrome.surface == dark.chrome.surface);
}

TEST_CASE("a broken theme file reports rather than degrades", "[theme]") {
    std::string error;
    CHECK(kt::parseToml("this is not toml", &error) == std::nullopt);
    CHECK_FALSE(error.empty());

    error.clear();
    CHECK(kt::parseToml("ansi = []\n", &error) == std::nullopt);
    CHECK_FALSE(error.empty());

    // Fifteen colours, not sixteen: the shape a hand-edited file ends up in.
    error.clear();
    std::string fifteen = "name = \"x\"\nansi = [";
    for (int i = 0; i < 15; ++i) {
        fifteen += "\"#000000\",";
    }
    fifteen += "]\n";
    CHECK(kt::parseToml(fifteen, &error) == std::nullopt);
    CHECK_FALSE(error.empty());
}

TEST_CASE("iTerm2 colours import", "[theme][import]") {
    std::string error;
    const std::vector<kt::Theme> themes = kt::importITerm2(itermSample(), &error);
    REQUIRE(themes.size() == 1);
    const kt::Theme& theme = themes.front();
    // 0..1 reals, so index 15 is full green and index 0 is none of it.
    CHECK(theme.ansi[0] == 0xFF0000);
    CHECK(theme.ansi[15] == 0xFFFF00);
    CHECK(theme.bg == 0x000033);
    CHECK(theme.fg == 0xFFFFFF);
    CHECK(theme.cursor == 0xFF0000);
    // No Cursor Text Color in the file: it falls back to the background rather
    // than to black, so the cursor stays readable on a non-black theme.
    CHECK(theme.cursorText == theme.bg);
    CHECK(theme.chrome != kt::Chrome{});
}

TEST_CASE("Windows Terminal schemes import, and one bad scheme costs only itself",
          "[theme][import]") {
    std::string error;
    const std::vector<kt::Theme> themes = kt::importWindowsTerminal(wtSample(), &error);
    REQUIRE(themes.size() == 1);
    CHECK(themes.front().name == "Campbell");
    CHECK(themes.front().ansi[0] == 0x0C0C0C);
    CHECK(themes.front().ansi[5] == 0x881798);  // "purple", Microsoft's spelling
    CHECK(themes.front().ansi[15] == 0xF2F2F2);
    CHECK(themes.front().bg == 0x0C0C0C);
    CHECK(themes.front().chrome.accent == 0x3B78FF);  // derived: bright blue

    // A bare scheme object, not wrapped in "schemes".
    const std::vector<kt::Theme> bare = kt::importWindowsTerminal(
        R"({"name":"Bare","black":"#000000","red":"#010000","green":"#000100",
            "yellow":"#010100","blue":"#000001","purple":"#010001","cyan":"#000101",
            "white":"#010101","brightBlack":"#020000","brightRed":"#030000",
            "brightGreen":"#000300","brightYellow":"#030300","brightBlue":"#000003",
            "brightPurple":"#030003","brightCyan":"#000303","brightWhite":"#030303",
            "background":"#000000","foreground":"#cccccc"})",
        &error);
    REQUIRE(bare.size() == 1);
    CHECK(bare.front().name == "Bare");
}

TEST_CASE("base16 YAML imports through the base16-shell mapping", "[theme][import]") {
    std::string error;
    const std::vector<kt::Theme> themes = kt::importBase16(base16Sample(), &error);
    REQUIRE(themes.size() == 1);
    const kt::Theme& theme = themes.front();
    CHECK(theme.name == "Ocean");
    CHECK(theme.bg == 0x2B303B);        // base00
    CHECK(theme.fg == 0xC0C5CE);        // base05
    CHECK(theme.ansi[1] == 0xBF616A);   // red    <- base08
    CHECK(theme.ansi[2] == 0xA3BE8C);   // green  <- base0B
    CHECK(theme.ansi[3] == 0xEBCB8B);   // yellow <- base0A
    CHECK(theme.ansi[4] == 0x8FA1B3);   // blue   <- base0D
    CHECK(theme.ansi[8] == 0x65737E);   // bright black <- base03
    CHECK(theme.ansi[15] == 0xEFF1F5);  // bright white <- base07
    CHECK(theme.selectionBg == 0x4F5B66);

    // Missing one entry is a refusal, not fifteen colours and a black hole.
    std::string truncated = base16Sample();
    truncated = truncated.substr(0, truncated.find("base0F"));
    error.clear();
    CHECK(kt::importBase16(truncated, &error).empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("importAny picks a reader from the name, or from the bytes", "[theme][import]") {
    std::string error;
    CHECK(kt::importAny("Ocean.yaml", base16Sample(), &error).size() == 1);
    CHECK(kt::importAny("scheme.json", wtSample(), &error).size() == 1);
    CHECK(kt::importAny("x.itermcolors", itermSample(), &error).size() == 1);
    // No extension: sniffed from the first non-space byte.
    CHECK(kt::importAny("", "  " + wtSample(), &error).size() == 1);
    CHECK(kt::importAny("", itermSample(), &error).size() == 1);
    CHECK(kt::importAny("", base16Sample(), &error).size() == 1);
    // Our own format, so a theme exported from the gallery can be re-imported.
    const std::string toml = kt::toToml(kt::builtins().front());
    const std::vector<kt::Theme> ours = kt::importAny("mine.toml", toml, &error);
    REQUIRE(ours.size() == 1);
    CHECK(ours.front().ansi == kt::builtins().front().ansi);
}

TEST_CASE("theme file names are safe to write", "[theme][import]") {
    CHECK(kt::themeFileName("Solarized Dark") == "solarized-dark.toml");
    CHECK(kt::themeFileName("../../etc/passwd") == "etc-passwd.toml");
    CHECK(kt::themeFileName("  ") == "theme.toml");
    CHECK(kt::themeFileName("***") == "theme.toml");
    CHECK(kt::themeFileName(std::string(200, 'a')).size() == 64 + 5);
}

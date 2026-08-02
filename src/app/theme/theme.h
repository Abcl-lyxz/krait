#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace krait::app::theme {

// sRGB 0xRRGGBB — the same representation render::Theme uses, so handing a
// theme to the renderer is a copy rather than a conversion. No alpha: a
// terminal colour is opaque by definition, and the one place transparency
// matters (the background image, T78) is a separate setting rather than a
// fourth channel every palette entry would have to carry.
using Rgb = std::uint32_t;

// The UI chrome tokens.
//
// rules/ui.md: "Theme tokens (colors, spacing, radii) come from the theme
// system; hex literals in QML are a defect." This struct is the colour half of
// that list, and it is deliberately SHORT — fourteen names that every surface
// in the app has to be expressible in. The alternative (a token per surface)
// makes an imported theme unusable, because an iTerm2 file has sixteen ANSI
// colours and nothing else: everything here must be derivable from a terminal
// palette alone (see deriveChrome), or imported themes could only ever colour
// the grid and would leave the chrome stuck in whatever the last theme was.
struct Chrome {
    Rgb bg = 0;          // the window itself
    Rgb surface = 0;     // bars and panels laid on the window
    Rgb surfaceAlt = 0;  // chips, buttons, tabs — raised off a surface
    Rgb overlay = 0;     // popups: the palette, sheets
    Rgb border = 0;      // dividers and outlines
    Rgb selection = 0;   // the selected row in a list
    Rgb text = 0;        // primary
    Rgb textDim = 0;     // secondary — labels, subtitles
    Rgb textFaint = 0;   // tertiary — hints, shortcut glyphs
    Rgb accent = 0;      // focus, links, the primary action
    Rgb success = 0;
    Rgb warning = 0;
    Rgb danger = 0;
    Rgb scrim = 0;  // modal dimming; always used with an opacity

    friend bool operator==(const Chrome&, const Chrome&) = default;
};

// One theme: the terminal palette, plus the chrome derived from or declared
// alongside it.
struct Theme {
    std::string name;
    // "builtin" or the path it was read from. Shown in the gallery, because
    // "why is there a second Solarized" is otherwise unanswerable.
    std::string source = "builtin";

    // 0-7 normal, 8-15 bright. Indices 16-255 are the fixed xterm cube and are
    // NOT themeable — every terminal agrees on them, and a theme that moved
    // them would break every program that computes a colour from an index.
    std::array<Rgb, 16> ansi{};
    Rgb fg = 0;
    Rgb bg = 0;
    Rgb cursor = 0;
    Rgb cursorText = 0;
    Rgb selectionBg = 0;
    Rgb highlightBg = 0;

    Chrome chrome{};

    friend bool operator==(const Theme&, const Theme&) = default;
};

// "#rgb", "#rrggbb", "rgb:RR/GG/BB" and "rgb:RRRR/GGGG/BBBB" (the XParseColor
// forms OSC 4/10/11 arrive in, so one parser serves both the config file and
// the wire). Nullopt rather than a fallback colour: a theme file with a typo
// has to SAY so, because a silently-black token is indistinguishable from a
// deliberate one.
std::optional<Rgb> parseColor(std::string_view text);

// Always "#rrggbb" — the form a human edits and every other terminal writes.
std::string formatColor(Rgb color);

// Relative luminance, 0..1 (WCAG's coefficients, gamma-decoded).
double luminance(Rgb color);

// Whether the theme reads as dark. Derived from the background rather than
// stored, so a hand-edited file cannot claim "light" while painting black —
// mode 2031 and OSC 11 both answer from this, and a wrong answer makes every
// theme-aware program on the far end pick the unreadable palette.
bool isDark(const Theme& theme);

// Fills every chrome token from the terminal palette. This is what an imported
// theme gets: iTerm2, Windows Terminal and base16 files all carry a terminal
// palette and no chrome, so without this an import could only colour the grid.
Chrome deriveChrome(const Theme& theme);

// The themes compiled into the binary. There is always at least one, and
// `builtins()[0]` is the fallback for a name that resolves to nothing —
// startup cannot be allowed to fail on a missing theme file.
std::span<const Theme> builtins();

// Null when no theme in `themes` is called `name`.
const Theme* find(std::span<const Theme> themes, std::string_view name);

// A theme file. Errors are values (rules/cpp.md — toml++ runs with
// TOML_EXCEPTIONS=0), and `error` gets a message on failure.
//
// A file may declare chrome or leave it out; what it leaves out is derived.
// Partial chrome is allowed too, which is the case that matters in the live
// editor: "this theme but with a red accent" should be three lines, not
// fourteen.
std::optional<Theme> parseToml(std::string_view text, std::string* error);

// Round-trips through parseToml. Chrome is always written out in full, because
// the file this produces is the one the live editor saves and a token that
// vanished on save would silently revert the next time the palette changed.
std::string toToml(const Theme& theme);

}  // namespace krait::app::theme

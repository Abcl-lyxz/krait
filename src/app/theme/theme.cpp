#include "theme/theme.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace krait::app::theme {
namespace {

int hexDigit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

// One XParseColor component: 1-4 hex digits, scaled to 8 bits. "f" is 0xFF and
// not 0x0F — the digits are a FRACTION of the field width, which is why
// "rgb:f/f/f" and "rgb:ffff/ffff/ffff" are both white.
std::optional<std::uint32_t> parseScaledHex(std::string_view part) {
    if (part.empty() || part.size() > 4) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char ch : part) {
        const int digit = hexDigit(ch);
        if (digit < 0) {
            return std::nullopt;
        }
        value = value * 16 + static_cast<std::uint32_t>(digit);
    }
    const std::uint32_t maximum = (1U << (4 * part.size())) - 1U;
    return (value * 255U + maximum / 2U) / maximum;
}

std::uint8_t channel(Rgb color, int shift) {
    return static_cast<std::uint8_t>((color >> shift) & 0xFFU);
}

Rgb pack(std::uint32_t r, std::uint32_t g, std::uint32_t b) {
    return (r << 16) | (g << 8) | b;
}

// Per-channel linear interpolation in sRGB space. Deliberately NOT
// linear-light: these are chrome tokens, and mixing a border 20% toward the
// foreground in linear light lands visibly lighter than every designer's
// intuition and than every hand-picked value it has to sit next to.
Rgb mix(Rgb from, Rgb to, double amount) {
    const auto lerp = [amount](std::uint8_t a, std::uint8_t b) {
        return static_cast<std::uint32_t>(
            std::lround(static_cast<double>(a) + (static_cast<double>(b) - a) * amount));
    };
    return pack(lerp(channel(from, 16), channel(to, 16)), lerp(channel(from, 8), channel(to, 8)),
                lerp(channel(from, 0), channel(to, 0)));
}

// The M4 chrome, lifted verbatim from the hex literals it replaces. Named
// separately from the palette so default-dark keeps the exact window it has
// today: deriving it from the Catppuccin background would lighten every bar in
// the app, which is a redesign rather than a theme system.
constexpr Chrome kDarkChrome{
    .bg = 0x0D0F17,
    .surface = 0x12141C,
    .surfaceAlt = 0x1C2030,
    .overlay = 0x161923,
    .border = 0x2C3242,
    .selection = 0x243049,
    .text = 0xE6E9F0,
    .textDim = 0x7C869E,
    .textFaint = 0x5B6478,
    .accent = 0x89B4FA,
    .success = 0xA6E3A1,
    .warning = 0xF9E2AF,
    .danger = 0xF38BA8,
    .scrim = 0x000000,
};

constexpr Chrome kLightChrome{
    .bg = 0xFFFFFF,
    .surface = 0xF3F4F6,
    .surfaceAlt = 0xE7E9ED,
    .overlay = 0xFFFFFF,
    .border = 0xD3D6DC,
    .selection = 0xDCE6F8,
    .text = 0x1F2228,
    .textDim = 0x5B6270,
    .textFaint = 0x8A909C,
    .accent = 0x4078F2,
    .success = 0x50A14F,
    .warning = 0xB7791F,
    .danger = 0xD33A3A,
    .scrim = 0x000000,
};

// Solarized (Ethan Schoonover). One palette, two backgrounds — which is the
// whole point of it, and the reason both ship: they are the cheapest possible
// proof that a light theme is not a special case somewhere in the pipeline.
constexpr std::array<Rgb, 16> kSolarized{
    0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682, 0x2AA198, 0xEEE8D5,
    0x002B36, 0xCB4B16, 0x586E75, 0x657B83, 0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3,
};

const std::vector<Theme>& themeTable() {
    // Built once, then completed: the two Solarizeds declare no chrome and get
    // it derived here. Doing it at construction rather than at every read means
    // builtins() hands out the same finished theme a file would produce, so
    // nothing downstream has to know which builtins declared chrome.
    static const std::vector<Theme> kThemes = [] {
        std::vector<Theme> themes{
            {
                .name = "default-dark",
                .ansi = {0x45475A, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5,
                         0xBAC2DE, 0x585B70, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7,
                         0x94E2D5, 0xA6ADC8},
                .fg = 0xCDD6F4,
                .bg = 0x1E1E2E,
                .cursor = 0xF5E0DC,
                .cursorText = 0x1E1E2E,
                .selectionBg = 0x45475A,
                .highlightBg = 0xF9E2AF,
                .chrome = kDarkChrome,
            },
            {
                .name = "default-light",
                .ansi = {0x383A42, 0xE45649, 0x50A14F, 0xC18401, 0x4078F2, 0xA626A4, 0x0184BC,
                         0xA0A1A7, 0x696C77, 0xE45649, 0x50A14F, 0xC18401, 0x4078F2, 0xA626A4,
                         0x0184BC, 0x383A42},
                .fg = 0x383A42,
                .bg = 0xFAFAFA,
                .cursor = 0x526FFF,
                .cursorText = 0xFAFAFA,
                .selectionBg = 0xD0D4DC,
                .highlightBg = 0xFFE9A8,
                .chrome = kLightChrome,
            },
            // Chrome LEFT EMPTY on purpose for both Solarizeds: they are the
            // builtin that exercises deriveChrome, so the derivation cannot rot
            // behind an import path nothing in the default install runs.
            {
                .name = "solarized-dark",
                .ansi = kSolarized,
                .fg = 0x839496,
                .bg = 0x002B36,
                .cursor = 0x93A1A1,
                .cursorText = 0x002B36,
                .selectionBg = 0x073642,
                .highlightBg = 0xB58900,
            },
            {
                .name = "solarized-light",
                .ansi = kSolarized,
                .fg = 0x657B83,
                .bg = 0xFDF6E3,
                .cursor = 0x586E75,
                .cursorText = 0xFDF6E3,
                .selectionBg = 0xEEE8D5,
                .highlightBg = 0xB58900,
            },
        };
        for (Theme& theme : themes) {
            if (theme.chrome == Chrome{}) {
                theme.chrome = deriveChrome(theme);
            }
        }
        return themes;
    }();
    return kThemes;
}

// The one table both parseToml and toToml walk, so a token cannot be readable
// and unwritable (or the reverse) — which is exactly how a live editor loses
// somebody's accent colour on save.
struct ChromeField {
    std::string_view key;
    Rgb Chrome::* member;
};

constexpr std::array<ChromeField, 14> kChromeFields{{
    {"bg", &Chrome::bg},
    {"surface", &Chrome::surface},
    {"surfaceAlt", &Chrome::surfaceAlt},
    {"overlay", &Chrome::overlay},
    {"border", &Chrome::border},
    {"selection", &Chrome::selection},
    {"text", &Chrome::text},
    {"textDim", &Chrome::textDim},
    {"textFaint", &Chrome::textFaint},
    {"accent", &Chrome::accent},
    {"success", &Chrome::success},
    {"warning", &Chrome::warning},
    {"danger", &Chrome::danger},
    {"scrim", &Chrome::scrim},
}};

struct PaletteField {
    std::string_view key;
    Rgb Theme::* member;
};

constexpr std::array<PaletteField, 6> kPaletteFields{{
    {"fg", &Theme::fg},
    {"bg", &Theme::bg},
    {"cursor", &Theme::cursor},
    {"cursorText", &Theme::cursorText},
    {"selection", &Theme::selectionBg},
    {"highlight", &Theme::highlightBg},
}};

}  // namespace

std::optional<Rgb> parseColor(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    if (text.front() == '#') {
        const std::string_view digits = text.substr(1);
        if (digits.size() != 3 && digits.size() != 6) {
            return std::nullopt;
        }
        std::array<std::uint32_t, 6> nibbles{};
        for (std::size_t i = 0; i < digits.size(); ++i) {
            const int digit = hexDigit(digits[i]);
            if (digit < 0) {
                return std::nullopt;
            }
            nibbles[i] = static_cast<std::uint32_t>(digit);
        }
        if (digits.size() == 3) {
            // #abc is #aabbcc, not #0a0b0c: the CSS reading, which is what
            // anyone hand-writing three digits means.
            return pack(nibbles[0] * 17, nibbles[1] * 17, nibbles[2] * 17);
        }
        return pack(nibbles[0] * 16 + nibbles[1], nibbles[2] * 16 + nibbles[3],
                    nibbles[4] * 16 + nibbles[5]);
    }

    // XParseColor's "rgb:R/G/B". OSC 4/10/11 answers are written in it, and
    // some senders use it too, so the theme parser and the wire share one
    // reader rather than growing a second nearly-identical one.
    constexpr std::string_view kPrefix = "rgb:";
    if (text.size() > kPrefix.size() && text.substr(0, kPrefix.size()) == kPrefix) {
        std::string_view rest = text.substr(kPrefix.size());
        std::array<std::uint32_t, 3> parts{};
        for (std::size_t i = 0; i < 3; ++i) {
            const std::size_t slash = rest.find('/');
            const std::string_view part = rest.substr(0, slash);
            const std::optional<std::uint32_t> scaled = parseScaledHex(part);
            if (!scaled) {
                return std::nullopt;
            }
            parts[i] = *scaled;
            if (i < 2) {
                if (slash == std::string_view::npos) {
                    return std::nullopt;
                }
                rest.remove_prefix(slash + 1);
            } else if (slash != std::string_view::npos) {
                return std::nullopt;  // a fourth component is not a colour
            }
        }
        return pack(parts[0], parts[1], parts[2]);
    }

    return std::nullopt;
}

std::string formatColor(Rgb color) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out = "#......";
    for (int i = 0; i < 6; ++i) {
        const auto nibble = static_cast<std::size_t>((color >> (20 - 4 * i)) & 0xFU);
        out[static_cast<std::size_t>(i) + 1] = kDigits[nibble];
    }
    return out;
}

double luminance(Rgb color) {
    const auto linear = [](std::uint8_t value) {
        const double srgb = static_cast<double>(value) / 255.0;
        return srgb <= 0.04045 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(channel(color, 16)) + 0.7152 * linear(channel(color, 8)) +
           0.0722 * linear(channel(color, 0));
}

bool isDark(const Theme& theme) {
    // 0.18 rather than 0.5: relative luminance is not perceptual lightness, and
    // a mid grey (#808080) lands at 0.216 — visibly a dark background to sit
    // white text on, but "light" under a naive half-way split.
    return luminance(theme.bg) < 0.18;
}

Chrome deriveChrome(const Theme& theme) {
    const Rgb base = theme.bg;
    const Rgb ink = theme.fg;
    return Chrome{
        .bg = base,
        .surface = mix(base, ink, 0.06),
        .surfaceAlt = mix(base, ink, 0.13),
        .overlay = mix(base, ink, 0.09),
        .border = mix(base, ink, 0.22),
        .selection = theme.selectionBg,
        .text = ink,
        .textDim = mix(base, ink, 0.68),
        .textFaint = mix(base, ink, 0.46),
        // The BRIGHT half of the palette, which is what a status colour has to
        // come from: normal-intensity red on a dark background is the colour
        // every theme picks for text, and it does not carry as a chip.
        .accent = theme.ansi[12],
        .success = theme.ansi[10],
        .warning = theme.ansi[11],
        .danger = theme.ansi[9],
        .scrim = 0x000000,
    };
}

std::span<const Theme> builtins() {
    return themeTable();
}

const Theme* find(std::span<const Theme> themes, std::string_view name) {
    const auto it = std::ranges::find(themes, name, &Theme::name);
    return it == themes.end() ? nullptr : &*it;
}

std::optional<Theme> parseToml(std::string_view text, std::string* error) {
    const auto fail = [error](std::string message) -> std::optional<Theme> {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return std::nullopt;
    };

    const toml::parse_result result = toml::parse(text);
    if (!result) {
        return fail(std::string(result.error().description()));
    }
    const toml::table& doc = result.table();

    Theme theme;
    if (const toml::node* node = doc.get("name"); node != nullptr) {
        const std::optional<std::string> name = node->value<std::string>();
        if (!name) {
            return fail("name must be a string");
        }
        theme.name = *name;
    }
    if (theme.name.empty()) {
        return fail("theme file has no name");
    }

    // The palette is REQUIRED in full. A theme missing half its colours would
    // silently paint them black, and black-on-black is indistinguishable from
    // a theme that simply did not load.
    const toml::array* ansi = doc["ansi"].as_array();
    if (ansi == nullptr || ansi->size() != 16) {
        return fail("ansi must be an array of 16 colours");
    }
    for (std::size_t i = 0; i < 16; ++i) {
        const std::optional<std::string> entry = ansi->get(i)->value<std::string>();
        const std::optional<Rgb> color = entry ? parseColor(*entry) : std::nullopt;
        if (!color) {
            return fail("ansi[" + std::to_string(i) + "] is not a colour");
        }
        theme.ansi[i] = *color;
    }
    for (const PaletteField& field : kPaletteFields) {
        const toml::node* node = doc.get(field.key);
        const std::optional<std::string> entry =
            node != nullptr ? node->value<std::string>() : std::nullopt;
        const std::optional<Rgb> color = entry ? parseColor(*entry) : std::nullopt;
        if (!color) {
            return fail(std::string(field.key) + " is missing or is not a colour");
        }
        theme.*field.member = *color;
    }

    // Chrome is optional, and PARTIAL chrome is the case that matters: the
    // whole point of deriving is that "this palette but with a different
    // accent" is one line in a file.
    theme.chrome = deriveChrome(theme);
    if (const toml::table* chrome = doc["chrome"].as_table(); chrome != nullptr) {
        for (const ChromeField& field : kChromeFields) {
            const toml::node* node = chrome->get(field.key);
            if (node == nullptr) {
                continue;
            }
            const std::optional<std::string> entry = node->value<std::string>();
            const std::optional<Rgb> color = entry ? parseColor(*entry) : std::nullopt;
            if (!color) {
                return fail("chrome." + std::string(field.key) + " is not a colour");
            }
            theme.chrome.*field.member = *color;
        }
    }
    return theme;
}

std::string toToml(const Theme& theme) {
    std::string out;
    out += "name = \"";
    // Names come from an imported file, so a quote in one would produce an
    // unparseable document. Dropped rather than escaped: a theme name is a
    // label, and no label needs a quote badly enough to carry an escaper.
    for (const char ch : theme.name) {
        if (ch != '"' && ch != '\\' && static_cast<unsigned char>(ch) >= 0x20) {
            out += ch;
        }
    }
    out += "\"\n\nansi = [\n";
    for (std::size_t i = 0; i < theme.ansi.size(); ++i) {
        out += "    \"" + formatColor(theme.ansi[i]) + "\",";
        out += i == 7 ? "  # 0-7 normal\n" : (i == 15 ? "  # 8-15 bright\n" : "\n");
    }
    out += "]\n\n";
    for (const PaletteField& field : kPaletteFields) {
        out += std::string(field.key) + " = \"" + formatColor(theme.*field.member) + "\"\n";
    }
    out += "\n[chrome]\n";
    for (const ChromeField& field : kChromeFields) {
        out += std::string(field.key) + " = \"" + formatColor(theme.chrome.*field.member) + "\"\n";
    }
    return out;
}

}  // namespace krait::app::theme

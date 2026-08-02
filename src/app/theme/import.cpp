#include "theme/import.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QXmlStreamReader>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>

namespace krait::app::theme {
namespace {

QString qstr(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

Rgb pack(double r, double g, double b) {
    const auto byteOf = [](double component) {
        const double clamped = component < 0.0 ? 0.0 : (component > 1.0 ? 1.0 : component);
        return static_cast<std::uint32_t>(std::lround(clamped * 255.0));
    };
    return (byteOf(r) << 16) | (byteOf(g) << 8) | byteOf(b);
}

// Everything an importer fills before the palette is complete enough to become
// a Theme. Held as optionals because "absent" and "black" are different: a
// scheme with no cursorColor should inherit the foreground, not go invisible.
struct Draft {
    std::string name;
    std::array<std::optional<Rgb>, 16> ansi{};
    std::optional<Rgb> fg;
    std::optional<Rgb> bg;
    std::optional<Rgb> cursor;
    std::optional<Rgb> cursorText;
    std::optional<Rgb> selection;
};

// The one place a partial import becomes a whole theme. Every fallback here is
// what the source format's own renderer does when the key is missing, so an
// imported theme looks the way it looked where it came from.
std::optional<Theme> finish(const Draft& draft, std::string* error) {
    Theme theme;
    theme.name = draft.name.empty() ? "imported" : draft.name;
    theme.source = "imported";
    for (std::size_t i = 0; i < 16; ++i) {
        if (!draft.ansi[i]) {
            if (error != nullptr) {
                *error = theme.name + ": missing ANSI colour " + std::to_string(i);
            }
            return std::nullopt;
        }
        theme.ansi[i] = *draft.ansi[i];
    }
    // Foreground and background are the two that cannot be guessed: a palette
    // with no background is a palette, not a theme.
    if (!draft.fg || !draft.bg) {
        if (error != nullptr) {
            *error = theme.name + ": no foreground/background colour";
        }
        return std::nullopt;
    }
    theme.fg = *draft.fg;
    theme.bg = *draft.bg;
    theme.cursor = draft.cursor.value_or(theme.fg);
    theme.cursorText = draft.cursorText.value_or(theme.bg);
    // No selection colour: bright black, which is the one palette entry every
    // scheme picks to sit between the background and the text.
    theme.selectionBg = draft.selection.value_or(theme.ansi[8]);
    // Nothing in any of the three formats carries a search/trigger highlight,
    // so it comes from the palette: bright yellow is what every terminal that
    // has one uses.
    theme.highlightBg = theme.ansi[11];
    theme.chrome = deriveChrome(theme);
    return theme;
}

// "Ansi 7 Color" -> 7; anything else -> -1.
int ansiIndexOf(QStringView key) {
    if (!key.startsWith(QLatin1String("Ansi ")) || !key.endsWith(QLatin1String(" Color"))) {
        return -1;
    }
    const QStringView digits = key.sliced(5, key.size() - 5 - 6);
    bool ok = false;
    const int index = digits.toInt(&ok);
    return ok && index >= 0 && index < 16 ? index : -1;
}

}  // namespace

std::vector<Theme> importITerm2(std::string_view xml, std::string* error) {
    QXmlStreamReader reader(qstr(xml));
    Draft draft;

    // A plist dict is a FLAT alternating key/value stream, not nested pairs, so
    // the reader carries the pending key across elements rather than descending.
    QString pendingOuterKey;
    QString pendingInnerKey;
    int depth = 0;
    double red = 0;
    double green = 0;
    double blue = 0;
    bool inColorDict = false;

    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (reader.name() == QLatin1String("dict")) {
                ++depth;
                if (depth == 2) {
                    inColorDict = true;
                    red = 0;
                    green = 0;
                    blue = 0;
                }
            } else if (reader.name() == QLatin1String("key")) {
                const QString key = reader.readElementText();
                if (inColorDict) {
                    pendingInnerKey = key;
                } else {
                    pendingOuterKey = key;
                }
            } else if (inColorDict && (reader.name() == QLatin1String("real") ||
                                       reader.name() == QLatin1String("integer"))) {
                const double value = reader.readElementText().toDouble();
                if (pendingInnerKey == QLatin1String("Red Component")) {
                    red = value;
                } else if (pendingInnerKey == QLatin1String("Green Component")) {
                    green = value;
                } else if (pendingInnerKey == QLatin1String("Blue Component")) {
                    blue = value;
                }
            }
        } else if (token == QXmlStreamReader::EndElement &&
                   reader.name() == QLatin1String("dict")) {
            if (depth == 2) {
                const Rgb color = pack(red, green, blue);
                const int index = ansiIndexOf(pendingOuterKey);
                if (index >= 0) {
                    draft.ansi[static_cast<std::size_t>(index)] = color;
                } else if (pendingOuterKey == QLatin1String("Background Color")) {
                    draft.bg = color;
                } else if (pendingOuterKey == QLatin1String("Foreground Color")) {
                    draft.fg = color;
                } else if (pendingOuterKey == QLatin1String("Cursor Color")) {
                    draft.cursor = color;
                } else if (pendingOuterKey == QLatin1String("Cursor Text Color")) {
                    draft.cursorText = color;
                } else if (pendingOuterKey == QLatin1String("Selection Color")) {
                    draft.selection = color;
                }
                inColorDict = false;
            }
            --depth;
        }
    }

    if (reader.hasError()) {
        if (error != nullptr) {
            *error = reader.errorString().toStdString();
        }
        return {};
    }

    const std::optional<Theme> theme = finish(draft, error);
    return theme ? std::vector<Theme>{*theme} : std::vector<Theme>{};
}

namespace {

// Windows Terminal's key names, in ANSI index order. "purple" rather than
// "magenta" is Microsoft's spelling and is not negotiable on read.
constexpr std::array<const char*, 16> kWtKeys{
    "black",      "red",          "green",       "yellow",      "blue",        "purple",
    "cyan",       "white",        "brightBlack", "brightRed",   "brightGreen", "brightYellow",
    "brightBlue", "brightPurple", "brightCyan",  "brightWhite",
};

std::optional<Rgb> jsonColor(const QJsonObject& object, const char* key) {
    const QJsonValue value = object.value(QLatin1String(key));
    if (!value.isString()) {
        return std::nullopt;
    }
    const QByteArray utf8 = value.toString().toUtf8();
    return parseColor(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
}

std::optional<Theme> wtScheme(const QJsonObject& object, std::string* error) {
    Draft draft;
    draft.name = object.value(QLatin1String("name")).toString().toStdString();
    for (std::size_t i = 0; i < kWtKeys.size(); ++i) {
        draft.ansi[i] = jsonColor(object, kWtKeys[i]);
    }
    draft.fg = jsonColor(object, "foreground");
    draft.bg = jsonColor(object, "background");
    draft.cursor = jsonColor(object, "cursorColor");
    draft.selection = jsonColor(object, "selectionBackground");
    return finish(draft, error);
}

}  // namespace

std::vector<Theme> importWindowsTerminal(std::string_view json, std::string* error) {
    QJsonParseError parseError{};
    const QByteArray bytes(json.data(), static_cast<qsizetype>(json.size()));
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (doc.isNull()) {
        if (error != nullptr) {
            *error = parseError.errorString().toStdString();
        }
        return {};
    }

    QJsonArray schemes;
    if (doc.isArray()) {
        schemes = doc.array();
    } else if (doc.isObject()) {
        const QJsonObject root = doc.object();
        if (root.value(QLatin1String("schemes")).isArray()) {
            schemes = root.value(QLatin1String("schemes")).toArray();
        } else {
            schemes.append(root);  // a bare scheme
        }
    }

    std::vector<Theme> out;
    std::string lastError;
    for (const QJsonValue& entry : schemes) {
        if (!entry.isObject()) {
            continue;
        }
        // Each scheme gets its own error slot: one malformed entry in a
        // settings.json must not cost the other eleven.
        std::string schemeError;
        if (const std::optional<Theme> theme = wtScheme(entry.toObject(), &schemeError)) {
            out.push_back(*theme);
        } else if (!schemeError.empty()) {
            lastError = schemeError;
        }
    }
    if (out.empty() && error != nullptr) {
        *error = lastError.empty() ? "no colour schemes in this file" : lastError;
    }
    return out;
}

std::vector<Theme> importBase16(std::string_view yaml, std::string* error) {
    // Not a YAML parser. A base16 scheme is sixteen `key: value` lines plus a
    // name, in a file format nobody nests, and pulling in a YAML library to
    // read it would be the dependency this project's rules exist to prevent.
    std::array<std::optional<Rgb>, 16> base{};
    std::string name;

    std::size_t pos = 0;
    while (pos <= yaml.size()) {
        const std::size_t eol = yaml.find('\n', pos);
        std::string_view line = yaml.substr(pos, eol == std::string_view::npos ? eol : eol - pos);
        pos = eol == std::string_view::npos ? yaml.size() + 1 : eol + 1;

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, colon);
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        // Trailing comment, then quotes. Order matters: `base00: "2b303b" # x`.
        if (const std::size_t hash = value.find(" #"); hash != std::string_view::npos) {
            value = value.substr(0, hash);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            value.remove_suffix(1);
        }
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        if (value.empty()) {
            continue;
        }

        if ((key == "name" || key == "scheme") && name.empty()) {
            name = std::string(value);
            continue;
        }
        if (key.size() == 6 && key.substr(0, 4) == "base") {
            const std::string_view digits = key.substr(4);
            std::uint32_t index = 0;
            bool ok = true;
            for (const char ch : digits) {
                const int digit = ch >= '0' && ch <= '9'   ? ch - '0'
                                  : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                  : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                                           : -1;
                if (digit < 0) {
                    ok = false;
                    break;
                }
                index = index * 16 + static_cast<std::uint32_t>(digit);
            }
            if (!ok || index > 15) {
                continue;
            }
            // Bare hex with no '#' is the base16 convention, so it gets one.
            const std::string text =
                value.front() == '#' ? std::string(value) : "#" + std::string(value);
            base[index] = parseColor(text);
        }
    }

    for (std::size_t i = 0; i < base.size(); ++i) {
        if (!base[i]) {
            if (error != nullptr) {
                constexpr std::string_view kHex = "0123456789ABCDEF";
                *error = std::string("base16: missing base0") + kHex[i];
            }
            return {};
        }
    }

    // base16-shell's mapping. The bright half repeating the normal half is not
    // a bug: base16 has sixteen slots and eight of them are greys, so a scheme
    // that spread them across bright ANSI would turn `ls` output into mud.
    Draft draft;
    draft.name = name.empty() ? "base16" : name;
    draft.ansi = {base[0x0], base[0x8], base[0xB], base[0xA], base[0xD], base[0xE],
                  base[0xC], base[0x5], base[0x3], base[0x8], base[0xB], base[0xA],
                  base[0xD], base[0xE], base[0xC], base[0x7]};
    draft.bg = base[0x0];
    draft.fg = base[0x5];
    draft.cursor = base[0x5];
    draft.cursorText = base[0x0];
    draft.selection = base[0x2];

    const std::optional<Theme> theme = finish(draft, error);
    return theme ? std::vector<Theme>{*theme} : std::vector<Theme>{};
}

std::vector<Theme> importAny(std::string_view fileName, std::string_view bytes,
                             std::string* error) {
    const auto endsWith = [fileName](std::string_view suffix) {
        return fileName.size() >= suffix.size() &&
               fileName.substr(fileName.size() - suffix.size()) == suffix;
    };
    if (endsWith(".itermcolors") || endsWith(".plist")) {
        return importITerm2(bytes, error);
    }
    if (endsWith(".json")) {
        return importWindowsTerminal(bytes, error);
    }
    if (endsWith(".yaml") || endsWith(".yml")) {
        return importBase16(bytes, error);
    }
    if (endsWith(".toml")) {
        std::optional<Theme> theme = parseToml(bytes, error);
        if (!theme) {
            return {};
        }
        theme->source = "imported";
        return {*theme};
    }

    // No usable extension: sniff the first non-space byte. Cheap and total —
    // the three formats disagree on it, and the fallback is base16, which is
    // the only one of them with no distinctive opening character.
    std::size_t start = 0;
    while (start < bytes.size() && static_cast<unsigned char>(bytes[start]) <= ' ') {
        ++start;
    }
    const char first = start < bytes.size() ? bytes[start] : '\0';
    if (first == '<') {
        return importITerm2(bytes, error);
    }
    if (first == '{' || first == '[') {
        return importWindowsTerminal(bytes, error);
    }
    return importBase16(bytes, error);
}

std::string themeFileName(std::string_view name) {
    // 64 is far past any real theme name and keeps the result inside every
    // path limit once the themes directory is prepended.
    constexpr std::size_t kMaxStem = 64;
    std::string out;
    out.reserve(std::min(name.size(), kMaxStem));
    bool lastWasDash = true;  // leading dashes are dropped by starting true
    for (const char ch : name) {
        if (out.size() >= kMaxStem) {
            break;
        }
        const bool alnum =
            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        if (alnum) {
            out += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
            lastWasDash = false;
        } else if (!lastWasDash) {
            out += '-';
            lastWasDash = true;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    // Never empty: a theme called "***" would otherwise be written to ".toml",
    // which on Windows is a hidden file with no name.
    return (out.empty() ? std::string("theme") : out) + ".toml";
}

}  // namespace krait::app::theme

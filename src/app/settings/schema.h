#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace krait::app::settings {

// A setting's value. Three types is not a simplification waiting to grow: a
// colour is a string ("#1e1e2e") because the theme system parses it, and an
// enum is a string because a TOML file a human edits should say "narrow", not
// 0. Adding a type means adding a TOML mapping AND a UI editor for it, so the
// bar for a fourth is deliberately high.
using Value = std::variant<bool, std::int64_t, std::string>;

// The same three types, but with a string_view — so the schema table below is a
// constexpr literal. A std::string in a static table means its construction can
// throw before main(), where nothing can catch it (bugprone-throwing-static-
// initialization), and the fix is to hold the bytes rather than own them.
using Default = std::variant<bool, std::int64_t, std::string_view>;

enum class Type : std::uint8_t { Bool, Int, String };

// One setting, declared once (rules/ui.md: "Schema (id, type, default, doc key,
// search keywords EN+TH, migration) → TOML IO → QML settings UI → command
// palette — all generated from one declaration. Adding a bare QSettings/TOML
// key by hand is a defect").
struct Def {
    // Dotted, and the dots ARE the TOML table path: "font.size" is
    // [font] size. One id, one place in the file, and no separate mapping table
    // that can drift out of sync with it.
    std::string_view id;
    Type type;
    Default fallback;
    // Int only, and inclusive. min == max means unbounded. A setting with a
    // range is validated on load as well as on set, so a hand-edited file
    // cannot put the app into a state its own UI could never produce.
    std::int64_t min = 0;
    std::int64_t max = 0;
    // Allowed values for a string that is really an enum; empty means free
    // text. Space separated.
    std::string_view choices;
    std::string_view doc;
    // Search keywords for the settings UI and the command palette. Thai ships
    // as a first-class locale, so a Thai speaker has to be able to FIND a
    // setting in Thai — a translated label with English-only search is half a
    // locale.
    std::string_view searchEn;
    std::string_view searchTh;
};

// The whole registry, in declaration order.
std::span<const Def> definitions();

// A def's default as a runtime Value. One place converts string_view to string,
// so nothing else has to know the schema stores views.
Value defaultValue(const Def& def);

// Null when no such setting exists. Callers must not invent ids: an unknown id
// is a bug in the caller, not a runtime condition to paper over.
const Def* find(std::string_view id);

// Whether `value` is a legal setting for `def` — right type, in range, and one
// of the choices when the def has any.
bool validate(const Def& def, const Value& value);

// Whether `def` matches a settings-search query.
//
// Matches the id, the doc text, and BOTH keyword lists — a Thai speaker has to
// be able to find a setting by typing Thai, and a translated label with
// English-only search is half a locale. Substring rather than fuzzy: a settings
// search is a filter over a short list where a wrong-but-plausible match costs
// more than a missed one, which is the opposite of the palette's problem.
//
// An empty query matches everything.
bool matchesSearch(const Def& def, std::string_view query);

// The current schema version, written into every file the app saves. Bumped
// when a migration is added, and never otherwise.
inline constexpr int kSchemaVersion = 1;

}  // namespace krait::app::settings

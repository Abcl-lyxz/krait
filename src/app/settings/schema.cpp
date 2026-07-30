#include "schema.h"

#include <algorithm>
#include <array>
#include <type_traits>

namespace krait::app::settings {
namespace {

// The registry. Every setting Krait has lives here and nowhere else.
//
// Each entry says what subsystem it drives, because a setting with nothing
// behind it is worse than a missing one: the user changes it, nothing happens,
// and they stop trusting the whole settings page.
constexpr std::array<Def, 8> kDefs{{
    {
        .id = "font.family",
        .type = Type::String,
        .fallback = std::string_view{},  // empty: pick the first installed candidate
        .choices = "",
        .doc = "settings.font.family",
        .searchEn = "font family typeface monospace",
        .searchTh = "ฟอนต์ แบบอักษร ตัวอักษร",
    },
    {
        .id = "font.size",
        .type = Type::Int,
        .fallback = std::int64_t{20},
        // 6 is the smallest size FreeType gives usable metrics for; 200 is far
        // past anything readable and exists only so a typo cannot ask for an
        // atlas the GPU refuses to allocate.
        .min = 6,
        .max = 200,
        .choices = "",
        .doc = "settings.font.size",
        .searchEn = "font size points zoom",
        .searchTh = "ขนาด ฟอนต์ ตัวอักษร",
    },
    {
        .id = "font.ligatures",
        .type = Type::Bool,
        .fallback = false,
        .choices = "",
        .doc = "settings.font.ligatures",
        .searchEn = "ligatures programming arrows",
        .searchTh = "ลิเกเจอร์ อักษรควบ",
    },
    {
        .id = "theme.name",
        .type = Type::String,
        .fallback = std::string_view{"default-dark"},
        .choices = "",
        .doc = "settings.theme.name",
        .searchEn = "theme colours colors palette dark light",
        .searchTh = "ธีม สี ชุดสี มืด สว่าง",
    },
    {
        .id = "unicode.eastAsianAmbiguous",
        .type = Type::String,
        .fallback = std::string_view{"narrow"},
        // The landmine from CLAUDE.md: there is no right default, only a
        // per-session setting. A user with a CJK locale needs "wide", or every
        // box-drawing character in their tooling lands half a cell off.
        .choices = "narrow wide",
        .doc = "settings.unicode.eastAsianAmbiguous",
        .searchEn = "east asian ambiguous width cjk box drawing",
        .searchTh = "ความกว้าง เอเชียตะวันออก กำกวม",
    },
    {
        .id = "scrollback.lines",
        .type = Type::Int,
        .fallback = std::int64_t{10000},
        // 0 disables scrollback entirely, which some people want on a shared
        // machine. The cap is per tab and bounds memory (T21).
        .min = 0,
        .max = 1000000,
        .choices = "",
        .doc = "settings.scrollback.lines",
        .searchEn = "scrollback history buffer lines memory",
        .searchTh = "ประวัติ เลื่อนขึ้น บรรทัด หน่วยความจำ",
    },
    {
        .id = "gpu.adapter",
        .type = Type::String,
        .fallback = std::string_view{"auto"},
        // The same three values as KRAIT_GPU (T26). "auto" picks software
        // inside an RDP session and hardware otherwise.
        .choices = "auto hardware warp",
        .doc = "settings.gpu.adapter",
        .searchEn = "gpu adapter graphics warp software rdp remote",
        .searchTh = "การ์ดจอ กราฟิก ซอฟต์แวร์ รีโมท",
    },
    {
        .id = "ui.language",
        .type = Type::String,
        .fallback = std::string_view{"system"},
        .choices = "system en th",
        .doc = "settings.ui.language",
        .searchEn = "language locale english thai translation",
        .searchTh = "ภาษา ไทย อังกฤษ การแปล",
    },
}};

}  // namespace

std::span<const Def> definitions() {
    return kDefs;
}

Value defaultValue(const Def& def) {
    return std::visit(
        [](const auto& held) -> Value {
            if constexpr (std::is_same_v<std::decay_t<decltype(held)>, std::string_view>) {
                return Value{std::string(held)};
            } else {
                return Value{held};
            }
        },
        def.fallback);
}

const Def* find(std::string_view id) {
    const auto it = std::ranges::find(kDefs, id, &Def::id);
    return it == kDefs.end() ? nullptr : &*it;
}

bool validate(const Def& def, const Value& value) {
    switch (def.type) {
    case Type::Bool:
        return std::holds_alternative<bool>(value);
    case Type::Int: {
        const auto* number = std::get_if<std::int64_t>(&value);
        if (number == nullptr) {
            return false;
        }
        if (def.min == def.max) {
            return true;  // unbounded
        }
        return *number >= def.min && *number <= def.max;
    }
    case Type::String: {
        const auto* text = std::get_if<std::string>(&value);
        if (text == nullptr) {
            return false;
        }
        if (def.choices.empty()) {
            return true;
        }
        // Space-separated WHOLE-word match. A substring test would accept
        // "warp2" for choices "auto hardware warp".
        std::string_view rest = def.choices;
        while (!rest.empty()) {
            const auto space = rest.find(' ');
            const std::string_view choice = rest.substr(0, space);
            if (choice == *text) {
                return true;
            }
            if (space == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(space + 1);
        }
        return false;
    }
    }
    return false;
}

}  // namespace krait::app::settings

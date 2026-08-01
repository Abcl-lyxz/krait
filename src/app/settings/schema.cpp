#include "schema.h"

#include <algorithm>
#include <array>
#include <string>
#include <type_traits>

namespace krait::app::settings {
namespace {

// The registry. Every setting Krait has lives here and nowhere else.
//
// Each entry says what subsystem it drives, because a setting with nothing
// behind it is worse than a missing one: the user changes it, nothing happens,
// and they stop trusting the whole settings page.
constexpr std::array<Def, 14> kDefs{{
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
        .id = "notify.longCommand",
        .type = Type::Bool,
        // On, because the failure mode of "off" is silent: a build that
        // finished ten minutes ago while you were reading something else. It
        // only ever fires when the window is NOT focused, so it cannot
        // interrupt someone who is already watching the command run.
        .fallback = true,
        .choices = "",
        .doc = "settings.notify.longCommand",
        .searchEn = "notify notification alert long command finished done shell integration",
        .searchTh = "แจ้งเตือน การแจ้งเตือน คำสั่ง เสร็จสิ้น ทำงานนาน",
    },
    {
        .id = "notify.longCommandSeconds",
        .type = Type::Int,
        // 30 s is past every command a person waits at the keyboard for and
        // short enough to catch a test run. Below it the notification is noise;
        // the ceiling is an hour, past which nobody is waiting on a banner.
        .fallback = std::int64_t{30},
        .min = 1,
        .max = 3600,
        .choices = "",
        .doc = "settings.notify.longCommandSeconds",
        .searchEn = "notify threshold seconds duration long command slow",
        .searchTh = "แจ้งเตือน เกณฑ์ วินาที ระยะเวลา คำสั่ง ช้า",
    },
    {
        .id = "notify.taskbarProgress",
        .type = Type::Bool,
        // On, because the feature is genuinely useful and a default that is not
        // useful is a default nobody keeps: OSC 9;4 is what puts a real
        // progress bar on the taskbar button for a long build or a big copy.
        //
        // It exists as a switch at all because the sender is REMOTE. Every
        // other surface a remote host can reach is inside the tab it owns;
        // this one paints a piece of the user's desktop, and there was no way
        // to decline it (T67 review). Off means Krait parses the sequence and
        // reports nothing, which is also the honest reading — the core still
        // consumes the bytes and still never replies to them.
        .fallback = true,
        .choices = "",
        .doc = "settings.notify.taskbarProgress",
        .searchEn = "taskbar progress bar osc 9 4 remote button percent build download",
        .searchTh = "แถบงาน ความคืบหน้า แถบสถานะ เปอร์เซ็นต์ ทางไกล ดาวน์โหลด",
    },
    {
        .id = "triggers.enabled",
        .type = Type::Bool,
        // On, because a profile with no triggers costs nothing: the engine
        // compiles nothing and feed() returns on the first line. The switch
        // exists because the WORK is driven by remote bytes — a master off is
        // what someone reaches for when a rule they wrote turns out to be
        // expensive on a chatty host, and editing every profile to find out
        // which one is not that.
        .fallback = true,
        .choices = "",
        .doc = "settings.triggers.enabled",
        .searchEn = "trigger triggers regex match highlight notify watch output alert",
        .searchTh = "ทริกเกอร์ ตัวกระตุ้น รูปแบบ ค้นหา เน้นสี แจ้งเตือน เฝ้าดู",
    },
    {
        .id = "triggers.allowSend",
        .type = Type::Bool,
        // OFF, and this is the one default in the file that is not about
        // taste. A trigger that sends text back, fired by output the REMOTE
        // side chose, is a remote-controlled input primitive the user pointed
        // at themselves — the same shape as an answerback, which rules/net.md
        // rate-limits for exactly this reason. Everything else about triggers
        // only ever costs a highlight or a banner; this one runs commands. It
        // is opt-in per install, on top of the per-trigger rate limit and the
        // size cap, so a profile file arriving from somewhere else cannot
        // enable it on its own.
        .fallback = false,
        .choices = "",
        .doc = "settings.triggers.allowSend",
        .searchEn = "trigger send auto respond reply automation type input answer",
        .searchTh = "ทริกเกอร์ ส่ง ตอบกลับ อัตโนมัติ พิมพ์ คำสั่ง",
    },
    {
        .id = "triggers.logFile",
        .type = Type::String,
        // Empty means <config dir>/logs/triggers.log, beside the session logs
        // — one file for every session, because the question a trigger log
        // answers ("when did that last happen") is usually asked across
        // sessions rather than inside one.
        .fallback = std::string_view{},
        .choices = "",
        .doc = "settings.triggers.logFile",
        .searchEn = "trigger log file path record match history",
        .searchTh = "ทริกเกอร์ บันทึก ไฟล์ ที่อยู่ไฟล์ ประวัติ",
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

bool matchesSearch(const Def& def, std::string_view query) {
    if (query.empty()) {
        return true;
    }
    // ASCII case folding only. Thai has no case to fold, so folding it is a
    // no-op rather than a bug; what matters is that a non-ASCII byte never
    // changes under it.
    const auto fold = [](char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    };
    std::string needle;
    needle.reserve(query.size());
    for (const char ch : query) {
        needle += fold(ch);
    }

    for (const std::string_view field : {def.id, def.doc, def.searchEn, def.searchTh}) {
        std::string haystack;
        haystack.reserve(field.size());
        for (const char ch : field) {
            haystack += fold(ch);
        }
        if (haystack.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
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

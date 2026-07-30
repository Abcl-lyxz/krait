#include "palette.h"

#include "actions.h"
#include "fuzzy.h"

#include <algorithm>
#include <initializer_list>

namespace krait::app::session {

namespace {

// A session outranks an action at the same score. With nothing typed the
// palette is a connection picker; with something typed, the thing someone
// configured by hand is more likely what they meant than a built-in command
// that happens to share letters.
constexpr int kSessionBias = 6;

// Best score across several haystacks. A query should find an action by its
// keywords ("quit" -> Close session) without those keywords having to appear in
// the label the user reads.
int bestScore(std::string_view query, std::initializer_list<std::string_view> haystacks) {
    int best = -1;
    for (const std::string_view text : haystacks) {
        if (text.empty()) {
            continue;
        }
        best = std::max(best, fuzzyScore(query, text));
    }
    return best;
}

}  // namespace

std::vector<PaletteEntry> rankPalette(std::string_view query, const ProfileStore& sessions) {
    std::vector<PaletteEntry> entries;
    entries.reserve(sessions.profiles().size() + allActions().size());

    for (const Profile& profile : sessions.profiles()) {
        // Host and user are matched as well as the name: people remember the
        // machine, not what they called the profile three months ago.
        const int score = bestScore(
            query, {profile.name, profile.host, profile.user, profile.folder, profile.id});
        if (score < 0) {
            continue;
        }
        entries.push_back({.kind = PaletteEntry::Kind::Session,
                           .id = profile.id,
                           .label = profile.name.empty() ? profile.id : profile.name,
                           .detail = profile.folder,
                           .score = score + kSessionBias});
    }

    for (const Action& action : allActions()) {
        const int score = bestScore(query, {action.label, action.keywords, action.id});
        if (score < 0) {
            continue;
        }
        entries.push_back({.kind = PaletteEntry::Kind::Action,
                           .id = std::string(action.id),
                           .label = std::string(action.label),
                           .detail = std::string(action.shortcut),
                           .score = score});
    }

    // Stable within a score so the order does not shuffle as someone types — a
    // list that reorders under an unchanged prefix is a list you cannot aim at.
    std::stable_sort(entries.begin(), entries.end(),
                     [](const PaletteEntry& a, const PaletteEntry& b) {
                         if (a.score != b.score) {
                             return a.score > b.score;
                         }
                         return a.label < b.label;
                     });
    return entries;
}

std::vector<TreeRow> buildTree(const ProfileStore& sessions) {
    // Every folder that exists, including the intermediate ones no profile sits
    // in directly — ProfileStore::folders() already fills those in.
    const std::vector<std::string> folders = sessions.folders();

    std::vector<TreeRow> rows;
    const auto appendSessionsIn = [&](const std::string& folder, int depth) {
        std::vector<const Profile*> here;
        for (const Profile& profile : sessions.profiles()) {
            if (profile.folder == folder) {
                here.push_back(&profile);
            }
        }
        std::sort(here.begin(), here.end(), [](const Profile* a, const Profile* b) {
            return a->name == b->name ? a->id < b->id : a->name < b->name;
        });
        for (const Profile* profile : here) {
            rows.push_back({.isFolder = false,
                            .depth = depth,
                            .label = profile->name.empty() ? profile->id : profile->name,
                            .id = profile->id});
        }
    };

    // Root-level sessions first, then the folder tree. folders() is sorted, and
    // a sorted list of slash-separated paths is already depth-first: a parent
    // always sorts immediately before its children.
    appendSessionsIn("", 0);
    for (const std::string& folder : folders) {
        const std::size_t slash = folder.rfind('/');
        const int depth = static_cast<int>(std::count(folder.begin(), folder.end(), '/'));
        rows.push_back({.isFolder = true,
                        .depth = depth,
                        .label = slash == std::string::npos ? folder : folder.substr(slash + 1),
                        .id = folder});
        appendSessionsIn(folder, depth + 1);
    }
    return rows;
}

}  // namespace krait::app::session

#pragma once

#include "profile.h"

#include <string>
#include <string_view>
#include <vector>

namespace krait::app::session {

// One row of the command palette. Actions and saved sessions share the list on
// purpose: "the thing I want to do" and "the machine I want to be on" are the
// same question to the person typing, and two separate pickers means learning
// which one holds what.
struct PaletteEntry {
    enum class Kind { Action, Session };

    Kind kind = Kind::Action;
    // Action id, or profile id.
    std::string id;
    std::string label;
    // Shortcut for an action; folder path for a session. Shown dimmed on the
    // right, which is where the eye goes for "where is this".
    std::string detail;
    int score = 0;
};

// Ranks actions and sessions against `query`, best first, dropping non-matches.
//
// An empty query returns everything, sessions first — with nothing typed, the
// useful default is "where do you want to connect", not an alphabetical list of
// commands.
std::vector<PaletteEntry> rankPalette(std::string_view query, const ProfileStore& sessions);

// A row of the session tree. The tree is DERIVED from the folder strings on
// each profile rather than stored, so there is no second structure to keep
// consistent when a profile moves.
struct TreeRow {
    bool isFolder = false;
    int depth = 0;
    // Display name — the last path segment for a folder, the profile name for a
    // session.
    std::string label;
    // Full folder path, or the profile id.
    std::string id;
};

// Depth-first, folders before sessions at each level, both alphabetical.
std::vector<TreeRow> buildTree(const ProfileStore& sessions);

}  // namespace krait::app::session

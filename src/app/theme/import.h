#pragma once

#include "theme/theme.h"

#include <string>
#include <string_view>
#include <vector>

namespace krait::app::theme {

// Theme importers (M5 acceptance: "theme import round-trip").
//
// All three formats carry a terminal palette and nothing else — no chrome, no
// UI tokens — so every one of these returns a Theme whose chrome came from
// deriveChrome(). That is the reason Chrome is short enough to derive: an
// importer that could only colour the grid would leave the window it sits in
// still wearing the previous theme, which looks like a broken import.
//
// Errors are values. `error` is set on failure and untouched on success; a
// vector-returning importer reports partial success by returning what it could
// read AND setting error, because a Windows Terminal settings.json with one bad
// scheme out of twelve should import eleven.

// iTerm2 `.itermcolors`: an XML property list of "Ansi N Color" dicts whose
// components are 0..1 reals in the P3 or sRGB space. Colour-space tags are
// IGNORED — converting P3 to sRGB would change every value in a file the user
// picked by eye, and no other terminal that imports these does it either.
std::vector<Theme> importITerm2(std::string_view xml, std::string* error);

// Windows Terminal colour schemes. Accepts a bare scheme object, a top-level
// array of them, or a whole settings.json (the "schemes" key) — all three are
// what people actually have on disk, and telling them to extract the right
// fragment first is a worse product than reading three shapes.
std::vector<Theme> importWindowsTerminal(std::string_view json, std::string* error);

// base16 / base24 YAML, both the flat `base00:` form and the tinted-theming
// `palette:` form. The base00-base0F to ANSI mapping is base16-shell's, which
// is the one every base16 terminal theme in the wild was authored against.
std::vector<Theme> importBase16(std::string_view yaml, std::string* error);

// Picks an importer from the file name, falling back to sniffing the content.
// `fileName` may be empty.
std::vector<Theme> importAny(std::string_view fileName, std::string_view bytes, std::string* error);

// A file name safe to write a theme to: lowercased, non-alphanumerics folded to
// '-', bounded. Exported because the gallery shows it before saving — "this
// will be saved as X" is the only way a user finds the file again.
std::string themeFileName(std::string_view name);

}  // namespace krait::app::theme

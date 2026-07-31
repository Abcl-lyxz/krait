#pragma once

#include "profile.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace krait::app::session {

// One PuTTY session as it comes out of the registry: the key name plus its
// values, all as text. The registry read and the MAPPING are separate so the
// mapping — which is where every interesting decision lives — can be tested
// against invented key/value sets rather than against whatever happens to be
// installed on the machine running the tests.
using PuttyValues = std::vector<std::pair<std::string, std::string>>;

// PuTTY escapes session names before using them as registry keys (mungestr in
// its winstore.c): space, backslash, asterisk, question mark, percent, control
// characters, anything above '~', and a LEADING dot all become %XX. Reading
// them back only needs the %XX case undone.
std::string decodePuttyName(std::string_view munged);

// Maps one session. Returns nothing when the session names a protocol we do not
// have yet — telnet, raw and serial are M3 — because importing those as SSH
// would produce a profile that fails at connect time with a confusing error,
// and silently dropping them would leave the user counting.
std::optional<Profile> profileFromPutty(std::string_view mungedName, const PuttyValues& values);

struct PuttyImport {
    std::vector<Profile> profiles;
    // Sessions we understood but cannot represent yet, by name, so the UI can
    // say WHICH ones were left behind rather than only how many.
    std::vector<std::string> skipped;
    // Set when the registry could not be read at all (no PuTTY installed, or no
    // permission). Distinct from "read it and found nothing".
    std::string error;
};

// Reads HKEY_CURRENT_USER\Software\SimonTatham\PuTTY\Sessions. The only part
// that touches Windows.
PuttyImport importFromPuttyRegistry();

}  // namespace krait::app::session

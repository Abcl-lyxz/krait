#include "putty_import.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <charconv>
#include <cstring>
#include <utility>

namespace krait::app::session {

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

const std::string* valueOf(const PuttyValues& values, std::string_view key) {
    for (const auto& [name, value] : values) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

// PuTTY has no folders, so people fake them in the session name — "prod/web-1"
// and "prod - web-1" are both common. Only the slash is treated as structure:
// splitting on " - " would turn a session honestly named "db - replica" into a
// folder, and a wrong folder is harder to notice than a flat list.
void splitFolder(const std::string& fullName, Profile* profile) {
    const std::size_t slash = fullName.rfind('/');
    if (slash == std::string::npos) {
        profile->name = fullName;
        return;
    }
    profile->folder = fullName.substr(0, slash);
    profile->name = fullName.substr(slash + 1);
    if (profile->name.empty()) {
        profile->name = fullName;
        profile->folder.clear();
    }
}

}  // namespace

std::string decodePuttyName(std::string_view munged) {
    std::string out;
    out.reserve(munged.size());
    for (std::size_t i = 0; i < munged.size(); ++i) {
        if (munged[i] != '%' || i + 2 >= munged.size()) {
            out += munged[i];
            continue;
        }
        const int high = hexDigit(munged[i + 1]);
        const int low = hexDigit(munged[i + 2]);
        if (high < 0 || low < 0) {
            // Not an escape after all. A literal '%' cannot appear unescaped in
            // a name PuTTY wrote, but this is data from outside the app, and
            // dropping bytes we do not understand loses information silently.
            out += munged[i];
            continue;
        }
        out += static_cast<char>(high * 16 + low);
        i += 2;
    }
    return out;
}

std::optional<Profile> profileFromPutty(std::string_view mungedName, const PuttyValues& values) {
    // Default is ssh: PuTTY writes Protocol for every session it saves, but a
    // hand-made or very old key may not have it, and ssh is both the common
    // case and the safe one to guess.
    const std::string* protocol = valueOf(values, "Protocol");
    Profile profile;
    if (protocol == nullptr || *protocol == "ssh") {
        profile.backend = BackendKind::Ssh;
    } else if (*protocol == "serial") {
        // T56. PuTTY keeps the line in SerialLine and the speed in SerialSpeed,
        // and Krait keeps the line in `host` for the same reason PuTTY does.
        profile.backend = BackendKind::Serial;
    } else if (*protocol == "telnet") {
        // T54. Importing these as SSH would have produced a profile that fails
        // at connect time with an error about the wrong protocol, which is why
        // they were skipped rather than coerced.
        profile.backend = BackendKind::Telnet;
    } else {
        // raw is the one left: PuTTY stores no port for it in a form worth
        // trusting, so importing one would produce a profile that cannot
        // connect. Skipped, and the caller names it, so the count of imported
        // sessions is never a count the user has to reconcile by hand.
        return std::nullopt;
    }
    profile.markExplicit("backend");

    const std::string fullName = decodePuttyName(mungedName);
    splitFolder(fullName, &profile);
    profile.markExplicit("name");
    if (!profile.folder.empty()) {
        profile.markExplicit("folder");
    }
    profile.id = slugify(fullName);

    if (const std::string* line = valueOf(values, "SerialLine");
        profile.backend == BackendKind::Serial && line != nullptr && !line->empty()) {
        profile.host = *line;
        profile.markExplicit("host");
    }
    if (const std::string* speed = valueOf(values, "SerialSpeed"); speed != nullptr) {
        std::int64_t baud = 0;
        std::from_chars(speed->data(), speed->data() + speed->size(), baud);
        if (baud > 0) {
            profile.baud = baud;
            profile.markExplicit("baud");
        }
    }
    if (const std::string* host = valueOf(values, "HostName");
        profile.backend != BackendKind::Serial && host != nullptr && !host->empty()) {
        profile.host = *host;
        profile.markExplicit("host");
    }
    if (const std::string* user = valueOf(values, "UserName"); user != nullptr && !user->empty()) {
        profile.user = *user;
        profile.markExplicit("user");
    }
    if (const std::string* port = valueOf(values, "PortNumber"); port != nullptr) {
        std::int64_t parsed = 0;
        const char* first = port->data();
        std::from_chars(first, first + port->size(), parsed);
        if (parsed > 0 && parsed <= 65535) {
            profile.port = parsed;
            profile.markExplicit("port");
        }
    }
    if (const std::string* key = valueOf(values, "PublicKeyFile");
        key != nullptr && !key->empty()) {
        // A .ppk is PuTTY's own format and libssh cannot read it. The path is
        // kept — throwing it away would lose the one piece of information that
        // says which key this session used — but the auth method stays Auto, so
        // the agent gets its turn first and an unreadable path does not become a
        // hard failure. Converting .ppk is a job for the settings UI, with the
        // user watching.
        profile.keyPath = *key;
        profile.markExplicit("key_path");
    }
    return profile;
}

PuttyImport importFromPuttyRegistry() {
    PuttyImport result;

    HKEY sessions = nullptr;
    const LSTATUS opened = RegOpenKeyExA(
        HKEY_CURRENT_USER, R"(Software\SimonTatham\PuTTY\Sessions)", 0, KEY_READ, &sessions);
    if (opened != ERROR_SUCCESS) {
        // Not a failure the user needs to see as one: it usually just means
        // PuTTY was never installed.
        result.error = "no PuTTY sessions found";
        return result;
    }

    for (DWORD index = 0;; ++index) {
        char keyName[256] = {};
        DWORD keyLen = sizeof(keyName);
        const LSTATUS enumerated =
            RegEnumKeyExA(sessions, index, keyName, &keyLen, nullptr, nullptr, nullptr, nullptr);
        if (enumerated == ERROR_MORE_DATA) {
            // A session name longer than the buffer. Skipping just that one and
            // carrying on matters: breaking here would silently drop every
            // LATER session too, and the summary would report the truncated
            // count as a success.
            continue;
        }
        if (enumerated != ERROR_SUCCESS) {
            break;
        }
        HKEY session = nullptr;
        if (RegOpenKeyExA(sessions, keyName, 0, KEY_READ, &session) != ERROR_SUCCESS) {
            continue;
        }

        PuttyValues values;
        for (DWORD valueIndex = 0;; ++valueIndex) {
            char valueName[256] = {};
            DWORD valueLen = sizeof(valueName);
            BYTE data[1024] = {};
            DWORD dataLen = sizeof(data);
            DWORD type = 0;
            if (RegEnumValueA(session, valueIndex, valueName, &valueLen, nullptr, &type, data,
                              &dataLen) != ERROR_SUCCESS) {
                break;
            }
            if (type == REG_SZ || type == REG_EXPAND_SZ) {
                // dataLen counts the NUL that RegEnumValue includes; keeping it
                // would put a NUL inside every std::string built here.
                const DWORD length =
                    dataLen > 0 && data[dataLen - 1] == '\0' ? dataLen - 1 : dataLen;
                values.emplace_back(valueName,
                                    std::string(reinterpret_cast<const char*>(data), length));
            } else if (type == REG_DWORD && dataLen >= sizeof(DWORD)) {
                DWORD number = 0;
                std::memcpy(&number, data, sizeof(number));
                values.emplace_back(valueName, std::to_string(number));
            }
        }
        RegCloseKey(session);

        if (std::optional<Profile> profile = profileFromPutty(keyName, values)) {
            result.profiles.push_back(std::move(*profile));
        } else {
            result.skipped.push_back(decodePuttyName(keyName));
        }
    }
    RegCloseKey(sessions);
    result.error.clear();
    return result;
}

}  // namespace krait::app::session

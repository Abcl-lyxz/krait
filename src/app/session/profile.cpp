#include "profile.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace krait::app::session {

namespace {

constexpr int kStoreVersion = 1;

// The key names, in one place. They are the TOML spelling AND what bulkSet
// takes, so a rename cannot drift between the file format and the editor.
constexpr const char* kName = "name";
constexpr const char* kFolder = "folder";
constexpr const char* kTags = "tags";
constexpr const char* kBackend = "backend";
constexpr const char* kHost = "host";
constexpr const char* kPort = "port";
constexpr const char* kUser = "user";
constexpr const char* kAuth = "auth";
constexpr const char* kKeyPath = "key_path";
constexpr const char* kAccent = "accent";
constexpr const char* kCommand = "command";
constexpr const char* kBaud = "baud";
constexpr const char* kProxyJump = "proxy_jump";

std::string readText(const std::string& path, bool* exists) {
    std::ifstream file(path, std::ios::binary);
    *exists = file.good();
    if (!*exists) {
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string tagsToText(const std::vector<std::string>& tags) {
    std::string joined;
    for (const std::string& tag : tags) {
        if (!joined.empty()) {
            joined += ',';
        }
        joined += tag;
    }
    return joined;
}

std::vector<std::string> tagsFromText(std::string_view text) {
    std::vector<std::string> tags;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        std::string_view tag = text.substr(start, end - start);
        while (!tag.empty() && tag.front() == ' ') {
            tag.remove_prefix(1);
        }
        while (!tag.empty() && tag.back() == ' ') {
            tag.remove_suffix(1);
        }
        if (!tag.empty()) {
            tags.emplace_back(tag);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return tags;
}

// One place that turns `key = text` into a field, shared by the inherited
// tables, the importer and bulkSet. Returns false for an unknown key so a
// caller can refuse rather than silently drop the edit.
bool applyField(Profile& profile, std::string_view key, std::string_view value) {
    if (key == kName) {
        profile.name = value;
    } else if (key == kFolder) {
        profile.folder = value;
    } else if (key == kTags) {
        profile.tags = tagsFromText(value);
    } else if (key == kBackend) {
        profile.backend = parseBackend(value);
    } else if (key == kHost) {
        profile.host = value;
    } else if (key == kPort) {
        std::int64_t port = 0;
        const char* first = value.data();
        const char* last = first + value.size();
        std::from_chars(first, last, port);
        // A port outside the wire range is a typo, not a configuration. Keep
        // the previous value rather than trying to connect to 0.
        if (port > 0 && port <= 65535) {
            profile.port = port;
        }
    } else if (key == kUser) {
        profile.user = value;
    } else if (key == kAuth) {
        profile.auth = parseAuth(value);
    } else if (key == kKeyPath) {
        profile.keyPath = value;
    } else if (key == kProxyJump) {
        profile.proxyJump = value;
    } else if (key == kAccent) {
        profile.accent = value;
    } else if (key == kCommand) {
        profile.command = value;
    } else if (key == kBaud) {
        std::int64_t baud = 0;
        std::from_chars(value.data(), value.data() + value.size(), baud);
        // A nonsense baud rate is a typo, not a configuration: keep the
        // previous value rather than asking the driver for 0 bits a second.
        if (baud > 0) {
            profile.baud = baud;
        }
    } else {
        return false;
    }
    return true;
}

std::string fieldText(const Profile& profile, std::string_view key) {
    if (key == kName) {
        return profile.name;
    }
    if (key == kFolder) {
        return profile.folder;
    }
    if (key == kTags) {
        return tagsToText(profile.tags);
    }
    if (key == kBackend) {
        return backendName(profile.backend);
    }
    if (key == kHost) {
        return profile.host;
    }
    if (key == kPort) {
        return std::to_string(profile.port);
    }
    if (key == kUser) {
        return profile.user;
    }
    if (key == kAuth) {
        return authName(profile.auth);
    }
    if (key == kKeyPath) {
        return profile.keyPath;
    }
    if (key == kProxyJump) {
        return profile.proxyJump;
    }
    if (key == kAccent) {
        return profile.accent;
    }
    if (key == kCommand) {
        return profile.command;
    }
    if (key == kBaud) {
        return std::to_string(profile.baud);
    }
    return {};
}

// toml++ hands back `true` for `ligatures = 3` (STATE.md), so every read here
// checks the node type first. A value of the wrong shape is ignored rather
// than coerced into something the user never wrote.
bool nodeToText(const toml::node& node, std::string* out) {
    if (node.is_string()) {
        *out = node.ref<std::string>();
        return true;
    }
    if (node.is_integer()) {
        *out = std::to_string(node.ref<std::int64_t>());
        return true;
    }
    if (node.is_boolean()) {
        *out = node.ref<bool>() ? "true" : "false";
        return true;
    }
    return false;
}

}  // namespace

std::string backendName(BackendKind kind) {
    switch (kind) {
    case BackendKind::Ssh:
        return "ssh";
    case BackendKind::Telnet:
        return "telnet";
    case BackendKind::Raw:
        return "raw";
    case BackendKind::Serial:
        return "serial";
    case BackendKind::Conpty:
        return "conpty";
    }
    // No default label on purpose: adding a backend has to break this switch at
    // COMPILE time rather than silently write "conpty" into someone's file.
    return "conpty";
}

std::string authName(SshAuth auth) {
    switch (auth) {
    case SshAuth::Agent:
        return "agent";
    case SshAuth::Password:
        return "password";
    case SshAuth::PublicKey:
        return "publickey";
    case SshAuth::KeyboardInteractive:
        return "keyboard-interactive";
    case SshAuth::Auto:
        break;
    }
    return "auto";
}

BackendKind parseBackend(std::string_view text, bool* parseOk) {
    if (parseOk != nullptr) {
        *parseOk = text == "ssh" || text == "conpty" || text == "telnet" || text == "raw" ||
                   text == "serial";
    }
    if (text == "ssh") {
        return BackendKind::Ssh;
    }
    if (text == "telnet") {
        return BackendKind::Telnet;
    }
    if (text == "raw") {
        return BackendKind::Raw;
    }
    if (text == "serial") {
        return BackendKind::Serial;
    }
    // Unknown text falls back to the local shell, which connects to nothing.
    // `parseOk` is how load() knows to warn rather than silently rewrite.
    return BackendKind::Conpty;
}

SshAuth parseAuth(std::string_view text, bool* parseOk) {
    if (parseOk != nullptr) {
        *parseOk = true;
    }
    if (text == "agent") {
        return SshAuth::Agent;
    }
    if (text == "password") {
        return SshAuth::Password;
    }
    if (text == "publickey") {
        return SshAuth::PublicKey;
    }
    if (text == "keyboard-interactive") {
        return SshAuth::KeyboardInteractive;
    }
    if (parseOk != nullptr) {
        *parseOk = text == "auto";
    }
    return SshAuth::Auto;
}

bool Profile::isExplicit(std::string_view key) const {
    return std::find(explicitKeys.begin(), explicitKeys.end(), key) != explicitKeys.end();
}

void Profile::markExplicit(std::string_view key) {
    if (!isExplicit(key)) {
        explicitKeys.emplace_back(key);
    }
}

std::string slugify(std::string_view name) {
    std::string slug;
    bool pendingDash = false;
    for (const char raw : name) {
        const auto ch = static_cast<unsigned char>(raw);
        const bool alnum =
            (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (alnum) {
            if (pendingDash && !slug.empty()) {
                slug += '-';
            }
            pendingDash = false;
            slug += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        } else {
            // Non-ASCII lands here too: a Thai session name slugifies to
            // nothing, and an empty id is worse than a generic one.
            pendingDash = true;
        }
    }
    return slug.empty() ? "session" : slug;
}

std::vector<std::string> folderChain(std::string_view folder) {
    std::vector<std::string> chain{std::string{}};
    std::size_t start = 0;
    while (start < folder.size()) {
        const std::size_t slash = folder.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? folder.size() : slash;
        if (end > start) {
            chain.emplace_back(folder.substr(0, end));
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return chain;
}

Profile ProfileStore::resolve(const Profile& raw) const {
    Profile resolved;
    resolved.id = raw.id;
    resolved.explicitKeys = raw.explicitKeys;
    // The folder decides WHICH tables apply, so it has to be in place before
    // anything is inherited.
    resolved.folder = raw.folder;

    for (const std::string& folder : folderChain(raw.folder)) {
        for (const auto& [scope, values] : m_inherited) {
            if (scope != folder) {
                continue;
            }
            for (const auto& [key, value] : values) {
                applyField(resolved, key, value);
            }
        }
    }
    // The profile's own keys last: inheritance is a default, never an override.
    resolved.folder = raw.folder;
    for (const std::string& key : raw.explicitKeys) {
        applyField(resolved, key, fieldText(raw, key));
    }
    return resolved;
}

bool ProfileStore::load(const std::string& path) {
    m_path = path;
    m_error.clear();
    m_profiles.clear();
    m_inherited.clear();

    bool exists = false;
    const std::string text = readText(path, &exists);
    if (!exists) {
        return true;  // first run, not a failure
    }

    const toml::parse_result parsed = toml::parse(text, path);
    if (!parsed) {
        m_error = std::string(parsed.error().description());
        return false;
    }
    const toml::table& doc = parsed.table();

    if (const toml::table* defaults = doc["defaults"].as_table()) {
        for (const auto& [key, node] : *defaults) {
            std::string value;
            if (nodeToText(node, &value)) {
                setInherited("", key.str(), value);
            }
        }
    }
    if (const toml::table* folders = doc["folders"].as_table()) {
        for (const auto& [folderKey, folderNode] : *folders) {
            const toml::table* table = folderNode.as_table();
            if (table == nullptr) {
                continue;
            }
            for (const auto& [key, node] : *table) {
                std::string value;
                if (nodeToText(node, &value)) {
                    setInherited(folderKey.str(), key.str(), value);
                }
            }
        }
    }

    const toml::array* sessions = doc["session"].as_array();
    if (sessions == nullptr) {
        return true;  // a file with only defaults is legal
    }
    for (const toml::node& node : *sessions) {
        const toml::table* table = node.as_table();
        if (table == nullptr) {
            continue;
        }
        Profile raw;
        if (const toml::node* id = table->get("id"); id != nullptr && id->is_string()) {
            raw.id = id->ref<std::string>();
        }
        // Read `folder` first: resolve() needs it to pick the inherited tables,
        // and a TOML table hands its keys back in no useful order.
        if (const toml::node* folder = table->get(kFolder);
            folder != nullptr && folder->is_string()) {
            raw.folder = folder->ref<std::string>();
            raw.markExplicit(kFolder);
        }
        for (const auto& [key, value] : *table) {
            const std::string_view name = key.str();
            if (name == "id" || name == kFolder) {
                continue;
            }
            if (name == kTags) {
                const toml::array* array = value.as_array();
                if (array == nullptr) {
                    continue;
                }
                for (const toml::node& tag : *array) {
                    if (tag.is_string()) {
                        raw.tags.push_back(tag.ref<std::string>());
                    }
                }
                raw.markExplicit(kTags);
                continue;
            }
            std::string field;
            if (nodeToText(value, &field) && applyField(raw, name, field)) {
                raw.markExplicit(name);
            }
        }
        if (raw.id.empty()) {
            raw.id = slugify(raw.name);
        }
        m_profiles.push_back(resolve(raw));
    }
    return true;
}

bool ProfileStore::save() const {
    toml::table doc;
    doc.insert("version", kStoreVersion);

    for (const auto& [scope, values] : m_inherited) {
        toml::table table;
        for (const auto& [key, value] : values) {
            table.insert(key, value);
        }
        if (scope.empty()) {
            doc.insert("defaults", std::move(table));
        } else {
            if (doc["folders"].as_table() == nullptr) {
                doc.insert("folders", toml::table{});
            }
            doc["folders"].as_table()->insert(scope, std::move(table));
        }
    }

    toml::array sessions;
    for (const Profile& profile : m_profiles) {
        toml::table table;
        // The id is always written, even when it was derived: pinning it is
        // what keeps a rename from orphaning the profile's vault entry.
        table.insert("id", profile.id);
        for (const std::string& key : profile.explicitKeys) {
            if (key == kTags) {
                toml::array tags;
                for (const std::string& tag : profile.tags) {
                    tags.push_back(tag);
                }
                table.insert(key, std::move(tags));
            } else if (key == kPort) {
                table.insert(key, profile.port);
            } else {
                table.insert(key, fieldText(profile, key));
            }
        }
        sessions.push_back(std::move(table));
    }
    doc.insert("session", std::move(sessions));

    std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << doc;
    return file.good();
}

std::string ProfileStore::add(Profile profile) {
    if (profile.id.empty()) {
        profile.id = slugify(profile.name);
    }
    std::string candidate = profile.id;
    int suffix = 1;
    while (find(candidate) != nullptr) {
        candidate = profile.id + '-' + std::to_string(++suffix);
    }
    profile.id = candidate;
    m_profiles.push_back(resolve(profile));
    return candidate;
}

bool ProfileStore::remove(std::string_view id) {
    const auto it = std::find_if(m_profiles.begin(), m_profiles.end(),
                                 [id](const Profile& p) { return p.id == id; });
    if (it == m_profiles.end()) {
        return false;
    }
    m_profiles.erase(it);
    return true;
}

const Profile* ProfileStore::find(std::string_view id) const {
    const auto it = std::find_if(m_profiles.begin(), m_profiles.end(),
                                 [id](const Profile& p) { return p.id == id; });
    return it == m_profiles.end() ? nullptr : &*it;
}

bool ProfileStore::bulkSet(const std::vector<std::string>& ids, std::string_view field,
                           std::string_view value) {
    // Validated against a scratch profile BEFORE anything is written: a typo
    // must not leave half of twenty hosts edited and the rest untouched.
    Profile probe;
    if (!applyField(probe, field, value)) {
        return false;
    }
    for (const std::string& id : ids) {
        const auto it = std::find_if(m_profiles.begin(), m_profiles.end(),
                                     [&id](const Profile& p) { return p.id == id; });
        if (it == m_profiles.end()) {
            continue;
        }
        applyField(*it, field, value);
        it->markExplicit(field);
    }
    return true;
}

std::vector<std::string> ProfileStore::folders() const {
    std::vector<std::string> all;
    for (const Profile& profile : m_profiles) {
        for (const std::string& folder : folderChain(profile.folder)) {
            if (!folder.empty()) {
                all.push_back(folder);
            }
        }
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    return all;
}

void ProfileStore::setInherited(std::string_view folder, std::string_view key,
                                std::string_view value) {
    for (auto& [scope, values] : m_inherited) {
        if (scope != folder) {
            continue;
        }
        for (auto& [existingKey, existingValue] : values) {
            if (existingKey == key) {
                existingValue = value;
                return;
            }
        }
        values.emplace_back(std::string{key}, std::string{value});
        return;
    }
    m_inherited.emplace_back(std::string{folder}, std::vector<std::pair<std::string, std::string>>{
                                                      {std::string{key}, std::string{value}}});
}

}  // namespace krait::app::session

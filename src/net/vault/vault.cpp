#include "vault.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// After windows.h: dpapi.h needs DATA_BLOB from wincrypt.h, which windows.h
// pulls in. clang-format may sort these; the include guards make that harmless.
#include <dpapi.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace krait::net {

namespace {

constexpr char kMagic[4] = {'K', 'R', 'V', 'T'};
constexpr std::uint32_t kVaultVersion = 1;

// The vault file is a file on disk that something else may have written, so it
// is parsed as hostile input (rules/net.md): every length is bounded and
// checked against what is actually left before a single byte is allocated.
constexpr std::uint32_t kMaxKeyLen = 256;
constexpr std::uint32_t kMaxBlobLen = std::uint32_t{64} * 1024;
constexpr std::size_t kMaxEntries = 4096;

// A passphrase does not need to be larger than this, and a size read from a
// file must never become an allocation request.
constexpr std::size_t kMaxSecretLen = std::size_t{16} * 1024;

void wipe(void* data, std::size_t size) {
    if (data != nullptr && size > 0) {
        // Not memset: the compiler is allowed to delete a memset whose result
        // is never read, which is exactly this call. SecureZeroMemory exists
        // because that optimisation is correct and ruinous here.
        SecureZeroMemory(data, size);
    }
}

// Entropy bound to the entry name. Without it a blob is just a blob: someone
// who can write the file can move the prod credential into the staging entry
// and the app will happily use it.
std::vector<std::uint8_t> entropyFor(std::string_view key) {
    static constexpr std::string_view kDomain = "krait.vault.v1:";
    std::vector<std::uint8_t> entropy;
    entropy.reserve(kDomain.size() + key.size());
    entropy.insert(entropy.end(), kDomain.begin(), kDomain.end());
    entropy.insert(entropy.end(), key.begin(), key.end());
    return entropy;
}

bool readExact(std::ifstream& file, void* out, std::size_t size) {
    file.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(file.gcount()) == size;
}

bool readU32(std::ifstream& file, std::uint32_t* out) {
    std::uint8_t bytes[4] = {};
    if (!readExact(file, bytes, sizeof(bytes))) {
        return false;
    }
    *out = static_cast<std::uint32_t>(bytes[0]) | static_cast<std::uint32_t>(bytes[1]) << 8 |
           static_cast<std::uint32_t>(bytes[2]) << 16 | static_cast<std::uint32_t>(bytes[3]) << 24;
    return true;
}

void writeU32(std::ofstream& file, std::uint32_t value) {
    const std::uint8_t bytes[4] = {static_cast<std::uint8_t>(value & 0xFF),
                                   static_cast<std::uint8_t>(value >> 8 & 0xFF),
                                   static_cast<std::uint8_t>(value >> 16 & 0xFF),
                                   static_cast<std::uint8_t>(value >> 24 & 0xFF)};
    file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

}  // namespace

Secret::Secret(std::string_view text) {
    // Sized once, never grown: a reallocation would leave a copy of the
    // plaintext in the freed block, and this class exists to prevent that.
    m_bytes.resize(text.size());
    std::memcpy(m_bytes.data(), text.data(), text.size());
}

Secret::~Secret() {
    clear();
}

Secret& Secret::operator=(Secret&& other) noexcept {
    if (this != &other) {
        clear();
        m_bytes = std::move(other.m_bytes);
        other.m_bytes.clear();
    }
    return *this;
}

void Secret::clear() {
    // No shrink_to_fit. It may reallocate, and therefore may throw, which makes
    // ~Secret and the move-assignment throwing functions that are declared not
    // to be. The buffer it would release has just been zeroed anyway, and the
    // destructor frees it for real a moment later.
    wipe(m_bytes.data(), m_bytes.size());
    m_bytes.clear();
}

bool Vault::load(const std::string& path) {
    const std::lock_guard lock(m_mutex);
    m_path = path;
    m_error.clear();
    m_entries.clear();

    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        return true;  // first run
    }

    char magic[sizeof(kMagic)] = {};
    std::uint32_t version = 0;
    if (!readExact(file, magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        m_error = "not a krait vault";
        return false;
    }
    if (!readU32(file, &version)) {
        m_error = "truncated header";
        return false;
    }
    if (version > kVaultVersion) {
        // Same rule as the settings registry: a newer file still loads nothing
        // rather than being overwritten by an older build.
        m_error = "vault written by a newer version";
        return false;
    }

    while (file.peek() != EOF) {
        if (m_entries.size() >= kMaxEntries) {
            m_error = "too many entries";
            return false;
        }
        std::uint32_t keyLen = 0;
        if (!readU32(file, &keyLen)) {
            m_error = "truncated entry";
            return false;
        }
        if (keyLen == 0 || keyLen > kMaxKeyLen) {
            m_error = "entry key length out of range";
            return false;
        }
        std::string key(keyLen, '\0');
        if (!readExact(file, key.data(), keyLen)) {
            m_error = "truncated entry key";
            return false;
        }
        std::uint32_t blobLen = 0;
        if (!readU32(file, &blobLen)) {
            m_error = "truncated entry";
            return false;
        }
        if (blobLen == 0 || blobLen > kMaxBlobLen) {
            m_error = "entry blob length out of range";
            return false;
        }
        std::vector<std::uint8_t> blob(blobLen);
        if (!readExact(file, blob.data(), blobLen)) {
            m_error = "truncated entry blob";
            return false;
        }
        m_entries.push_back(Entry{std::move(key), std::move(blob)});
    }
    return true;
}

bool Vault::save() const {
    const std::lock_guard lock(m_mutex);
    // Written beside the real file and renamed over it. Truncating in place
    // means a crash, a full disk or a power cut between the truncate and the
    // last write leaves an empty vault — and every stored credential is gone
    // with no way to tell that is what happened.
    const std::string temporary = m_path + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
        m_error = "cannot open the vault for writing";
        return false;
    }
    file.write(kMagic, sizeof(kMagic));
    writeU32(file, kVaultVersion);
    for (const Entry& entry : m_entries) {
        writeU32(file, static_cast<std::uint32_t>(entry.key.size()));
        file.write(entry.key.data(), static_cast<std::streamsize>(entry.key.size()));
        writeU32(file, static_cast<std::uint32_t>(entry.blob.size()));
        file.write(reinterpret_cast<const char*>(entry.blob.data()),
                   static_cast<std::streamsize>(entry.blob.size()));
    }
    if (!file.good()) {
        m_error = "the vault could not be written";
        file.close();
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
        return false;
    }
    file.close();

    std::error_code renameError;
    std::filesystem::rename(temporary, m_path, renameError);
    if (renameError) {
        m_error = "the vault could not be replaced";
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
        return false;
    }
    return true;
}

bool Vault::store(std::string_view key, const Secret& secret) {
    const std::lock_guard lock(m_mutex);
    if (key.empty() || key.size() > kMaxKeyLen || secret.empty() || secret.size() > kMaxSecretLen) {
        m_error = "key or secret out of range";
        return false;
    }

    std::vector<std::uint8_t> entropy = entropyFor(key);
    DATA_BLOB in{static_cast<DWORD>(secret.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))};
    DATA_BLOB entropyBlob{static_cast<DWORD>(entropy.size()), entropy.data()};
    DATA_BLOB out{};

    // No description string: it is stored in the CLEAR alongside the blob, and
    // there is nothing worth putting there that is also worth leaking. No
    // prompt struct either — that flow is deprecated and removed in 2027.
    if (CryptProtectData(&in, nullptr, &entropyBlob, nullptr, nullptr, 0, &out) == FALSE) {
        // No GetLastError text in the message: it goes to a banner, and this
        // one is about a credential.
        m_error = "the system refused to protect the secret";
        return false;
    }
    std::vector<std::uint8_t> blob(out.pbData, out.pbData + out.cbData);
    wipe(out.pbData, out.cbData);
    LocalFree(out.pbData);

    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [key](const Entry& e) { return e.key == key; });
    if (it != m_entries.end()) {
        it->blob = std::move(blob);
    } else {
        m_entries.push_back(Entry{std::string{key}, std::move(blob)});
    }
    m_error.clear();
    return true;
}

bool Vault::retrieve(std::string_view key, Secret* out) const {
    const std::lock_guard lock(m_mutex);
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [key](const Entry& e) { return e.key == key; });
    if (it == m_entries.end()) {
        return false;
    }

    std::vector<std::uint8_t> entropy = entropyFor(key);
    DATA_BLOB in{static_cast<DWORD>(it->blob.size()), const_cast<BYTE*>(it->blob.data())};
    DATA_BLOB entropyBlob{static_cast<DWORD>(entropy.size()), entropy.data()};
    DATA_BLOB plain{};

    if (CryptUnprotectData(&in, nullptr, &entropyBlob, nullptr, nullptr, 0, &plain) == FALSE) {
        // Tampered, or written by a different user or machine. Either way the
        // caller gets "no", and the reason stays out of the message.
        return false;
    }
    *out = Secret(std::string_view(reinterpret_cast<const char*>(plain.pbData), plain.cbData));
    // The DPAPI buffer is plaintext too. Zero it before letting it go.
    wipe(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return true;
}

bool Vault::erase(std::string_view key) {
    const std::lock_guard lock(m_mutex);
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [key](const Entry& e) { return e.key == key; });
    if (it == m_entries.end()) {
        return false;
    }
    wipe(it->blob.data(), it->blob.size());
    m_entries.erase(it);
    return true;
}

bool Vault::contains(std::string_view key) const {
    const std::lock_guard lock(m_mutex);
    return std::any_of(m_entries.begin(), m_entries.end(),
                       [key](const Entry& e) { return e.key == key; });
}

std::vector<std::string> Vault::keys() const {
    const std::lock_guard lock(m_mutex);
    std::vector<std::string> names;
    names.reserve(m_entries.size());
    for (const Entry& entry : m_entries) {
        names.push_back(entry.key);
    }
    return names;
}

}  // namespace krait::net

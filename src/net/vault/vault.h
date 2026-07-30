#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace krait::net {

// A byte buffer that zeroes itself on the way out (rules/net.md: secrets live
// in memory no longer than needed).
//
// Deliberately not a std::string. A string is free to reallocate on growth and
// leaves the OLD buffer in the heap with the password still in it — zeroing the
// live one afterwards then feels like it worked. This type never grows: the
// size is fixed at construction, and the destructor zeroes exactly what it
// owns. Move-only, because a copy is one more place to forget to wipe.
class Secret {
  public:
    Secret() = default;
    explicit Secret(std::string_view text);
    ~Secret();

    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;

    Secret(Secret&& other) noexcept : m_bytes(std::move(other.m_bytes)) { other.m_bytes.clear(); }

    Secret& operator=(Secret&& other) noexcept;

    bool empty() const { return m_bytes.empty(); }

    std::size_t size() const { return m_bytes.size(); }

    // Borrowed, valid until the next mutation or destruction. Callers must not
    // copy it into anything that outlives the Secret.
    std::string_view view() const { return {m_bytes.data(), m_bytes.size()}; }

    const char* data() const { return m_bytes.data(); }

    void clear();

  private:
    std::vector<char> m_bytes;
};

// The DPAPI-backed secret store (rules/net.md: DPAPI vault only — never TOML,
// never logs, never error strings).
//
// Each entry is encrypted individually with CryptProtectData, and the ENTROPY
// is derived from the entry's key. Two consequences worth having: one corrupted
// entry does not cost the whole file, and someone who can write the file cannot
// swap the prod password into the staging entry, because the blob will not
// decrypt under the other key's entropy.
//
// Errors are values. A vault file is a file on disk that something else may
// have touched, so a bad one degrades to "no secrets" and says why.
class Vault {
  public:
    // A missing file is a first run, not a failure — true either way. False
    // only when the file exists and is not a vault; error() says what.
    bool load(const std::string& path);
    bool save() const;

    const std::string& path() const { return m_path; }

    const std::string& error() const { return m_error; }

    // Encrypts immediately: only ciphertext is held afterwards. Replaces any
    // existing entry for `key`.
    bool store(std::string_view key, const Secret& secret);
    // Decrypts on demand. False when the key is absent OR when the blob fails
    // to decrypt — a tampered entry and a missing one are the same answer to a
    // caller, and telling them apart out loud is a hint we do not owe.
    bool retrieve(std::string_view key, Secret* out) const;

    bool erase(std::string_view key);
    bool contains(std::string_view key) const;
    // Key names only, never values. Used by the settings UI to show which
    // profiles have a stored credential.
    std::vector<std::string> keys() const;

  private:
    struct Entry {
        std::string key;
        std::vector<std::uint8_t> blob;  // DPAPI ciphertext, never plaintext
    };

    std::string m_path;
    mutable std::string m_error;
    std::vector<Entry> m_entries;
};

}  // namespace krait::net

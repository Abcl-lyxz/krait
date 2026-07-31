#include "vault/vault.h"
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace krait::net;

namespace {

// Invented, and obviously so: a test fixture that looks like a real credential
// is a test fixture someone eventually pastes somewhere real.
constexpr const char* kSecretText = "hunter2-not-a-real-password";

class TempFile {
  public:
    explicit TempFile(std::string name)
        : m_path((std::filesystem::temp_directory_path() / std::move(name)).string()) {}

    ~TempFile() {
        // The error_code overload: the throwing one makes this destructor a
        // throwing destructor, and a temp file that will not delete is not
        // worth terminating a test run over.
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const { return m_path; }

    std::string readAll() const {
        std::ifstream file(m_path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    void write(std::string_view bytes) const {
        std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

  private:
    std::string m_path;
};

}  // namespace

TEST_CASE("a secret round-trips through DPAPI and survives a save/load", "[net][vault]") {
    TempFile file("krait-vault-roundtrip.dat");

    Vault vault;
    REQUIRE(vault.load(file.path()));  // missing file is a first run
    REQUIRE(vault.keys().empty());

    REQUIRE(vault.store("web-1:password", Secret(kSecretText)));
    REQUIRE(vault.contains("web-1:password"));
    REQUIRE(vault.save());

    Vault reopened;
    REQUIRE(reopened.load(file.path()));
    REQUIRE(reopened.keys() == std::vector<std::string>{"web-1:password"});

    Secret out;
    REQUIRE(reopened.retrieve("web-1:password", &out));
    CHECK(out.view() == kSecretText);

    Secret missing;
    CHECK_FALSE(reopened.retrieve("web-1:passphrase", &missing));
    CHECK(missing.empty());
}

TEST_CASE("the file never contains the plaintext", "[net][vault]") {
    TempFile file("krait-vault-plaintext.dat");
    Vault vault;
    REQUIRE(vault.load(file.path()));
    REQUIRE(vault.store("web-1:password", Secret(kSecretText)));
    REQUIRE(vault.save());

    // rules/net.md: never in TOML, never in logs, never in error strings. The
    // file is the one place a mistake here would be permanent and greppable.
    const std::string bytes = file.readAll();
    CHECK(bytes.find(kSecretText) == std::string::npos);
    // The key name IS in the clear, deliberately — the settings UI lists which
    // profiles have a credential without decrypting anything.
    CHECK(bytes.find("web-1:password") != std::string::npos);
}

TEST_CASE("a blob cannot be moved between entries", "[net][vault]") {
    TempFile file("krait-vault-swap.dat");
    Vault vault;
    REQUIRE(vault.load(file.path()));
    REQUIRE(vault.store("prod:password", Secret("prod-secret-value")));
    REQUIRE(vault.store("staging:password", Secret("staging-secret-value")));
    REQUIRE(vault.save());

    // Rename an entry in the file, leaving its ciphertext intact and its length
    // field valid — same byte count, so the file still parses. This is the
    // attack the per-key entropy exists to stop: the blob is untouched, but it
    // no longer decrypts, because the entropy is derived from the key name.
    std::string bytes = file.readAll();
    const std::size_t prodAt = bytes.find("prod:password");
    REQUIRE(prodAt != std::string::npos);
    bytes.replace(prodAt, std::string("prod:password").size(), "prod:passwerd");
    file.write(bytes);

    Vault tampered;
    REQUIRE(tampered.load(file.path()));
    Secret out;
    CHECK_FALSE(tampered.retrieve("prod:passwerd", &out));
    CHECK(out.empty());
    // The untouched entry still works, so the failure above is the entropy
    // check and not the whole file having been rejected.
    Secret staging;
    CHECK(tampered.retrieve("staging:password", &staging));
    CHECK(staging.view() == "staging-secret-value");
}

TEST_CASE("erase and overwrite behave", "[net][vault]") {
    TempFile file("krait-vault-erase.dat");
    Vault vault;
    REQUIRE(vault.load(file.path()));

    REQUIRE(vault.store("k", Secret("first-value")));
    REQUIRE(vault.store("k", Secret("second-value")));
    CHECK(vault.keys().size() == 1);  // replaced, not appended

    Secret out;
    REQUIRE(vault.retrieve("k", &out));
    CHECK(out.view() == "second-value");

    CHECK(vault.erase("k"));
    CHECK_FALSE(vault.erase("k"));
    CHECK_FALSE(vault.contains("k"));
}

TEST_CASE("a hostile vault file is refused, not parsed", "[net][vault]") {
    TempFile file("krait-vault-hostile.dat");

    SECTION("wrong magic") {
        file.write(std::string("NOPE\x01\x00\x00\x00", 8));
        Vault vault;
        CHECK_FALSE(vault.load(file.path()));
        CHECK(vault.error() == "not a krait vault");
    }
    SECTION("a length that would allocate the world") {
        // keyLen = 0xFFFFFFFF. The bound has to be checked BEFORE the
        // allocation, or a four-byte edit to this file is an OOM.
        file.write(std::string("KRVT\x01\x00\x00\x00\xFF\xFF\xFF\xFF", 12));
        Vault vault;
        CHECK_FALSE(vault.load(file.path()));
        CHECK(vault.error() == "entry key length out of range");
        CHECK(vault.keys().empty());
    }
    SECTION("truncated mid-entry") {
        file.write(std::string("KRVT\x01\x00\x00\x00\x02\x00\x00\x00k", 13));
        Vault vault;
        CHECK_FALSE(vault.load(file.path()));
        CHECK_FALSE(vault.error().empty());
    }
    SECTION("a future version is refused rather than overwritten") {
        file.write(std::string("KRVT\x63\x00\x00\x00", 8));
        Vault vault;
        CHECK_FALSE(vault.load(file.path()));
        CHECK(vault.error() == "vault written by a newer version");
    }
}

TEST_CASE("Secret is move-only and clears itself", "[net][vault]") {
    Secret secret(kSecretText);
    CHECK(secret.size() == std::string(kSecretText).size());

    Secret moved = std::move(secret);
    CHECK(moved.view() == kSecretText);
    CHECK(secret.empty());  // NOLINT(bugprone-use-after-move): that IS the check

    moved.clear();
    CHECK(moved.empty());
    CHECK(moved.size() == 0);

    // An empty secret is not storable: it would be an entry that decrypts to
    // nothing, which reads as "there is a password" and is worse than absent.
    Vault vault;
    CHECK_FALSE(vault.store("k", Secret()));
}

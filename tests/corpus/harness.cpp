// Corpus harness v0 (docs/plan/03-test-strategy.md §2).
// Case file format, one or more pairs per file:
//   # comment / blank lines ignored
//   IN  <bytes: printable ASCII verbatim, \xNN escapes>
//   EXPECT <tokens>   — for utf8/ cases: U+XXXX scalar values
#include "core/unicode/utf8.h"
#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> parseBytes(const std::string& spec) {
    std::vector<std::uint8_t> bytes;
    for (size_t i = 0; i < spec.size();) {
        if (spec[i] == '\\' && i + 3 < spec.size() && spec[i + 1] == 'x') {
            bytes.push_back(
                static_cast<std::uint8_t>(std::stoi(spec.substr(i + 2, 2), nullptr, 16)));
            i += 4;
        } else {
            bytes.push_back(static_cast<std::uint8_t>(spec[i]));
            ++i;
        }
    }
    return bytes;
}

std::vector<char32_t> parseScalars(const std::string& spec) {
    std::vector<char32_t> scalars;
    size_t pos = 0;
    while ((pos = spec.find("U+", pos)) != std::string::npos) {
        size_t end = pos + 2;
        while (end < spec.size() && std::isxdigit(static_cast<unsigned char>(spec[end]))) {
            ++end;
        }
        scalars.push_back(
            static_cast<char32_t>(std::stoul(spec.substr(pos + 2, end - pos - 2), nullptr, 16)));
        pos = end;
    }
    return scalars;
}

std::vector<char32_t> decodeAll(const std::vector<std::uint8_t>& bytes) {
    krait::core::Utf8Decoder decoder;
    std::vector<char32_t> result;
    char32_t out[2];
    for (std::uint8_t b : bytes) {
        const int n = decoder.feed(b, out);
        result.insert(result.end(), out, out + n);
    }
    char32_t tail[1];
    if (decoder.finish(tail) == 1) {
        result.push_back(tail[0]);
    }
    return result;
}

}  // namespace

TEST_CASE("corpus: utf8 decode", "[corpus][utf8]") {
    const std::filesystem::path dir = std::filesystem::path(KRAIT_CORPUS_DIR) / "utf8";
    size_t cases = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".case") {
            continue;
        }
        std::ifstream file(entry.path());
        REQUIRE(file.is_open());
        std::string line;
        std::string pendingIn;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || line[0] == '#') {
                continue;
            }
            if (line.rfind("IN ", 0) == 0) {
                pendingIn = line.substr(3);
            } else if (line.rfind("EXPECT ", 0) == 0) {
                ++cases;
                CAPTURE(entry.path().filename().string(), pendingIn);
                CHECK(decodeAll(parseBytes(pendingIn)) == parseScalars(line.substr(7)));
            }
        }
    }
    CHECK(cases > 0);  // an empty corpus directory is a broken corpus
}

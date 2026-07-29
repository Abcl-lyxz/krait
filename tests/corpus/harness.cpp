// Corpus harness (docs/plan/03-test-strategy.md §2).
// Case file format, one or more pairs per file:
//   # comment / blank lines ignored
//   MODE c1          — optional: parse subsequent cases with acceptC1 on
//   IN  <bytes: printable ASCII verbatim, \xNN escapes>
//   EXPECT <tokens>  — utf8/ cases: U+XXXX scalar values
//                      parser/ cases: event tokens, whitespace-separated:
//                        U+XXXX   print            E:XX      execute (hex)
//                        ESC:<inters><final>       CSI:/DCS:<markers><params><inters><final>
//                        PUT[..]  coalesced DCS data          UNHOOK
//                        OSC[..]  payload, emitted when the OSC ends
//                        ]! / UNHOOK! — the string was aborted (CAN/SUB/C1)
//                        -        no events at all
//                      Non-printables and space render as \xNN (uppercase).
#include "core/parser/machine.h"
#include "core/unicode/utf8.h"
#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct CaseEntry {
    std::string file;
    std::string in;
    std::string expect;
    bool c1 = false;
};

std::vector<CaseEntry> loadCases(const std::filesystem::path& dir) {
    std::vector<CaseEntry> cases;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".case") {
            continue;
        }
        std::ifstream file(entry.path());
        REQUIRE(file.is_open());
        std::string line;
        std::string pendingIn;
        bool c1 = false;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || line[0] == '#') {
                continue;
            }
            if (line == "MODE c1") {
                c1 = true;
            } else if (line.rfind("IN ", 0) == 0) {
                pendingIn = line.substr(3);
            } else if (line.rfind("EXPECT ", 0) == 0) {
                cases.push_back({entry.path().filename().string(), pendingIn, line.substr(7), c1});
            }
        }
    }
    return cases;
}

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

std::string escapeByte(std::uint8_t b) {
    if (b > 0x20 && b < 0x7F) {
        return std::string(1, static_cast<char>(b));
    }
    return std::format("\\x{:02X}", b);
}

// Records parser events as the token strings documented in the header.
class EventRecorder final : public krait::core::vt::ParserEvents {
  public:
    std::vector<std::string> tokens;

    void print(char32_t cp) override {
        flushPut();
        tokens.push_back(std::format("U+{:04X}", static_cast<std::uint32_t>(cp)));
    }

    void execute(std::uint8_t control) override {
        flushPut();
        tokens.push_back(std::format("E:{:02X}", control));
    }

    void escDispatch(std::span<const std::uint8_t> intermediates, std::uint8_t final) override {
        flushPut();
        std::string s = "ESC:";
        for (std::uint8_t b : intermediates) {
            s += escapeByte(b);
        }
        tokens.push_back(s + escapeByte(final));
    }

    void csiDispatch(const krait::core::vt::Params& params,
                     std::span<const std::uint8_t> intermediates, std::uint8_t final) override {
        flushPut();
        tokens.push_back("CSI:" + header(params, intermediates) + escapeByte(final));
    }

    void dcsHook(const krait::core::vt::Params& params, std::span<const std::uint8_t> intermediates,
                 std::uint8_t final) override {
        flushPut();
        tokens.push_back("DCS:" + header(params, intermediates) + escapeByte(final));
    }

    void dcsPut(std::uint8_t byte) override { m_put += escapeByte(byte); }

    void dcsUnhook(bool aborted) override {
        flushPut();
        tokens.emplace_back(aborted ? "UNHOOK!" : "UNHOOK");
    }

    void oscStart() override { m_osc.clear(); }

    void oscPut(std::uint8_t byte) override { m_osc += escapeByte(byte); }

    void oscEnd(bool aborted) override {
        flushPut();
        tokens.push_back("OSC[" + m_osc + (aborted ? "]!" : "]"));
    }

  private:
    // Private markers precede params on the wire, intermediates follow; the
    // parser's collect buffer keeps arrival order, and the ranges are
    // disjoint, so a split by range reconstructs the wire order.
    static std::string header(const krait::core::vt::Params& params,
                              std::span<const std::uint8_t> intermediates) {
        std::string markers;
        std::string inters;
        for (std::uint8_t b : intermediates) {
            (b >= 0x3C ? markers : inters) += escapeByte(b);
        }
        std::string ps;
        for (std::size_t i = 0; i < params.count; ++i) {
            if (i > 0) {
                ps += params.subparam[i] ? ':' : ';';
            }
            ps += std::to_string(params.values[i]);
        }
        return markers + ps + inters;
    }

    void flushPut() {
        if (!m_put.empty()) {
            tokens.push_back("PUT[" + m_put + "]");
            m_put.clear();
        }
    }

    std::string m_put;
    std::string m_osc;
};

std::vector<std::string> parseTokens(const std::string& spec) {
    std::vector<std::string> tokens;
    std::istringstream in(spec);
    std::string tok;
    while (in >> tok) {
        tokens.push_back(tok);
    }
    if (tokens.size() == 1 && tokens[0] == "-") {
        tokens.clear();  // "-" = expect no events
    }
    return tokens;
}

}  // namespace

TEST_CASE("corpus: utf8 decode", "[corpus][utf8]") {
    const auto cases = loadCases(std::filesystem::path(KRAIT_CORPUS_DIR) / "utf8");
    for (const auto& c : cases) {
        CAPTURE(c.file, c.in);
        CHECK(decodeAll(parseBytes(c.in)) == parseScalars(c.expect));
    }
    CHECK(!cases.empty());  // an empty corpus directory is a broken corpus
}

TEST_CASE("corpus: parser events", "[corpus][parser]") {
    const auto cases = loadCases(std::filesystem::path(KRAIT_CORPUS_DIR) / "parser");
    for (const auto& c : cases) {
        CAPTURE(c.file, c.in);
        const auto bytes = parseBytes(c.in);
        const auto expected = parseTokens(c.expect);

        EventRecorder whole;
        krait::core::vt::Parser parser(whole, c.c1);
        parser.feed(bytes);
        CHECK(whole.tokens == expected);

        // Chunk boundaries must be invisible: byte-at-a-time == one buffer.
        EventRecorder split;
        krait::core::vt::Parser splitParser(split, c.c1);
        for (std::uint8_t b : bytes) {
            splitParser.feed({&b, 1});
        }
        CHECK(split.tokens == whole.tokens);
    }
    CHECK(!cases.empty());
}

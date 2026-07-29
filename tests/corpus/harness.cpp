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
#include "core/caps/caps.h"
#include "core/parser/csi_cursor.h"
#include "core/parser/machine.h"
#include "core/parser/sgr.h"
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

// Feeds execute/csiDispatch into the T6 stub grid; everything else ignored.
// csi/ case EXPECT tokens: cur:R,C (1-based), g1:on if shifted, bell:N if >0.
class CursorSink final : public krait::core::vt::ParserEvents {
  public:
    krait::core::vt::Grid grid{24, 80};
    krait::core::vt::Capabilities caps;
    krait::core::vt::ReplyLimiter limiter;
    std::vector<std::string> replyTokens;  // reports/ cases assert these

    void print(char32_t cp) override { grid.putChar(cp); }

    void execute(std::uint8_t control) override { krait::core::vt::handleControl(grid, control); }

    void escDispatch(std::span<const std::uint8_t>, std::uint8_t) override {}

    void csiDispatch(const krait::core::vt::Params& params,
                     std::span<const std::uint8_t> intermediates, std::uint8_t final) override {
        // Interim routing; the real terminal layer owns this from T15 on.
        if (final == 'm') {
            krait::core::vt::applySgr(grid.pen, params, intermediates);
        } else if (final == 'J' || final == 'K') {
            krait::core::vt::handleErase(grid, params, intermediates, final);
        } else if (final == 'c' || final == 'n') {
            std::string out;
            krait::core::vt::handleReport(grid, caps, params, intermediates, final, limiter, out);
            if (!out.empty()) {
                std::string tok = "reply:";
                for (const char ch : out) {
                    tok += escapeByte(static_cast<std::uint8_t>(ch));
                }
                replyTokens.push_back(tok);
            }
        } else {
            krait::core::vt::handleCsiCursor(grid, params, intermediates, final);
        }
    }

    void dcsHook(const krait::core::vt::Params&, std::span<const std::uint8_t>,
                 std::uint8_t) override {}

    void dcsPut(std::uint8_t) override {}

    void dcsUnhook(bool) override {}

    void oscStart() override {}

    void oscPut(std::uint8_t) override {}

    void oscEnd(bool) override {}

    // sgr/ case tokens: pen:<flags>/<fg>/<bg> then line:N:<chars, '.'=erased>
    // for every row containing at least one written cell (trailing erased
    // cells trimmed). Corpus text is ASCII-only by construction.
    std::vector<std::string> describeSgr() const {
        namespace vt = krait::core::vt;
        std::vector<std::string> tokens;
        static constexpr std::array<std::pair<std::uint16_t, const char*>, 8> kNames{
            {{vt::Attr::kBold, "bold"},
             {vt::Attr::kDim, "dim"},
             {vt::Attr::kItalic, "italic"},
             {vt::Attr::kUnderline, "ul"},
             {vt::Attr::kBlink, "blink"},
             {vt::Attr::kReverse, "rev"},
             {vt::Attr::kInvisible, "hide"},
             {vt::Attr::kStrike, "strike"}}};
        std::string flags;
        for (const auto& [bit, name] : kNames) {
            if ((grid.pen.flags & bit) != 0) {
                if (!flags.empty()) {
                    flags += ',';
                }
                flags += name;
            }
        }
        if (flags.empty()) {
            flags = "-";
        }
        const auto color = [](const vt::Color& c) {
            return c.kind == vt::Color::Kind::Default ? std::string("def")
                                                      : std::to_string(c.index);
        };
        tokens.push_back("pen:" + flags + "/" + color(grid.pen.fg) + "/" + color(grid.pen.bg));
        for (int r = 0; r < grid.rows; ++r) {
            std::string line;
            for (int c = 0; c < grid.cols; ++c) {
                const char32_t ch = grid.cellAt(r, c).ch;
                line += ch == 0 ? '.' : static_cast<char>(ch);
            }
            while (!line.empty() && line.back() == '.') {
                line.pop_back();
            }
            if (!line.empty()) {
                tokens.push_back(std::format("line:{}:{}", r + 1, line));
            }
        }
        return tokens;
    }

    std::vector<std::string> describe() const {
        std::vector<std::string> tokens;
        tokens.push_back(std::format("cur:{},{}", grid.row + 1, grid.col + 1));
        if (grid.g1Invoked) {
            tokens.emplace_back("g1:on");
        }
        if (grid.bells > 0) {
            tokens.push_back(std::format("bell:{}", grid.bells));
        }
        return tokens;
    }
};

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

TEST_CASE("corpus: csi cursor + c0 controls", "[corpus][csi]") {
    const auto cases = loadCases(std::filesystem::path(KRAIT_CORPUS_DIR) / "csi");
    for (const auto& c : cases) {
        CAPTURE(c.file, c.in);
        CursorSink sink;
        krait::core::vt::Parser parser(sink, c.c1);
        parser.feed(parseBytes(c.in));
        CHECK(sink.describe() == parseTokens(c.expect));
    }
    CHECK(!cases.empty());
}

TEST_CASE("corpus: reports (DA1/DSR) honest replies", "[corpus][reports]") {
    const auto cases = loadCases(std::filesystem::path(KRAIT_CORPUS_DIR) / "reports");
    for (const auto& c : cases) {
        CAPTURE(c.file, c.in);
        CursorSink sink;
        krait::core::vt::Parser parser(sink, c.c1);
        const auto bytes = parseBytes(c.in);
        // Production wiring: addInput per chunk, then feed the chunk — this
        // is what exercises the limiter's refill path.
        for (std::size_t off = 0; off < bytes.size(); off += 64) {
            const std::size_t n = std::min<std::size_t>(64, bytes.size() - off);
            sink.limiter.addInput(n);
            parser.feed(std::span(bytes).subspan(off, n));
        }
        CHECK(sink.replyTokens == parseTokens(c.expect));
    }
    CHECK(!cases.empty());
}

TEST_CASE("corpus: sgr + erase", "[corpus][sgr]") {
    const auto cases = loadCases(std::filesystem::path(KRAIT_CORPUS_DIR) / "sgr");
    for (const auto& c : cases) {
        CAPTURE(c.file, c.in);
        CursorSink sink;
        krait::core::vt::Parser parser(sink, c.c1);
        parser.feed(parseBytes(c.in));
        CHECK(sink.describeSgr() == parseTokens(c.expect));
    }
    CHECK(!cases.empty());
}

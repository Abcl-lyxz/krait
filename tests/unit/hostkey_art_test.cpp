#include "ssh/hostkey_art.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

using krait::net::randomart;

namespace {

std::vector<std::string> lines(const std::string& art) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= art.size()) {
        const std::size_t nl = art.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(art.substr(start));
            break;
        }
        out.push_back(art.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

}  // namespace

TEST_CASE("the art is a box of the size ssh-keygen prints", "[net][ssh][art]") {
    const unsigned char digest[32] = {0x9b, 0x4c, 0x2f, 0x71, 0x08, 0xe5, 0xd3, 0x1a,
                                      0x66, 0xbe, 0x40, 0x17, 0xc2, 0x8d, 0x59, 0x03,
                                      0xf7, 0x21, 0x6a, 0xcd, 0x84, 0x35, 0x9e, 0x72,
                                      0x0b, 0xd6, 0x48, 0xaf, 0x13, 0x5c, 0xe0, 0x92};
    const std::vector<std::string> rows =
        lines(randomart(digest, sizeof(digest), "ED25519 256", "SHA256"));

    // Two borders plus nine field rows, every one 19 wide.
    REQUIRE(rows.size() == 11);
    for (const std::string& row : rows) {
        CHECK(row.size() == 19);
    }
    CHECK(rows.front().front() == '+');
    CHECK(rows.front().back() == '+');
    CHECK(rows.back().front() == '+');
    CHECK(rows.back().back() == '+');
    CHECK(rows.front().find("[ED25519 256]") != std::string::npos);
    CHECK(rows.back().find("[SHA256]") != std::string::npos);
    for (std::size_t i = 1; i + 1 < rows.size(); ++i) {
        CHECK(rows[i].front() == '|');
        CHECK(rows[i].back() == '|');
    }
}

TEST_CASE("the walk is OpenSSH's, step for step", "[net][ssh][art]") {
    // One zero byte is four moves of "both bits clear" = up-left each time.
    // Starting at the centre (col 8, row 4) that is
    //   (8,4) -> (7,3) -> (6,2) -> (5,1) -> (4,0),
    // so three squares visited once ('.'), 'S' left at the centre, and 'E'
    // where he stopped. Hand-computable, which is the point: it pins the bit
    // ORDER and the start square, and those are the two things that would
    // silently produce a plausible-looking but wrong picture.
    const unsigned char digest[1] = {0x00};
    const std::vector<std::string> rows = lines(randomart(digest, sizeof(digest), "T", "H"));

    REQUIRE(rows.size() == 11);
    const auto cell = [&rows](std::size_t col, std::size_t row) { return rows[row + 1][col + 1]; };
    CHECK(cell(4, 0) == 'E');
    CHECK(cell(5, 1) == '.');
    CHECK(cell(6, 2) == '.');
    CHECK(cell(7, 3) == '.');
    CHECK(cell(8, 4) == 'S');
    CHECK(cell(0, 0) == ' ');
}

TEST_CASE("the bishop is stopped by the walls, not wrapped", "[net][ssh][art]") {
    // 0xFF is four moves of down-right. From the centre he reaches the corner
    // in four and then stays there. Wrapping instead of clamping would put the
    // end mark on the opposite side and make far more keys collide.
    const unsigned char digest[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const std::vector<std::string> rows = lines(randomart(digest, sizeof(digest), "T", "H"));
    CHECK(rows[9][17] == 'E');  // bottom-right field square
}

TEST_CASE("different keys look different, the same key does not", "[net][ssh][art]") {
    const unsigned char a[4] = {0x01, 0x02, 0x03, 0x04};
    const unsigned char b[4] = {0x01, 0x02, 0x03, 0x05};
    CHECK(randomart(a, sizeof(a), "T", "H") == randomart(a, sizeof(a), "T", "H"));
    CHECK(randomart(a, sizeof(a), "T", "H") != randomart(b, sizeof(b), "T", "H"));

    // An empty digest still draws a legal box: the UI must never be handed
    // something it cannot lay out, whatever the server sent.
    const std::vector<std::string> rows = lines(randomart(nullptr, 0, "T", "H"));
    REQUIRE(rows.size() == 11);
    CHECK(rows[5][9] == 'E');  // start and end are the same square
}

TEST_CASE("an over-long title is truncated rather than breaking the box", "[net][ssh][art]") {
    const unsigned char digest[1] = {0x42};
    const std::vector<std::string> rows =
        lines(randomart(digest, sizeof(digest), "A VERY LONG KEY TYPE NAME INDEED", "SHA512"));
    for (const std::string& row : rows) {
        CHECK(row.size() == 19);
    }
}

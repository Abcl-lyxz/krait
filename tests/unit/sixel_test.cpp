#include "core/graphics/sixel.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

using namespace krait::core::vt;

namespace {

// Runs a payload through the decoder. `p2` is the DCS background-select
// parameter, the only one of the three that changes what is drawn.
std::optional<Image> decode(std::string_view payload, std::uint16_t p2 = 0) {
    Params params;
    params.values[0] = 1;
    params.values[1] = p2;
    params.values[2] = 0;
    params.count = 3;

    SixelDecoder decoder;
    decoder.begin(params);
    for (const char ch : payload) {
        decoder.put(static_cast<std::uint8_t>(ch));
    }
    return decoder.end(false);
}

std::uint32_t at(const Image& image, int x, int y) {
    return image.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                        static_cast<std::size_t>(x)];
}

constexpr std::uint32_t kRed = 0xFFFF0000;
constexpr std::uint32_t kGreen = 0xFF00FF00;
constexpr std::uint32_t kBlack = 0xFF000000;
constexpr std::uint32_t kTransparent = 0x00000000;

}  // namespace

TEST_CASE("a sixel byte's least significant bit is the TOP pixel", "[core][sixel]") {
    // The classic way to get this format wrong. '@' is 0x40, so bits = 1: only
    // the top pixel of the band. Reversing it decodes an image that looks
    // almost right and is mirrored inside every six-pixel band — exactly the
    // kind of wrong that survives a casual look at a screenshot.
    const std::optional<Image> top = decode("#1;2;100;0;0@");
    REQUIRE(top.has_value());
    REQUIRE(top->width == 1);
    REQUIRE(top->height == 6);
    CHECK(at(*top, 0, 0) == kRed);
    for (int y = 1; y < 6; ++y) {
        CHECK(at(*top, 0, y) == kBlack);
    }

    // '~' is 0x7E: all six bits, the whole band.
    const std::optional<Image> all = decode("#1;2;100;0;0~");
    REQUIRE(all.has_value());
    for (int y = 0; y < 6; ++y) {
        CHECK(at(*all, 0, y) == kRed);
    }

    // 0x20 is the bottom pixel alone ('?' + 32 = '_').
    const std::optional<Image> bottom = decode("#1;2;100;0;0_");
    REQUIRE(bottom.has_value());
    CHECK(at(*bottom, 0, 5) == kRed);
    CHECK(at(*bottom, 0, 0) == kBlack);
}

TEST_CASE("colour components are PERCENTAGES, not bytes", "[core][sixel]") {
    // The other classic. Reading 100 as 100/255 makes every image about 40% as
    // bright as it should be — dark, plausible, and very hard to spot without
    // something to compare against.
    const std::optional<Image> full = decode("#1;2;100;100;100~");
    REQUIRE(full.has_value());
    CHECK(at(*full, 0, 0) == 0xFFFFFFFF);

    const std::optional<Image> half = decode("#1;2;50;0;0~");
    REQUIRE(half.has_value());
    // 50% of 255, rounded: 128, not 50.
    CHECK(at(*half, 0, 0) == 0xFF800000);
}

TEST_CASE("the repeat introducer repeats one character", "[core][sixel]") {
    const std::optional<Image> image = decode("#1;2;0;100;0!5~");
    REQUIRE(image.has_value());
    CHECK(image->width == 5);
    CHECK(image->height == 6);
    for (int x = 0; x < 5; ++x) {
        CHECK(at(*image, x, 0) == kGreen);
    }

    // A repeat with no count is one.
    const std::optional<Image> bare = decode("#1;2;0;100;0!~");
    REQUIRE(bare.has_value());
    CHECK(bare->width == 1);

    // A zero count draws nothing but must not wedge the decoder.
    const std::optional<Image> zero = decode("#1;2;0;100;0!0~~");
    REQUIRE(zero.has_value());
    CHECK(zero->width == 1);
}

TEST_CASE("$ returns to the left margin and - starts a new band", "[core][sixel]") {
    // Red across the top band, then green across the second.
    const std::optional<Image> image = decode("#1;2;100;0;0!3~-#2;2;0;100;0!3~");
    REQUIRE(image.has_value());
    CHECK(image->width == 3);
    CHECK(image->height == 12);
    CHECK(at(*image, 0, 0) == kRed);
    CHECK(at(*image, 2, 5) == kRed);
    CHECK(at(*image, 0, 6) == kGreen);
    CHECK(at(*image, 2, 11) == kGreen);

    // $ overprints the SAME band, which is how encoders layer colours.
    const std::optional<Image> overprint = decode("#1;2;100;0;0!3@$#2;2;0;100;0!3_");
    REQUIRE(overprint.has_value());
    CHECK(overprint->height == 6);
    CHECK(at(*overprint, 0, 0) == kRed);    // from the first pass
    CHECK(at(*overprint, 0, 5) == kGreen);  // from the second
}

TEST_CASE("raster attributes declare the size", "[core][sixel]") {
    // "1;1;4;3 says the image is 4x3 even though the data fills a 6-high band.
    const std::optional<Image> image = decode("\"1;1;4;3#1;2;100;0;0!4~");
    REQUIRE(image.has_value());
    CHECK(image->width == 4);
    CHECK(image->height == 3);

    // A DECLARED width survives a final band that ends early: cropping to the
    // last written pixel would shrink an image with a transparent right edge.
    const std::optional<Image> wide = decode("\"1;1;8;6#1;2;100;0;0!2~", 1);
    REQUIRE(wide.has_value());
    CHECK(wide->width == 8);
    CHECK(at(*wide, 7, 0) == kTransparent);
}

TEST_CASE("P2 = 1 leaves zero bits transparent", "[core][sixel]") {
    // "Pixel positions specified as 0 remain at their current color", which for
    // an image with nothing underneath means transparent.
    const std::optional<Image> transparent = decode("#1;2;100;0;0@", 1);
    REQUIRE(transparent.has_value());
    CHECK(at(*transparent, 0, 0) == kRed);
    CHECK(at(*transparent, 0, 1) == kTransparent);

    // Every other P2 paints the background instead.
    for (const std::uint16_t p2 : {std::uint16_t{0}, std::uint16_t{2}}) {
        const std::optional<Image> opaque = decode("#1;2;100;0;0@", p2);
        REQUIRE(opaque.has_value());
        CHECK(at(*opaque, 0, 1) == kBlack);
    }
}

TEST_CASE("HLS colours use sixel's ranges and DEC's hue origin", "[core][sixel]") {
    // Hue is 0-360 DEGREES, lightness and saturation 0-100 PERCENT. DEC's zero
    // is BLUE rather than red — getting that wrong swaps the primaries in every
    // HLS image, which looks like a decoder that merely has odd taste.
    CHECK(sixelHlsToRgb(0, 50, 100) == 0xFF0000FF);    // blue
    CHECK(sixelHlsToRgb(120, 50, 100) == 0xFFFF0000);  // red
    CHECK(sixelHlsToRgb(240, 50, 100) == 0xFF00FF00);  // green

    // Zero saturation is grey at the given lightness, whatever the hue.
    CHECK(sixelHlsToRgb(37, 0, 0) == 0xFF000000);
    CHECK(sixelHlsToRgb(37, 100, 0) == 0xFFFFFFFF);

    // Out-of-range values are clamped or wrapped, never allowed to index out.
    CHECK(sixelHlsToRgb(-120, 50, 100) == sixelHlsToRgb(240, 50, 100));
    CHECK(sixelHlsToRgb(720, 50, 100) == sixelHlsToRgb(0, 50, 100));
    CHECK(sixelHlsToRgb(0, 500, 500) == 0xFFFFFFFF);

    // And through the decoder, where Pu = 1 selects it.
    const std::optional<Image> image = decode("#1;1;120;50;100~");
    REQUIRE(image.has_value());
    CHECK(at(*image, 0, 0) == 0xFFFF0000);
}

TEST_CASE("the default palette is the VT340's", "[core][sixel]") {
    // A file that selects a colour without defining it relies on this table,
    // and most real files define everything — so without a test naming it, the
    // table could be wrong for years without anyone noticing.
    const std::span<const std::uint32_t> palette = sixelDefaultPalette();
    REQUIRE(palette.size() == 16);
    CHECK(palette[0] == 0xFF000000);
    CHECK(palette[15] == 0xFFCCCCCC);  // 80% of 255, rounded

    const std::optional<Image> image = decode("#2~");
    REQUIRE(image.has_value());
    CHECK(at(*image, 0, 0) == palette[2]);
}

TEST_CASE("selecting a colour and defining one are the same command", "[core][sixel]") {
    // `#1;2;100;0;0` both defines colour 1 and selects it — every encoder
    // relies on that, and a decoder that only defined would draw in whatever
    // was selected before.
    const std::optional<Image> image = decode("#5;2;100;0;0~-#5~");
    REQUIRE(image.has_value());
    CHECK(at(*image, 0, 0) == kRed);
    CHECK(at(*image, 0, 6) == kRed);  // reselected without redefining
}

TEST_CASE("a sixel decoder survives hostile input", "[core][sixel]") {
    // An aborted string yields NOTHING, not a partial image: half a picture
    // painted over the user's terminal is worse than none.
    Params params;
    params.count = 0;
    SixelDecoder aborted;
    aborted.begin(params);
    for (const char ch : std::string("#1;2;100;0;0!100~")) {
        aborted.put(static_cast<std::uint8_t>(ch));
    }
    CHECK_FALSE(aborted.end(true).has_value());

    // A declared size past the bounds is refused rather than allocated.
    SixelDecoder huge;
    huge.begin(params);
    for (const char ch : std::string("\"1;1;99999;99999~")) {
        huge.put(static_cast<std::uint8_t>(ch));
    }
    CHECK(huge.overflowed());
    CHECK_FALSE(huge.end(false).has_value());

    // So is a repeat count that would grow past them, and the decoder keeps
    // CONSUMING afterwards so the parser can still find its terminator.
    SixelDecoder wide;
    wide.begin(params);
    for (const char ch : std::string("#1;2;100;0;0!999999~~~~")) {
        wide.put(static_cast<std::uint8_t>(ch));
    }
    CHECK(wide.overflowed());
    CHECK_FALSE(wide.end(false).has_value());

    // Digits long enough to overflow a 32-bit count are clamped, not wrapped —
    // a wrapped count is a small number that draws the WRONG picture.
    CHECK_FALSE(decode("#1;2;100;0;0!99999999999999999999~").has_value());

    // Empty and junk payloads produce nothing at all.
    CHECK_FALSE(decode("").has_value());
    CHECK_FALSE(decode("   \r\n").has_value());
    CHECK_FALSE(decode("#").has_value());
    CHECK_FALSE(decode("#;;;;").has_value());
    CHECK_FALSE(decode("\"").has_value());
    CHECK_FALSE(decode("!").has_value());
    CHECK_FALSE(decode("$-$-$-").has_value());

    // A colour index past the palette wraps rather than reading out of bounds.
    CHECK(decode("#999;2;100;0;0~").has_value());
    // Bytes outside the data alphabet are ignored rather than fatal: the format
    // has no escaping, and encoders wrap long lines with real newlines.
    const std::optional<Image> wrapped = decode("#1;2;100;0;0~\r\n~\r\n~");
    REQUIRE(wrapped.has_value());
    CHECK(wrapped->width == 3);
}

TEST_CASE("a trailing command with no data still completes", "[core][sixel]") {
    // `...~#3` ends mid-parameters. The select must still happen — it is legal,
    // and a decoder that dropped it would leave the state machine mid-command
    // for the next image.
    const std::optional<Image> image = decode("#1;2;100;0;0~#3");
    REQUIRE(image.has_value());
    CHECK(at(*image, 0, 0) == kRed);
}

// ---------------------------------------------------------------------------
// Through a real Session: DCS routing, the placement, and the image store.

#include "core/terminal/session.h"

namespace {

void feedAll(Session& session, std::string_view bytes) {
    session.feed({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

// A session with a cell size, which is what the placement arithmetic needs.
// Without one the core drops the image rather than guessing (session.cpp).
Session sized(int rows, int cols) {
    Session session(rows, cols);
    session.grid().cellWidthPx = 10;
    session.grid().cellHeightPx = 20;
    return session;
}

}  // namespace

TEST_CASE("a sixel arrives through DCS and lands on the grid", "[core][sixel][session]") {
    Session session = sized(10, 20);
    // 30x12 pixels at 10x20 per cell: 3 columns, 1 row (12 rounds up to 1).
    feedAll(session, "\x1bP0;0;0q\"1;1;30;12#1;2;100;0;0!30~-!30~\x1b\\");

    REQUIRE(session.grid().images.imageCount() == 1);
    REQUIRE(session.grid().images.placements().size() == 1);
    const Placement& placement = session.grid().images.placements().front();
    CHECK(placement.cols == 3);
    CHECK(placement.rows == 1);
    CHECK(placement.col == 0);

    const Image* image = session.grid().images.find(placement.imageId);
    REQUIRE(image != nullptr);
    CHECK(image->width == 30);
    CHECK(image->height == 12);

    // The cursor ends on the line BELOW the image, at column 0 — which is what
    // makes a run of sixels stack instead of overprinting each other.
    CHECK(session.grid().col == 0);
    CHECK(session.grid().row == 1);
}

TEST_CASE("DCS forms that are not sixel are left alone", "[core][sixel][session]") {
    Session session = sized(10, 20);
    // DECRQSS: '$' intermediate then 'q'. Reading it as a picture would swallow
    // a query somebody is waiting on — and produce an image out of the reply.
    feedAll(session, "\x1bP$qm\x1b\\");
    CHECK(session.grid().images.imageCount() == 0);

    // DECUDK and XTGETTCAP, for the same reason.
    feedAll(session, "\x1bP|17/0041\x1b\\");
    feedAll(session, "\x1bP+q544e\x1b\\");
    CHECK(session.grid().images.imageCount() == 0);

    // And a sixel still works afterwards, so a non-sixel DCS cannot wedge the
    // decoder for everything that follows it.
    feedAll(session, "\x1bPq#1;2;100;0;0~\x1b\\");
    CHECK(session.grid().images.imageCount() == 1);
}

TEST_CASE("an aborted sixel places nothing", "[core][sixel][session]") {
    Session session = sized(10, 20);
    feedAll(session, "\x1bPq#1;2;100;0;0!10~\x18");
    CHECK(session.grid().images.imageCount() == 0);
    CHECK(session.grid().images.placements().empty());
    // The cursor did not move either: nothing was drawn, so nothing scrolled.
    CHECK(session.grid().row == 0);
}

TEST_CASE("without a cell size the core refuses to guess", "[core][sixel][session]") {
    // A headless test or a bench run has no renderer and therefore no cell
    // size. Placing at a guessed size would put a picture in the wrong place,
    // which is worse than one that never appeared.
    Session session(10, 20);
    feedAll(session, "\x1bPq#1;2;100;0;0!10~\x1b\\");
    CHECK(session.grid().images.imageCount() == 0);
}

TEST_CASE("the image store is bounded in bytes and drops the oldest first", "[core][images]") {
    ImageStore store;

    const auto makeImage = [](int side) {
        Image image;
        image.width = side;
        image.height = side;
        image.pixels.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side),
                            0xFFFFFFFF);
        return image;
    };

    const std::uint32_t first = store.put(0, makeImage(8));
    const std::uint32_t second = store.put(0, makeImage(8));
    CHECK(first != 0);
    CHECK(second != 0);
    CHECK(first != second);  // an id of 0 means "assign one", not "reuse one"
    CHECK(store.imageCount() == 2);
    CHECK(store.byteSize() == 2 * 8 * 8 * 4);

    // A placement cannot outlive the pixels it names.
    Placement placement;
    placement.imageId = first;
    placement.cols = 1;
    placement.rows = 1;
    CHECK(store.place(placement));
    placement.imageId = 9999;
    CHECK_FALSE(store.place(placement));
    // Nor can it be zero-sized.
    placement.imageId = first;
    placement.cols = 0;
    CHECK_FALSE(store.place(placement));

    // Erasing an image takes its placements with it.
    store.erase(first);
    CHECK(store.imageCount() == 1);
    CHECK(store.placements().empty());

    // One image bigger than the whole budget is refused rather than evicting
    // everything to make room for something that still would not fit.
    //
    // Sized from the constant rather than from a round number: 4096x4096x4 is
    // EXACTLY 64 MiB and therefore fits, which is what the first draft of this
    // assertion picked and why it failed. One pixel row past the cap is the
    // smallest thing that is genuinely over it. It does allocate that much for
    // a moment — the alternative is trusting a bound nothing exercises.
    const std::size_t overCap = ImageStore::kMaxBytes / 4 + 1;
    Image enormous;
    enormous.width = static_cast<int>(overCap);
    enormous.height = 1;
    enormous.pixels.assign(overCap, 0u);
    CHECK(store.put(0, std::move(enormous)) == 0);
    CHECK(store.imageCount() == 1);  // the survivor is untouched

    // An empty image is not an image.
    CHECK(store.put(0, Image{}) == 0);
}

TEST_CASE("placements whose anchor was evicted are dropped", "[core][images]") {
    // Without this a long session accumulates placements pointing at history
    // that is gone, and the placement cap starts refusing new ones.
    ImageStore store;
    Image image;
    image.width = 1;
    image.height = 1;
    image.pixels.assign(1, 0xFFFFFFFF);
    const std::uint32_t id = store.put(0, std::move(image));

    for (std::uint64_t anchor : {std::uint64_t{5}, std::uint64_t{50}, std::uint64_t{500}}) {
        Placement placement;
        placement.imageId = id;
        placement.anchor = anchor;
        placement.cols = 1;
        placement.rows = 1;
        REQUIRE(store.place(placement));
    }
    CHECK(store.placements().size() == 3);

    store.dropAnchorsBefore(50);
    REQUIRE(store.placements().size() == 2);
    CHECK(store.placements().front().anchor == 50);
}

TEST_CASE("a repeat flood cannot overflow the active position", "[core][sixel]") {
    // Found by reading, not by the fuzzer — which has never been run against
    // this decoder (STATE.md flags that). `repeat` is capped at 2^24 by the
    // parameter parser, so a plain `m_x + repeat` carries past INT_MAX after
    // about a hundred commands: roughly a KILOBYTE of input, trivially
    // reachable from a remote host, and signed overflow is undefined
    // behaviour. rules/vt-core.md calls that a security bug, not a defect.
    Params params;
    params.count = 0;
    SixelDecoder decoder;
    decoder.begin(params);

    const std::string flood = "#1;2;100;0;0" + [] {
        std::string out;
        for (int i = 0; i < 400; ++i) {
            out += "!16777215~";
        }
        return out;
    }();
    for (const char ch : flood) {
        decoder.put(static_cast<std::uint8_t>(ch));
    }
    // The bound held and nothing was produced; the point is that we got here at
    // all, and that a sanitizer build gets here too.
    CHECK(decoder.overflowed());
    CHECK_FALSE(decoder.end(false).has_value());

    // The same for the band position, which `-` advances. It needs far more
    // input to overflow — ~360 MB — which makes it the less urgent half of the
    // same problem, not a different one.
    SixelDecoder bands;
    bands.begin(params);
    for (int i = 0; i < 200000; ++i) {
        bands.put(static_cast<std::uint8_t>('-'));
    }
    bands.put(static_cast<std::uint8_t>('~'));
    CHECK(bands.overflowed());
    CHECK_FALSE(bands.end(false).has_value());
}

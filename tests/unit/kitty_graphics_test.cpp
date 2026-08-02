#include "core/graphics/kitty.h"
#include "core/parser/osc.h"
#include "core/terminal/session.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

using namespace krait::core::vt;

namespace {

void feedAll(Session& session, std::string_view bytes) {
    session.feed({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

Session sized(int rows, int cols) {
    Session session(rows, cols);
    session.grid().cellWidthPx = 10;
    session.grid().cellHeightPx = 20;
    return session;
}

// One APC graphics escape.
std::string apc(std::string_view control, std::string_view rawPixels = {}) {
    std::string out = "\x1b_G";
    out += control;
    if (!rawPixels.empty()) {
        out += ";";
        out += encodeBase64(rawPixels);
    }
    out += "\x1b\\";
    return out;
}

// `count` RGBA pixels, all the same colour.
std::string rgba(std::size_t count, char r, char g, char b, char a) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        out += r;
        out += g;
        out += b;
        out += a;
    }
    return out;
}

}  // namespace

TEST_CASE("the control half parses every key we implement", "[core][kitty]") {
    const Command command =
        parseControl("a=T,f=24,s=4,v=3,i=7,p=2,x=1,y=2,w=3,h=4,c=5,r=6,z=-9,C=1,q=1");
    CHECK(command.action == Command::Action::TransmitAndPut);
    CHECK(command.format == Command::Format::Rgb);
    CHECK(command.width == 4);
    CHECK(command.height == 3);
    CHECK(command.id == 7);
    CHECK(command.placementId == 2);
    CHECK(command.srcX == 1);
    CHECK(command.srcY == 2);
    CHECK(command.srcW == 3);
    CHECK(command.srcH == 4);
    CHECK(command.cols == 5);
    CHECK(command.rows == 6);
    // NEGATIVE z is the feature, not an error: it draws the image UNDER the
    // text, which is what a background watermark is.
    CHECK(command.zIndex == -9);
    CHECK(command.cursorStays);
    CHECK(command.quiet == 1);
    CHECK(command.error.empty());

    // Defaults, for a command that says nothing.
    const Command bare = parseControl("");
    CHECK(bare.action == Command::Action::TransmitAndPut);
    CHECK(bare.format == Command::Format::Rgba);
    CHECK(bare.error.empty());

    // An unknown key is SKIPPED, as the spec requires — the alphabet grows, and
    // refusing a command over a key we have not heard of would break senders
    // that are entirely correct.
    const Command future = parseControl("a=T,s=2,v=2,Q=9,zzz=1");
    CHECK(future.width == 2);
    CHECK(future.error.empty());

    // A value that is not wholly a number leaves the field alone. Reading "12x"
    // as 12 is how a z-index comes to mean something the sender did not write.
    CHECK(parseControl("z=12x").zIndex == 0);
    CHECK(parseControl("s=99999999999999999999").width == 0);
}

TEST_CASE("what we decline, we decline OUT LOUD", "[core][kitty]") {
    // A silent drop is what makes an image viewer print nothing and give no
    // reason. A sender that is told falls back — which is what the protocol's
    // format negotiation exists for.
    CHECK_FALSE(parseControl("f=100").error.empty());  // PNG
    CHECK_FALSE(parseControl("o=z").error.empty());    // zlib
    CHECK_FALSE(parseControl("a=f").error.empty());    // animation frame
    CHECK_FALSE(parseControl("a=X").error.empty());    // nonsense

    // The transmission medium is the security one. `t=f` names a path on the
    // machine running the TERMINAL, so over SSH a remote host would be asking
    // this terminal to read a local file and draw it.
    for (const std::string_view medium : {"t=f", "t=t", "t=s"}) {
        const Command command = parseControl(medium);
        INFO(medium);
        CHECK_FALSE(command.error.empty());
    }
    CHECK(parseControl("t=d").error.empty());
}

TEST_CASE("replies go only to senders that asked", "[core][kitty]") {
    Command command;
    // No i= and no I=: the reply is addressed to those, and writing anyway
    // scatters `_Gi=0;OK` through the output of every program that sends an
    // image without wanting confirmation.
    CHECK(kittyReply(command).empty());

    command.id = 31;
    CHECK(kittyReply(command) == "\x1b_Gi=31;OK\x1b\\");

    command.error = "EINVAL:nope";
    CHECK(kittyReply(command) == "\x1b_Gi=31;EINVAL:nope\x1b\\");

    // q=1 suppresses the OK but not the error; q=2 suppresses both. Honoured
    // exactly, because a program that asked for silence is one whose output a
    // stray reply would corrupt.
    command.error.clear();
    command.quiet = 1;
    CHECK(kittyReply(command).empty());
    command.error = "EINVAL:nope";
    CHECK_FALSE(kittyReply(command).empty());
    command.quiet = 2;
    CHECK(kittyReply(command).empty());

    // I= without i= is answered too, and both together name both.
    Command numbered;
    numbered.number = 5;
    CHECK(kittyReply(numbered) == "\x1b_GI=5;OK\x1b\\");
    numbered.id = 9;
    CHECK(kittyReply(numbered) == "\x1b_Gi=9,I=5;OK\x1b\\");
}

TEST_CASE("an RGBA image transmits and places", "[core][kitty][session]") {
    Session session = sized(10, 20);
    std::string replies;
    session.onReply = [&replies](const std::string& text) { replies += text; };

    // 20x40 pixels at 10x20 per cell: 2 columns, 2 rows.
    feedAll(session, apc("a=T,f=32,s=20,v=40,i=1", rgba(20 * 40, '\xff', '\x00', '\x00', '\xff')));

    REQUIRE(session.grid().images.imageCount() == 1);
    const Image* image = session.grid().images.find(1);
    REQUIRE(image != nullptr);
    CHECK(image->width == 20);
    CHECK(image->height == 40);
    CHECK(image->pixels.front() == 0xFFFF0000);

    REQUIRE(session.grid().images.placements().size() == 1);
    const Placement& placement = session.grid().images.placements().front();
    CHECK(placement.imageId == 1);
    CHECK(placement.cols == 2);
    CHECK(placement.rows == 2);

    CHECK(replies == "\x1b_Gi=1;OK\x1b\\");
    // The cursor lands below the image, as it does after a sixel.
    CHECK(session.grid().row == 2);
    CHECK(session.grid().col == 0);
}

TEST_CASE("RGB is opaque, not transparent", "[core][kitty][session]") {
    // f=24 has no alpha channel. Defaulting it to zero decodes a perfectly
    // correct image and then draws absolutely nothing — a bug that reads as a
    // rendering failure rather than a decoding one.
    Session session = sized(10, 20);
    std::string pixels;
    for (int i = 0; i < 4; ++i) {
        pixels += "\x11\x22\x33";
    }
    feedAll(session, apc("a=t,f=24,s=2,v=2,i=3", pixels));
    const Image* image = session.grid().images.find(3);
    REQUIRE(image != nullptr);
    CHECK(image->pixels.front() == 0xFF112233);
}

TEST_CASE("a chunked transmission assembles", "[core][kitty][session]") {
    Session session = sized(10, 20);
    // Only the FIRST escape carries the parameters; the rest carry m= alone.
    const std::string half = rgba(2, '\x01', '\x02', '\x03', '\xff');
    feedAll(session, "\x1b_Ga=t,f=32,s=2,v=2,i=8,m=1;" + encodeBase64(half) + "\x1b\\");
    CHECK(session.grid().images.imageCount() == 0);  // nothing until the last chunk

    feedAll(session, "\x1b_Gm=0;" + encodeBase64(half) + "\x1b\\");
    const Image* image = session.grid().images.find(8);
    REQUIRE(image != nullptr);
    CHECK(image->width == 2);
    CHECK(image->height == 2);
    CHECK(image->pixels.size() == 4);
}

TEST_CASE("an aborted chunk discards the whole assembly", "[core][kitty][session]") {
    // Not just the chunk. The remaining chunks would be spliced onto a
    // truncated prefix and decode to noise that still passes every size check —
    // an image made of garbage is worse than no image.
    Session session = sized(10, 20);
    feedAll(session, "\x1b_Ga=t,f=32,s=2,v=2,i=9,m=1;" +
                         encodeBase64(rgba(2, '\x01', '\x02', '\x03', '\xff')) + "\x18");
    feedAll(session,
            "\x1b_Gm=0;" + encodeBase64(rgba(2, '\x04', '\x05', '\x06', '\xff')) + "\x1b\\");
    CHECK(session.grid().images.find(9) == nullptr);
}

TEST_CASE("a=t stores without showing, a=p shows without resending", "[core][kitty][session]") {
    Session session = sized(10, 20);
    feedAll(session, apc("a=t,f=32,s=10,v=20,i=4", rgba(10 * 20, '\x00', '\xff', '\x00', '\xff')));
    CHECK(session.grid().images.imageCount() == 1);
    CHECK(session.grid().images.placements().empty());  // stored, not shown
    CHECK(session.grid().row == 0);                     // and the cursor did not move

    feedAll(session, apc("a=p,i=4,c=1,r=1"));
    REQUIRE(session.grid().images.placements().size() == 1);
    CHECK(session.grid().images.placements().front().imageId == 4);

    // Placing the same image again is a SECOND placement, not a replacement —
    // that is what a=p is for.
    feedAll(session, apc("a=p,i=4,c=1,r=1"));
    CHECK(session.grid().images.placements().size() == 2);

    // a=p naming an image that is not here places nothing.
    feedAll(session, apc("a=p,i=999,c=1,r=1"));
    CHECK(session.grid().images.placements().size() == 2);
}

TEST_CASE("C=1 leaves the cursor alone", "[core][kitty][session]") {
    Session session = sized(10, 20);
    feedAll(session,
            apc("a=T,f=32,s=20,v=40,i=5,C=1", rgba(20 * 40, '\x00', '\x00', '\xff', '\xff')));
    CHECK(session.grid().images.placements().size() == 1);
    CHECK(session.grid().row == 0);  // a program placing several images needs this
}

TEST_CASE("deletion drops what it names", "[core][kitty][session]") {
    Session session = sized(10, 20);
    feedAll(session, apc("a=T,f=32,s=10,v=20,i=6", rgba(10 * 20, '\x11', '\x11', '\x11', '\xff')));
    REQUIRE(session.grid().images.imageCount() == 1);

    feedAll(session, apc("a=d,i=6"));
    CHECK(session.grid().images.imageCount() == 0);
    CHECK(session.grid().images.placements().empty());
}

TEST_CASE("a short or lying payload is refused, never padded", "[core][kitty][session]") {
    Session session = sized(10, 20);
    std::string replies;
    session.onReply = [&replies](const std::string& text) { replies += text; };

    // s x v says 100 pixels; four arrive. Padding with black would draw an
    // image the sender never sent, over the user's terminal.
    feedAll(session, apc("a=T,f=32,s=10,v=10,i=11", rgba(4, '\x01', '\x02', '\x03', '\xff')));
    CHECK(session.grid().images.imageCount() == 0);
    CHECK(replies.find("EINVAL") != std::string::npos);

    // Raw pixels with no size at all.
    replies.clear();
    feedAll(session, apc("a=T,f=32,i=12", rgba(4, '\x01', '\x02', '\x03', '\xff')));
    CHECK(session.grid().images.imageCount() == 0);
    CHECK(replies.find("EINVAL") != std::string::npos);

    // A payload that is not base64.
    replies.clear();
    feedAll(session, "\x1b_Ga=T,f=32,s=2,v=2,i=13;not!base64!\x1b\\");
    CHECK(session.grid().images.imageCount() == 0);
    CHECK(replies.find("EINVAL") != std::string::npos);

    // A declared size past the pixel bound.
    replies.clear();
    feedAll(session, apc("a=T,f=32,s=8000,v=8000,i=14", rgba(4, '\x01', '\x02', '\x03', '\xff')));
    CHECK(session.grid().images.imageCount() == 0);
    CHECK(replies.find("EINVAL") != std::string::npos);
}

TEST_CASE("an APC that is not kitty graphics is left entirely alone", "[core][kitty][session]") {
    // iTerm2's file protocol rides APC too, and so may things that do not exist
    // yet. Anything not beginning with 'G' must produce no image, no placement
    // and above all no REPLY — answering would corrupt a conversation between
    // the sender and something else.
    Session session = sized(10, 20);
    std::string replies;
    session.onReply = [&replies](const std::string& text) { replies += text; };

    feedAll(session, "\x1b_File=name=x;AAAA\x1b\\");
    feedAll(session, "\x1b_\x1b\\");
    feedAll(session, "\x1b_somethingelse\x1b\\");
    CHECK(session.grid().images.imageCount() == 0);
    CHECK(replies.empty());

    // SOS and PM stay ignored outright — nothing implements them, and giving
    // them a payload would mean buffering remote bytes for nobody.
    feedAll(session, "\x1bXsome sos string\x1b\\");
    feedAll(session, "\x1b^some pm string\x1b\\");
    CHECK(replies.empty());

    // And graphics still work afterwards: a foreign APC must not wedge the
    // decoder for everything that follows it.
    feedAll(session, apc("a=t,f=32,s=1,v=1,i=21", rgba(1, '\x01', '\x02', '\x03', '\xff')));
    CHECK(session.grid().images.find(21) != nullptr);
}

TEST_CASE("a graphics command may end with BEL", "[core][kitty][session]") {
    // kitty's own docs use ESC \, but every terminal accepts BEL for a string,
    // and a sender that used it would otherwise leave the parser mid-image.
    Session session = sized(10, 20);
    feedAll(session, "\x1b_Ga=t,f=32,s=1,v=1,i=22;" +
                         encodeBase64(rgba(1, '\x09', '\x08', '\x07', '\xff')) + "\x07");
    CHECK(session.grid().images.find(22) != nullptr);
}

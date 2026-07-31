#include "app/input/keymap.h"
#include "core/parser/kitty_keys.h"
#include "core/terminal/session.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

using krait::core::vt::KittyKeyboard;
using krait::core::vt::Session;
using namespace krait::app::input;

namespace {

// Feeds bytes and returns everything the terminal replied.
std::string reply(Session& session, std::string_view bytes) {
    std::string out;
    session.onReply = [&out](const std::string& text) { out += text; };
    session.feed({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    return out;
}

}  // namespace

TEST_CASE("the query answers with what is really on", "[core][kitty]") {
    Session session(24, 80);

    // Nothing negotiated yet.
    CHECK(reply(session, "\x1b[?u") == "\x1b[?0u");

    // An application asks for everything. It gets the one flag we implement,
    // and the query says so — which is exactly how the protocol expects a
    // terminal to decline: mask on the way in, tell the truth on the way out.
    reply(session, "\x1b[>31u");
    CHECK(reply(session, "\x1b[?u") == "\x1b[?1u");
    CHECK(session.grid().kittyKeys.flags == KittyKeyboard::kDisambiguate);

    // The alternative — storing 31 and reporting 31 — would promise key event
    // types and alternate keys that translateKey never sends, and an
    // application that believed it would misread every keystroke.
    CHECK((session.grid().kittyKeys.flags & KittyKeyboard::kReportEvents) == 0);
}

TEST_CASE("set modes replace, or, and clear", "[core][kitty]") {
    KittyKeyboard keys;

    keys.apply(0b1, 1);
    CHECK(keys.flags == 0b1);
    keys.apply(0b1, 3);  // clear those bits
    CHECK(keys.flags == 0);
    keys.apply(0b1, 2);  // set those bits
    CHECK(keys.flags == 0b1);

    // An unknown mode does nothing at all, rather than guessing "probably
    // replace". A no-op is something the application's own query can detect.
    keys.apply(0, 99);
    CHECK(keys.flags == 0b1);
}

TEST_CASE("the stack survives a program that loses track of it", "[core][kitty]") {
    KittyKeyboard keys;

    keys.push(1);
    CHECK(keys.flags == 1);
    CHECK(keys.depth() == 1);
    keys.pop(1);
    CHECK(keys.flags == 0);
    CHECK(keys.depth() == 0);

    // Popping an empty stack leaves the protocol OFF — the state every
    // application can read — rather than whatever was underneath.
    keys.push(1);
    keys.pop(10);
    CHECK(keys.flags == 0);
    CHECK(keys.depth() == 0);

    // Overflow discards the oldest instead of refusing, so a program that
    // pushes without popping degrades rather than wedging.
    for (std::size_t i = 0; i < KittyKeyboard::kMaxDepth + 4; ++i) {
        keys.push(1);
    }
    CHECK(keys.depth() == KittyKeyboard::kMaxDepth);
}

TEST_CASE("a bare CSI u is not ours", "[core][kitty]") {
    Session session(24, 80);
    // No private marker: it belongs to no protocol we speak, and answering
    // would be claiming one.
    CHECK(reply(session, "\x1b[u").empty());
    CHECK(reply(session, "\x1b[1;2u").empty());
    CHECK(session.grid().kittyKeys.flags == 0);
}

TEST_CASE("disambiguation only changes the keys that were ambiguous", "[input][kitty]") {
    const KeyModes off{};
    const KeyModes on{.appCursorKeys = false, .kittyFlags = 1};

    // Escape is the headline case: indistinguishable from the start of an
    // escape sequence, which is why editors guess with a timeout.
    CHECK(translateKey(Qt::Key_Escape, {}, QStringLiteral("\x1b"), on) == QByteArray("\x1b[27u"));

    // Ctrl+I and Tab are both 0x09 in legacy encoding. With the flag on they
    // are finally different bytes.
    const QByteArray ctrlI = translateKey(Qt::Key_I, Qt::ControlModifier, QStringLiteral("\t"), on);
    const QByteArray tab = translateKey(Qt::Key_Tab, {}, QStringLiteral("\t"), on);
    CHECK(ctrlI == QByteArray("\x1b[105;5u"));
    CHECK(tab == QByteArray("\t"));
    CHECK(ctrlI != tab);

    // Unmodified text-producing keys still send TEXT — with only this flag set
    // the protocol requires it, and an application receiving CSI u for every
    // letter would be unusable.
    CHECK(translateKey(Qt::Key_A, {}, QStringLiteral("a"), on) == QByteArray("a"));

    // Unmodified Enter keeps its legacy byte; a modified one does not, because
    // that is the ambiguous case.
    CHECK(translateKey(Qt::Key_Return, {}, QStringLiteral("\r"), on) == QByteArray("\r"));
    CHECK(translateKey(Qt::Key_Return, Qt::ShiftModifier, QStringLiteral("\r"), on) ==
          QByteArray("\x1b[13;2u"));
}

TEST_CASE("with the flag off nothing changes at all", "[input][kitty]") {
    const KeyModes off{};
    // The regression that matters: negotiating nothing must leave every byte
    // exactly as M1 shipped it.
    CHECK(translateKey(Qt::Key_Tab, {}, QStringLiteral("\t"), off) == QByteArray("\t"));
    CHECK(translateKey(Qt::Key_Return, Qt::ShiftModifier, QStringLiteral("\r"), off) !=
          QByteArray("\x1b[13;2u"));
    CHECK(translateKey(Qt::Key_A, {}, QStringLiteral("a"), off) == QByteArray("a"));
    CHECK(translateKey(Qt::Key_Escape, {}, QStringLiteral("\x1b"), off) != QByteArray("\x1b[27u"));
}

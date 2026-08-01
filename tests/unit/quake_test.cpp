// T74 — quake mode's decision half.
//
// WHAT THIS FILE CANNOT DO, said plainly rather than mocked into looking done:
// there is no test here for the global hotkey actually firing, for the window
// sliding, or for the drop-down taking the keyboard. RegisterHotKey needs a
// real window and a real message loop, the slide is a wall-clock animation, and
// whether Windows grants the foreground is a decision the OS makes about the
// whole desktop. A test that asserted "we called RegisterHotKey" would prove
// only that the line is still there.
//
// What IS testable is everything the OS half reads: the hotkey spelling turned
// into MOD_*/VK_* pairs, and the rectangle a drop-down lands in on a given
// screen. Both are pure functions for exactly that reason.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "quake.h"
#include <catch2/catch_test_macros.hpp>
#include <windows.h>

#include <QRect>

using krait::app::Hotkey;
using krait::app::parseHotkey;
using krait::app::quakeGeometry;

namespace {

// Every parse adds MOD_NOREPEAT, so spelling it out in each expectation would
// only obscure the part under test.
constexpr unsigned int kRepeat = MOD_NOREPEAT;

}  // namespace

TEST_CASE("quake: a plain combination becomes its Win32 pair", "[app][quake]") {
    const auto parsed = parseHotkey(QStringLiteral("Ctrl+Alt+K"));
    REQUIRE(parsed.has_value());
    CHECK(parsed->modifiers == (MOD_CONTROL | MOD_ALT | kRepeat));
    CHECK(parsed->key == 'K');
}

TEST_CASE("quake: MOD_NOREPEAT is always set", "[app][quake]") {
    // Without it, holding the combination toggles the window at the keyboard's
    // auto-repeat rate — which looks exactly like a broken build.
    const auto parsed = parseHotkey(QStringLiteral("Ctrl+Shift+F12"));
    REQUIRE(parsed.has_value());
    CHECK((parsed->modifiers & MOD_NOREPEAT) != 0U);
}

TEST_CASE("quake: every modifier word Krait writes elsewhere is understood", "[app][quake]") {
    CHECK(parseHotkey(QStringLiteral("Ctrl+A"))->modifiers == (MOD_CONTROL | kRepeat));
    CHECK(parseHotkey(QStringLiteral("Control+A"))->modifiers == (MOD_CONTROL | kRepeat));
    CHECK(parseHotkey(QStringLiteral("Alt+A"))->modifiers == (MOD_ALT | kRepeat));
    CHECK(parseHotkey(QStringLiteral("Shift+Alt+A"))->modifiers == (MOD_SHIFT | MOD_ALT | kRepeat));
    CHECK(parseHotkey(QStringLiteral("Win+A"))->modifiers == (MOD_WIN | kRepeat));
    CHECK(parseHotkey(QStringLiteral("Meta+A"))->modifiers == (MOD_WIN | kRepeat));
    // Case is not something a config file should have to get right.
    CHECK(parseHotkey(QStringLiteral("ctrl+ALT+k")) == parseHotkey(QStringLiteral("Ctrl+Alt+K")));
}

TEST_CASE("quake: the keys people actually bind a drop-down to", "[app][quake]") {
    CHECK(parseHotkey(QStringLiteral("Ctrl+`"))->key == VK_OEM_3);
    CHECK(parseHotkey(QStringLiteral("Ctrl+Backtick"))->key == VK_OEM_3);
    CHECK(parseHotkey(QStringLiteral("Ctrl+Grave"))->key == VK_OEM_3);
    CHECK(parseHotkey(QStringLiteral("F1"))->key == VK_F1);
    CHECK(parseHotkey(QStringLiteral("F12"))->key == VK_F12);
    CHECK(parseHotkey(QStringLiteral("F24"))->key == VK_F24);
    CHECK(parseHotkey(QStringLiteral("Ctrl+Space"))->key == VK_SPACE);
    CHECK(parseHotkey(QStringLiteral("Ctrl+Alt+Escape"))->key == VK_ESCAPE);
    CHECK(parseHotkey(QStringLiteral("Ctrl+Alt+Tab"))->key == VK_TAB);
    CHECK(parseHotkey(QStringLiteral("Ctrl+7"))->key == '7');
}

TEST_CASE("quake: a bare key is refused unless it is a function key", "[app][quake]") {
    // A bare letter registered SYSTEM-WIDE takes that letter away from every
    // other program on the machine, and nothing in the settings page would
    // explain what had happened.
    CHECK_FALSE(parseHotkey(QStringLiteral("K")).has_value());
    CHECK_FALSE(parseHotkey(QStringLiteral("7")).has_value());
    CHECK_FALSE(parseHotkey(QStringLiteral("`")).has_value());
    CHECK_FALSE(parseHotkey(QStringLiteral("Space")).has_value());
    // Function keys are the exception: nobody types F9 into a document, and a
    // bare function key is the conventional drop-down binding.
    CHECK(parseHotkey(QStringLiteral("F9")).has_value());
}

TEST_CASE("quake: nonsense is refused rather than half-registered", "[app][quake]") {
    CHECK_FALSE(parseHotkey(QString{}).has_value());
    CHECK_FALSE(parseHotkey(QStringLiteral("   ")).has_value());
    CHECK_FALSE(parseHotkey(QStringLiteral("Ctrl")).has_value());         // no key at all
    CHECK_FALSE(parseHotkey(QStringLiteral("Ctrl+Alt")).has_value());     // still no key
    CHECK_FALSE(parseHotkey(QStringLiteral("Ctrl+A+B")).has_value());     // two keys
    CHECK_FALSE(parseHotkey(QStringLiteral("Ctrl+Banana")).has_value());  // no such key
    CHECK_FALSE(parseHotkey(QStringLiteral("Ctrl+F0")).has_value());      // no F0
    CHECK_FALSE(parseHotkey(QStringLiteral("Ctrl+F25")).has_value());     // no F25
    CHECK_FALSE(parseHotkey(QStringLiteral("Hyper+A")).has_value());      // not a modifier
}

// --- where the drop-down lands ----------------------------------------------

TEST_CASE("quake: the drop-down spans the top of the usable area", "[app][quake]") {
    // availableGeometry, so a taskbar at the top is already excluded — the
    // window starts where the desktop does, not where the screen does.
    const QRect screen{0, 40, 1920, 1040};
    const QRect where = quakeGeometry(screen, 50);
    CHECK(where.x() == 0);
    CHECK(where.y() == 40);
    CHECK(where.width() == 1920);
    CHECK(where.height() == 520);
}

TEST_CASE("quake: a monitor left of or above the primary one has negative coordinates",
          "[app][quake]") {
    // THE multi-monitor bug this function exists to not have. A screen placed
    // to the left of the primary starts at a negative x, and anything that
    // assumed 0 would open the drop-down on the wrong monitor.
    const QRect leftOfPrimary{-1280, -200, 1280, 1024};
    const QRect where = quakeGeometry(leftOfPrimary, 45);
    CHECK(where.x() == -1280);
    CHECK(where.y() == -200);
    CHECK(where.width() == 1280);
    CHECK(where.height() == 460);
}

TEST_CASE("quake: the height percentage is clamped rather than obeyed", "[app][quake]") {
    const QRect screen{0, 0, 1000, 1000};
    // A hand-edited config can hold anything, and a zero-height window is one
    // nobody can find again.
    CHECK(quakeGeometry(screen, 0).height() == 100);
    CHECK(quakeGeometry(screen, -50).height() == 100);
    CHECK(quakeGeometry(screen, 500).height() == 1000);
    CHECK(quakeGeometry(screen, 100).height() == 1000);
}

TEST_CASE("quake: a short screen still gets a window with height in it", "[app][quake]") {
    // Integer division at 10% of a very short work area rounds toward zero.
    const QRect tiny{0, 0, 640, 5};
    CHECK(quakeGeometry(tiny, 10).height() >= 1);
}

TEST_CASE("quake: geometry is device-independent pixels in and out", "[app][quake]") {
    // Nothing here multiplies by a device pixel ratio, and that is the point:
    // QScreen::availableGeometry() and QWindow::setGeometry() are both in
    // device-independent pixels, so a 200% monitor reports 1920x1080 of them
    // and the drop-down covers the same FRACTION of it as on a 100% one.
    const QRect logical{0, 0, 1920, 1080};
    CHECK(quakeGeometry(logical, 45).height() == 486);
    CHECK(quakeGeometry(logical, 45).width() == 1920);
}

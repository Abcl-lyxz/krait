// T57: the hexdump view and the session log.
//
// Both exist to answer "what did the device ACTUALLY send", so the properties
// worth asserting are the ones that would quietly destroy that answer: an
// offset column that restarts, a text column that decodes UTF-8, a log that
// cannot tell input from output, or one that buffers the tail it was started
// to capture.

#include "capture.h"
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <string>

using krait::app::formatHexdump;
using krait::app::SessionLog;
using krait::app::sessionLogPath;

TEST_CASE("a hexdump line has offset, hex and text", "[capture]") {
    const std::string dump = formatHexdump(QByteArray("hello"), 0);
    CHECK(dump == "00000000  68 65 6c 6c 6f                                    |hello|\r\n");
}

TEST_CASE("the offset continues across reads", "[capture]") {
    // The whole point of the column. Reads arrive at the mercy of TCP, so an
    // offset that restarted every read could not be matched against a packet
    // capture — which is the reason someone turned the hexdump on.
    const std::string second = formatHexdump(QByteArray("x"), 0x10);
    CHECK(second.starts_with("00000010  78 "));
}

TEST_CASE("sixteen bytes per line, and the text column stays aligned", "[capture]") {
    QByteArray seventeen;
    for (int i = 0; i < 17; ++i) {
        seventeen.append(static_cast<char>('a' + i));
    }
    const std::string dump = formatHexdump(seventeen, 0);
    const std::size_t firstEnd = dump.find("\r\n");
    REQUIRE(firstEnd != std::string::npos);
    // The second line holds one byte and must pad its hex column, or the text
    // column of a short final line lands somewhere different from every line
    // above it.
    const std::string secondLine = dump.substr(firstEnd + 2);
    CHECK(secondLine.starts_with("00000010  71 "));
    CHECK(secondLine.find("|q|") != std::string::npos);
    CHECK(dump.substr(0, firstEnd).find("|abcdefghijklmnop|") != std::string::npos);
}

TEST_CASE("the text column does not decode anything", "[capture]") {
    // A hexdump that rendered UTF-8 would hide exactly the bytes being looked
    // for. Everything outside printable ASCII is a dot, including valid
    // continuation bytes and DEL.
    const QByteArray bytes = QByteArray("a\xc3\xa9\x1b\x7f", 5);
    const std::string dump = formatHexdump(bytes, 0);
    CHECK(dump.find("|a....|") != std::string::npos);
    CHECK(dump.find("c3 a9") != std::string::npos);
}

TEST_CASE("an empty read produces no line at all", "[capture]") {
    CHECK(formatHexdump(QByteArray(), 0).empty());
}

TEST_CASE("a log records both directions and survives a crash", "[capture]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("session.log");

    SessionLog log;
    REQUIRE(log.open(path));
    log.writeInput(QByteArray("ls\r"));
    log.writeOutput(QByteArray("total 0\r\n"));

    // Read WITHOUT closing: the log exists to survive whatever is about to
    // happen to the session, and a buffered tail is exactly the part that gets
    // lost when it does.
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(file.readAll());
    CHECK(text.contains(QStringLiteral("> ls")));
    CHECK(text.contains(QStringLiteral("< total 0")));
}

TEST_CASE("control bytes are escaped rather than written raw", "[capture]") {
    // A log full of raw escape sequences reformats itself the moment anyone
    // opens it — and the escapes are usually the interesting part.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("esc.log");

    SessionLog log;
    REQUIRE(log.open(path));
    log.writeOutput(QByteArray("\x1b[31mred\x1b[0m", 12));

    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(file.readAll());
    CHECK(text.contains(QStringLiteral("\\x1b[31mred")));
    CHECK_FALSE(text.contains(QChar(0x1b)));
}

TEST_CASE("a log path cannot escape its directory", "[capture]") {
    // The name comes from a profile, which comes from a TOML file or an
    // importer. A session called "../../etc/passwd" must not produce a path
    // outside the logs directory.
    const QString path = sessionLogPath("C:/cfg", "../../evil");
    CHECK(path.contains(QStringLiteral("/logs/")));
    CHECK_FALSE(path.contains(QStringLiteral("..")));

    // And a name that slugifies to nothing still produces a usable file.
    const QString thai = sessionLogPath("C:/cfg", QString::fromUtf8("เซิร์ฟเวอร์"));
    CHECK(thai.contains(QStringLiteral("/logs/session-")));
    CHECK(thai.endsWith(QStringLiteral(".log")));
}

TEST_CASE("a log that cannot be opened says so instead of pretending", "[capture]") {
    SessionLog log;
    // A path whose parent cannot be created.
    CHECK_FALSE(log.open(QStringLiteral("Z:/nope/does/not/exist/x.log")));
    CHECK_FALSE(log.isOpen());
    CHECK_FALSE(log.error().isEmpty());
    // Writing to a closed log is a no-op, not a crash: the toggle is a user
    // action and the failure path has to be survivable.
    log.writeOutput(QByteArray("ignored"));
}

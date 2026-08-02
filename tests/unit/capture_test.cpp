// T57: the hexdump view and the session log.
//
// Both exist to answer "what did the device ACTUALLY send", so the properties
// worth asserting are the ones that would quietly destroy that answer: an
// offset column that restarts, a text column that decodes UTF-8, a log that
// cannot tell input from output, or one that buffers the tail it was started
// to capture.

#include "capture.h"
#include <catch2/catch_test_macros.hpp>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <string>

using krait::app::expandLogPath;
using krait::app::formatHexdump;
using krait::app::LogFormat;
using krait::app::SessionLog;

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

namespace {

// A fixed instant, so a substitution test asserts on the stamp rather than on
// whatever the clock said while it ran.
QDateTime when() {
    return QDateTime(QDate(2026, 8, 1), QTime(19, 30, 5));
}

QString expand(const QString& tmpl, const QString& session, const QString& host = {}) {
    return expandLogPath("C:/cfg", tmpl, {.session = session, .host = host}, when());
}

}  // namespace

TEST_CASE("the default template reproduces what T57 wrote", "[capture]") {
    // The setting existing must change nothing until someone edits it.
    const QString path = expand("", "prod eu");
    CHECK(path == QStringLiteral("C:/cfg/logs/prod-eu-20260801-193005.log"));
}

TEST_CASE("a substituted value cannot escape its directory", "[capture]") {
    // The value comes from a profile, which comes from a TOML file or an
    // importer, and {host} is remote-influenced besides. A session called
    // "../../etc/passwd" must not choose where the file lands.
    const QString path = expand("logs/{session}.log", "../../evil");
    CHECK(path == QStringLiteral("C:/cfg/logs/evil.log"));
    CHECK_FALSE(path.contains(QStringLiteral("..")));

    // Separators, drive letters and the Win32-illegal set all go the same way.
    CHECK(expand("logs/{host}.log", "", "a\\b:c*d?e|f") ==
          QStringLiteral("C:/cfg/logs/a-b-c-d-e-f.log"));

    // A name that slugifies to nothing still produces a usable file rather than
    // an empty component.
    const QString thai = expand("logs/{session}.log", QString::fromUtf8("เซิร์ฟเวอร์"));
    CHECK(thai == QStringLiteral("C:/cfg/logs/session.log"));
}

TEST_CASE("a DOS device name is not a usable file name", "[capture]") {
    // The one thing slugify cannot know about: "con" survives it unchanged, and
    // CON is still a device. Opening it would succeed and write to nothing.
    CHECK(expand("logs/{session}.log", "CON") == QStringLiteral("C:/cfg/logs/con-log.log"));
    CHECK(expand("logs/{host}.log", "", "lpt1") == QStringLiteral("C:/cfg/logs/lpt1-log.log"));
    // A name that merely CONTAINS one is fine — only the whole stem is reserved.
    CHECK(expand("logs/{session}.log", "console") == QStringLiteral("C:/cfg/logs/console.log"));
}

TEST_CASE("separators in the template are the user's own choice", "[capture]") {
    // The template is trusted — the user typed it into their own settings — and
    // a separator in it is how they say "one folder per host". Only the
    // SUBSTITUTED values are hostile.
    CHECK(expand("logs/{host}/{date}.log", "s", "web1.prod") ==
          QStringLiteral("C:/cfg/logs/web1-prod/20260801.log"));
    // A host with no value at all still names something.
    CHECK(expand("logs/{host}.log", "s", "") == QStringLiteral("C:/cfg/logs/none.log"));
}

TEST_CASE("an unknown placeholder is left where it can be seen", "[capture]") {
    // Silently dropping it would leave the user staring at a file name with a
    // hole in it and no idea why; leaving it shows the typo.
    CHECK(expand("logs/{hostname}.log", "s").endsWith(QStringLiteral("{hostname}.log")));
}

TEST_CASE("the raw format is byte-exact and carries no input", "[capture]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("raw.log");

    SessionLog log;
    REQUIRE(log.open(path, LogFormat::Raw));
    log.writeOutput(QByteArray("\x1b[31mred\x1b[0m", 12));
    log.writeInput(QByteArray("secret"));

    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QByteArray text = file.readAll();
    // Byte for byte, no header, no timestamps: the file replays.
    CHECK(text == QByteArray("\x1b[31mred\x1b[0m", 12));
    // And input is not in it, whatever the include-input setting says — a
    // byte-exact stream has no lane to put a direction marker in.
    CHECK_FALSE(text.contains("secret"));
}

TEST_CASE("the text format strips the escape sequences out", "[capture]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("text.log");

    SessionLog log;
    REQUIRE(log.open(path, LogFormat::Text));
    log.writeOutput(QByteArray("\x1b[31mred\x1b[0m\r\n", 14));
    // Split across two writes, mid-sequence. A stripper that restarted at
    // Ground on every read would put "1mbait" into the file — the far end picks
    // where its writes are cut.
    log.writeOutput(QByteArray("\x1b[3", 3));
    log.writeOutput(QByteArray("1mbait\r\n", 8));

    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(file.readAll());
    CHECK(text.contains(QStringLiteral("red")));
    CHECK(text.contains(QStringLiteral("bait")));
    CHECK_FALSE(text.contains(QStringLiteral("[31m")));
    CHECK_FALSE(text.contains(QStringLiteral("1mbait")));
    CHECK_FALSE(text.contains(QChar(0x1b)));
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

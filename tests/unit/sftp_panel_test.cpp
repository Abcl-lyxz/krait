// T65 — the SFTP panel's view-model.
//
// WHAT THIS FILE COVERS. The panel is QML and is not test-driven here; what is
// tested is the C++ underneath it, which is where every decision lives: whether
// a name may be used to compose a path, how a remote path is joined and walked
// up, and which reply belongs to which question.
//
// The one that matters most is the first block. sftp.cpp already drops names
// carrying separators or control characters out of a LISTING, but the panel
// composes a LOCAL destination path out of those names — so a check that lives
// only in the producer protects nothing the day the panel is handed a name from
// anywhere else. These cases are that check, at the point of use.

#include "sftp_model.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QSignalSpy>

using krait::app::isSafeLeafName;
using krait::app::remoteJoin;
using krait::app::remoteParent;
using krait::app::SftpModel;
using krait::app::SftpRequests;

TEST_CASE("a name that could steer a path is refused", "[sftp][panel]") {
    // The whole point: a server answering readdir with any of these must not
    // get to choose where a download lands.
    CHECK_FALSE(isSafeLeafName(QStringLiteral("../evil")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral("..\\..\\Startup\\evil.lnk")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral("sub/dir")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral("sub\\dir")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral("C:evil")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral("notes.txt:hidden")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral("..")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral(".")));
    CHECK_FALSE(isSafeLeafName(QString()));

    // A control character does not survive a terminal either — a name with a
    // CR in it rewrites the line it is printed on.
    CHECK_FALSE(isSafeLeafName(QStringLiteral("evil\r.txt")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral("evil\nrm -rf")));
    CHECK_FALSE(isSafeLeafName(QString(QChar(0x1b)) + QStringLiteral("[2J")));

    // Win32 strips these, so the file that opens is not the one the listing
    // showed.
    CHECK_FALSE(isSafeLeafName(QStringLiteral("report.txt.")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral("report.txt ")));

    // Device names still resolve to devices in any directory, with any
    // extension. A download written to one is discarded and reported as done.
    CHECK_FALSE(isSafeLeafName(QStringLiteral("NUL")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral("nul")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral("nul.txt")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral("COM1")));
    CHECK_FALSE(isSafeLeafName(QStringLiteral("lpt9.log")));

    // And the ordinary names still work, including the ones that merely look
    // alarming. A guard that refuses real files is a guard people route around.
    CHECK(isSafeLeafName(QStringLiteral("notes.txt")));
    CHECK(isSafeLeafName(QStringLiteral(".bashrc")));
    CHECK(isSafeLeafName(QStringLiteral("..bashrc")));
    CHECK(isSafeLeafName(QStringLiteral("console.log")));
    CHECK(isSafeLeafName(QStringLiteral("com10")));
    CHECK(isSafeLeafName(QStringLiteral("a b c.tar.gz")));
    CHECK(isSafeLeafName(QString::fromUtf8("รายงาน.txt")));
}

TEST_CASE("a hostile name never moves the local pane", "[sftp][panel]") {
    // The guard where the panel actually uses it. No backend is needed: walking
    // the LOCAL side is done here, so this is the whole path from a name the
    // server chose to a directory this process would read.
    SftpModel model;
    QSignalSpy errors(&model, &SftpModel::errorRaised);
    const QString before = model.localPath();
    REQUIRE_FALSE(before.isEmpty());

    model.enterLocal(QStringLiteral("..\\..\\Windows"));
    CHECK(model.localPath() == before);
    model.enterLocal(QStringLiteral(".."));
    CHECK(model.localPath() == before);
    model.enterLocal(QStringLiteral("C:evil"));
    CHECK(model.localPath() == before);
    // Refusals are said out loud, not swallowed: a pane that ignores a
    // double-click looks broken.
    CHECK(errors.count() == 3);
}

TEST_CASE("a session with no SFTP is inert rather than absent", "[sftp][panel]") {
    // A local shell tab still has a model — TerminalItem builds one for every
    // terminal — and every method has to be safe to call on it.
    SftpModel model;
    CHECK_FALSE(model.available());
    CHECK(model.remotePath().isEmpty());
    CHECK_FALSE(model.busy());

    model.start();
    model.refreshRemote();
    model.leaveRemote();
    model.enterRemote(QStringLiteral("etc"));
    model.download(QStringLiteral("notes.txt"));
    model.upload(QStringLiteral("notes.txt"));
    model.uploadUrls({});
    model.cancel();

    CHECK(model.remotePath().isEmpty());
    CHECK(model.remoteEntries().isEmpty());
    // start() still fills the local side: the panel is half useful without a
    // server, and an empty pane would look like a failure.
    CHECK_FALSE(model.localPath().isEmpty());
}

TEST_CASE("remote paths are joined and walked as POSIX", "[sftp][panel]") {
    // SFTP has one separator and it is not the platform's. QDir would hand back
    // a backslash here and the server would treat it as part of the name.
    CHECK(remoteJoin(QStringLiteral("/home/kla"), QStringLiteral("notes.txt")) ==
          QStringLiteral("/home/kla/notes.txt"));
    CHECK(remoteJoin(QStringLiteral("/"), QStringLiteral("etc")) == QStringLiteral("/etc"));
    CHECK(remoteJoin(QString(), QStringLiteral("etc")) == QStringLiteral("/etc"));

    CHECK(remoteParent(QStringLiteral("/home/kla/src")) == QStringLiteral("/home/kla"));
    CHECK(remoteParent(QStringLiteral("/home")) == QStringLiteral("/"));
    // The root is its own parent. An up button that walks off the top leaves a
    // pane that can only be recovered by typing a path.
    CHECK(remoteParent(QStringLiteral("/")) == QStringLiteral("/"));
    CHECK(remoteParent(QStringLiteral("home")) == QStringLiteral("/"));
}

TEST_CASE("a request id is never reused", "[sftp][panel]") {
    SftpRequests open;
    CHECK(open.empty());

    const quint64 first = open.add(SftpRequests::Kind::List, QStringLiteral("/etc"));
    const quint64 second = open.add(SftpRequests::Kind::Download, QStringLiteral("notes.txt"));
    CHECK(first != second);
    CHECK_FALSE(open.empty());

    REQUIRE(open.find(second) != nullptr);
    CHECK(open.find(second)->kind == SftpRequests::Kind::Download);
    CHECK(open.find(second)->subject == QStringLiteral("notes.txt"));

    const auto taken = open.take(first);
    REQUIRE(taken.has_value());
    CHECK(taken->kind == SftpRequests::Kind::List);
    // Exactly one sftpFinished per request, so a second take must find nothing
    // rather than replay the first answer.
    CHECK_FALSE(open.take(first).has_value());
    CHECK(open.find(first) == nullptr);

    // The reason ids keep counting past a clear(): a queued emission from the
    // session this panel used to be pointed at is already posted to it, and
    // disconnect() cannot reach it. Reusing an id would let that stale reply be
    // read as an answer to a question about a different server.
    open.clear();
    CHECK(open.empty());
    const quint64 third = open.add(SftpRequests::Kind::Upload, QStringLiteral("notes.txt"));
    CHECK(third != first);
    CHECK(third != second);
    CHECK_FALSE(open.take(first).has_value());
    CHECK_FALSE(open.take(second).has_value());
}

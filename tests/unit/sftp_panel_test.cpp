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
#include "shell_integration.h"
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QString>

using krait::app::blockState;
using krait::app::BlockState;
using krait::app::isSafeLeafName;
using krait::app::isShellExecutableName;
using krait::app::kBlockBegin;
using krait::app::kBlockEnd;
using krait::app::remoteJoin;
using krait::app::remoteParent;
using krait::app::scriptPath;
using krait::app::SftpModel;
using krait::app::SftpRequests;
using krait::app::shellTargetFor;
using krait::app::shellTargets;
using krait::app::spliceBlock;

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

TEST_CASE("a name that would RUN instead of open is refused by Edit", "[sftp][panel]") {
    // isSafeLeafName stops a server-chosen name from steering a path. This
    // stops it from choosing a program: the editor round-trip's default is the
    // OS association, and asking Windows to open a .exe is asking it to run
    // one — out of a temp file this process wrote, so with no
    // mark-of-the-web either.
    CHECK(isShellExecutableName(QStringLiteral("update.exe")));
    CHECK(isShellExecutableName(QStringLiteral("setup.EXE")));
    CHECK(isShellExecutableName(QStringLiteral("go.bat")));
    CHECK(isShellExecutableName(QStringLiteral("go.cmd")));
    CHECK(isShellExecutableName(QStringLiteral("run.com")));
    // The indirections and the script hosts, none of which look like programs.
    CHECK(isShellExecutableName(QStringLiteral("notes.hta")));
    CHECK(isShellExecutableName(QStringLiteral("readme.lnk")));
    CHECK(isShellExecutableName(QStringLiteral("patch.reg")));
    CHECK(isShellExecutableName(QStringLiteral("saver.scr")));
    CHECK(isShellExecutableName(QStringLiteral("pkg.msi")));
    CHECK(isShellExecutableName(QStringLiteral("invoice.vbs")));
    CHECK(isShellExecutableName(QStringLiteral("invoice.js")));
    // The double extension a hostile listing uses to look harmless: the LAST
    // one is what Windows acts on, so that is the one this reads.
    CHECK(isShellExecutableName(QStringLiteral("notes.txt.exe")));

    // And the files people actually edit still open. A guard that refuses real
    // work is a guard people route around by setting editor.command to
    // something worse.
    //
    // `main.py` is the reason this is a fixed list rather than %PATHEXT%:
    // installing Python puts .PY in PATHEXT, so reading it would refuse this
    // on a developer's machine and allow it on the build server.
    CHECK_FALSE(isShellExecutableName(QStringLiteral("nginx.conf")));
    CHECK_FALSE(isShellExecutableName(QStringLiteral("deploy.sh")));
    CHECK_FALSE(isShellExecutableName(QStringLiteral("main.py")));
    CHECK_FALSE(isShellExecutableName(QStringLiteral("app.rb")));
    CHECK_FALSE(isShellExecutableName(QStringLiteral("values.yaml")));
    CHECK_FALSE(isShellExecutableName(QStringLiteral(".bashrc")));  // dot, no extension after
    CHECK_FALSE(isShellExecutableName(QStringLiteral("Makefile")));
    CHECK_FALSE(isShellExecutableName(QString::fromUtf8("รายงาน.txt")));
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

// --- T73, the shell-integration block --------------------------------------
//
// This is the "never clobber" half of the SSH auto-install, and it is the half
// that can be tested without a server: everything the installer decides about
// somebody else's rc file happens in these three functions.

namespace {

QString block(const QString& body) {
    return kBlockBegin + QStringLiteral("\n") + body + QStringLiteral("\n") + kBlockEnd;
}

QString readAsset(const QString& name) {
    QFile file(QStringLiteral(KRAIT_ASSETS_DIR "/shell-integration/") + name);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return QString::fromUtf8(file.readAll());
}

}  // namespace

TEST_CASE("a block is recognised, and a half-deleted one is refused", "[sftp][shell]") {
    CHECK(blockState(QString()) == BlockState::Absent);
    CHECK(blockState(QStringLiteral("export PATH=/usr/bin\n")) == BlockState::Absent);
    CHECK(blockState(block(QStringLiteral("echo hi"))) == BlockState::Present);

    // A marker is a whole line. A file that TALKS about the block — a comment
    // quoting it, a README pasted into an rc file — must not be read as one, or
    // the installer would replace everything between two sentences.
    CHECK(blockState(QStringLiteral("# see ") + kBlockBegin + QStringLiteral(" for details\n")) ==
          BlockState::Absent);

    // The cases where there is no single span to replace. Guessing an end here
    // truncates a file on a machine we are a guest on, so the installer says so
    // and leaves it alone.
    CHECK(blockState(kBlockBegin + QStringLiteral("\necho hi\n")) == BlockState::Damaged);
    CHECK(blockState(QStringLiteral("echo hi\n") + kBlockEnd) == BlockState::Damaged);
    CHECK(blockState(kBlockEnd + QStringLiteral("\n") + kBlockBegin) == BlockState::Damaged);
    CHECK(blockState(block(QStringLiteral("a")) + QStringLiteral("\n") +
                     block(QStringLiteral("b"))) == BlockState::Damaged);
}

TEST_CASE("splicing leaves everything outside the markers byte for byte", "[sftp][shell]") {
    // The whole point of reading the file, editing it here and writing it back
    // rather than appending through the shell: what comes out has to be what
    // went in, plus exactly one block.
    const QString before = QStringLiteral("# my prompt\nexport PS1='$ '\n");
    const QString after = spliceBlock(before, QStringLiteral("echo hi\n"));
    CHECK(after.startsWith(before));
    CHECK(blockState(after) == BlockState::Present);
    CHECK(after.endsWith(kBlockEnd + QStringLiteral("\n")));

    // Idempotent: installing twice REPLACES, and the file does not grow a
    // second copy or a stray blank line.
    CHECK(spliceBlock(after, QStringLiteral("echo hi\n")) == after);

    // Replacing changes only the body.
    const QString updated = spliceBlock(after, QStringLiteral("echo bye\n"));
    CHECK(updated.startsWith(before));
    CHECK(updated.contains(QStringLiteral("echo bye")));
    CHECK_FALSE(updated.contains(QStringLiteral("echo hi")));

    // Removal is exact — the file comes back to what it was before anyone
    // installed anything, which is what makes uninstalling safe to offer.
    CHECK(spliceBlock(updated, QString()) == before);
    CHECK(spliceBlock(before, QString()) == before);
}

TEST_CASE("splicing survives an rc file's line endings", "[sftp][shell]") {
    // A file with no trailing newline gets the one it was missing, because
    // there is no way to append after an unterminated line — and the line
    // itself is not touched.
    const QString unterminated = QStringLiteral("export A=1");
    const QString after = spliceBlock(unterminated, QStringLiteral("echo hi\n"));
    CHECK(after.startsWith(QStringLiteral("export A=1\n")));
    CHECK(blockState(after) == BlockState::Present);

    // A CRLF file keeps its CRLFs on the lines it already had. Rewriting them
    // would be an edit nobody asked for, in a file that belongs to someone
    // else.
    const QString crlf = QStringLiteral("export A=1\r\nexport B=2\r\n");
    CHECK(spliceBlock(crlf, QStringLiteral("echo hi\n")).startsWith(crlf));
}

TEST_CASE("every shell target names a script that ships", "[sftp][shell]") {
    // A target whose script is missing is an install that offers a shell and
    // then fails at the last step, after the user has already said yes.
    for (const krait::app::ShellTarget& target : shellTargets()) {
        CAPTURE(target.rc.toStdString());
        CHECK(shellTargetFor(target.rc) != nullptr);
        CHECK_FALSE(target.rc.startsWith(u'/'));  // always relative to the login dir
        const QString text = readAsset(target.script);
        CHECK_FALSE(text.isEmpty());
    }
    CHECK(shellTargetFor(QStringLiteral(".nonesuch")) == nullptr);
    // scriptPath answers about files beside the EXE, and a test binary has none
    // — which is the honest answer rather than a path that does not exist.
    CHECK(scriptPath(QStringLiteral("krait.bash")).isEmpty());
}

TEST_CASE("every shipped script marks a prompt and reports an exit status", "[sftp][shell]") {
    // The regression this catches is silent: a script that stops emitting D
    // still gives a working prompt, and the only symptom is that Krait never
    // colours a failure again. Checked against what osc.cpp actually reads —
    // A for the prompt, D for the end — rather than against the spec at large.
    struct Case {
        const char* file;
        const char* guard;
    };

    for (const Case& shell : {Case{"krait.bash", "KRAIT_SHELL_INTEGRATION"},
                              Case{"krait.zsh", "KRAIT_SHELL_INTEGRATION"},
                              Case{"krait.fish", "KRAIT_SHELL_INTEGRATION"},
                              Case{"krait.ps1", "__KraitShellIntegration"}}) {
        CAPTURE(shell.file);
        const QString text = readAsset(QString::fromUtf8(shell.file));
        CHECK(text.contains(QStringLiteral("133;A")));
        CHECK(text.contains(QStringLiteral("133;D")));
        // Sourcing twice must not double the marks, so every script guards on
        // something it sets itself.
        CHECK(text.contains(QString::fromUtf8(shell.guard)));
        // None of them may carry the markers: the installer wraps the file in
        // them, and a script that already contained one would produce a block
        // whose end is in the middle.
        CHECK_FALSE(text.contains(kBlockBegin));
        CHECK_FALSE(text.contains(kBlockEnd));
    }

    // C arms the long-command notification. bash, zsh and fish have a hook for
    // it; PowerShell does not (see krait.ps1), and claiming otherwise here
    // would make this test lie about a gap that is documented.
    for (const char* file : {"krait.bash", "krait.zsh", "krait.fish"}) {
        CAPTURE(file);
        CHECK(readAsset(QString::fromUtf8(file)).contains(QStringLiteral("133;C")));
    }

    // THE EXIT STATUS ORDERING, which is the whole reason D exists and the one
    // regression that would be invisible: only the FIRST prompt hook still sees
    // the command's $?, so both scripts have to put themselves at the front of
    // a list the user's prompt framework is already in. Appending compiles,
    // runs, and reports oh-my-zsh's exit status forever.
    const QString bash = readAsset(QStringLiteral("krait.bash"));
    CHECK(bash.contains(QStringLiteral("PROMPT_COMMAND=$'__krait_command_end\\n'$PROMPT_COMMAND")));
    CHECK_FALSE(bash.contains(QStringLiteral("PROMPT_COMMAND+=")));
    CHECK(readAsset(QStringLiteral("krait.zsh"))
              .contains(QStringLiteral("precmd_functions=(__krait_precmd")));
}

// T86 — the parts of crash handling that can be tested at all.
//
// Deliberately NOT tested here: install(), and anything that writes a dump. A
// crash handler runs in a process that is already dying, so exercising it means
// crashing the test binary — and a test that has to crash to pass cannot report
// that it did. What IS testable is the arithmetic the handler does before it
// touches the disk, which is exactly where a mistake would be silent: a wrong
// name, or a prune that deletes the dump it just wrote.

#include "app/crash/crash_handler.h"
#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <cstddef>
#include <utility>
#include <vector>

namespace crash = krait::app::crash;

TEST_CASE("crash: the dump type carries no heap and no globals", "[app][crash][t86]") {
    // The flags are a SECURITY choice, so they are pinned rather than trusted
    // to stay right: Krait holds SSH passwords in memory, and any of these
    // would put them in a file on disk.
    constexpr unsigned long kWithDataSegs = 0x1;                     // globals
    constexpr unsigned long kWithIndirectlyReferencedMemory = 0x40;  // heap via locals
    constexpr unsigned long kWithFullMemory = 0x2;                   // everything
    constexpr unsigned long kWithPrivateReadWriteMemory = 0x200;     // the heap
    constexpr unsigned long kWithPrivateWriteCopyMemory = 0x10000;   // ditto
    constexpr unsigned long kWithTokenInformation = 0x40000;         // identity

    CHECK((crash::kDumpType & kWithDataSegs) == 0);
    CHECK((crash::kDumpType & kWithIndirectlyReferencedMemory) == 0);
    CHECK((crash::kDumpType & kWithFullMemory) == 0);
    CHECK((crash::kDumpType & kWithPrivateReadWriteMemory) == 0);
    CHECK((crash::kDumpType & kWithPrivateWriteCopyMemory) == 0);
    CHECK((crash::kDumpType & kWithTokenInformation) == 0);

    // ...and it does carry the two that make a dump readable.
    CHECK((crash::kDumpType & 0x1000) != 0);  // MiniDumpWithThreadInfo
    CHECK((crash::kDumpType & 0x20) != 0);    // MiniDumpWithUnloadedModules
}

TEST_CASE("crash: dump names sort chronologically", "[app][crash][t86]") {
    const QString early = crash::dumpFileName(QStringLiteral("20260102-030405"), 1234);
    const QString late = crash::dumpFileName(QStringLiteral("20260102-030406"), 99);

    CHECK(early == QStringLiteral("krait-20260102-030405-1234.dmp"));
    // A plain string compare has to order these, because that is what the prune
    // relies on rather than asking the filesystem for timestamps — which a
    // crashing process has no business doing.
    CHECK(early < late);
    // The pid does not get to decide the order.
    CHECK(late > early);
}

TEST_CASE("crash: pruning keeps the newest and leaves room for one more", "[app][crash][t86]") {
    std::vector<QString> existing;
    for (int i = 1; i <= 8; ++i) {
        existing.push_back(crash::dumpFileName(QStringLiteral("2026010%1-000000").arg(i), 100));
    }
    // Deliberately out of order: entryList sorts, but this must not depend on
    // its caller having done so.
    std::swap(existing[0], existing[7]);

    const std::vector<QString> pruned = crash::dumpsToPrune(existing);

    // 8 present, room for the one about to be written, cap 5 -> keep 4, drop 4.
    REQUIRE(pruned.size() == 4);
    CHECK(pruned[0] == crash::dumpFileName(QStringLiteral("20260101-000000"), 100));
    CHECK(pruned[3] == crash::dumpFileName(QStringLiteral("20260104-000000"), 100));
    // The newest survive.
    for (const QString& name : pruned) {
        CHECK(name < crash::dumpFileName(QStringLiteral("20260105-000000"), 100));
    }
}

TEST_CASE("crash: a directory under the cap prunes nothing", "[app][crash][t86]") {
    std::vector<QString> existing;
    for (int i = 1; i <= 3; ++i) {
        existing.push_back(crash::dumpFileName(QStringLiteral("2026010%1-000000").arg(i), 7));
    }
    CHECK(crash::dumpsToPrune(existing).empty());
    CHECK(crash::dumpsToPrune({}).empty());
}

TEST_CASE("crash: exactly at the cap still frees a slot", "[app][crash][t86]") {
    // The off-by-one that matters: with kMaxDumps already present, writing one
    // more must drop the oldest, or the directory grows by one per crash
    // forever. Four survive plus the new one, which is the cap.
    std::vector<QString> existing;
    for (std::size_t i = 1; i <= crash::kMaxDumps; ++i) {
        existing.push_back(
            crash::dumpFileName(QStringLiteral("2026010%1-000000").arg(static_cast<int>(i)), 7));
    }
    const std::vector<QString> pruned = crash::dumpsToPrune(existing);
    REQUIRE(pruned.size() == 1);
    CHECK(pruned[0] == crash::dumpFileName(QStringLiteral("20260101-000000"), 7));
}

TEST_CASE("crash: install refuses an empty directory", "[app][crash][t86]") {
    // The one install() path that is safe to exercise: it must not install
    // handlers pointed at nowhere, because they would then run at crash time
    // and write nothing while looking installed.
    CHECK_FALSE(crash::install(QString()));
    CHECK(crash::dumpDir().isEmpty());
}

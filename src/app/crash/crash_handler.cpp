#include "app/crash/crash_handler.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// After windows.h, which DbgHelp depends on.
#include <dbghelp.h>

#include <QDateTime>
#include <QDir>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdlib>
#include <exception>

namespace krait::app::crash {
namespace {

// The one piece of state the handlers need. A plain global because a crash
// handler cannot take a parameter and must not allocate to find its context —
// by the time it runs, the heap is the thing least worth trusting.
QString g_dumpDir;

// DbgHelp is documented as single-threaded: "calls from more than one thread to
// this function will likely result in unexpected behavior or memory
// corruption". Two threads faulting at once is exactly when that happens, so
// the first one through wins and the second is left to the OS. A CRITICAL
// SECTION rather than std::mutex: this must work when the CRT is already sick,
// and it must not throw.
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
bool g_dumped = false;

// Everything below runs INSIDE a crashing process. Nothing here allocates more
// than it must, and nothing takes a lock that application code holds.

void pruneOldDumps(QDir& dir) {
    const QStringList names =
        dir.entryList({QStringLiteral("krait-*.dmp")}, QDir::Files, QDir::Name);
    std::vector<QString> existing;
    existing.reserve(static_cast<std::size_t>(names.size()));
    for (const QString& name : names) {
        existing.push_back(name);
    }
    for (const QString& stale : dumpsToPrune(std::move(existing))) {
        dir.remove(stale);
    }
}

// Writes the dump. `pointers` is null for the non-SEH entry points (terminate,
// purecall, invalid parameter), where there is no EXCEPTION_POINTERS to hand
// over — the dump is still useful, it just carries no fault address.
void writeDump(EXCEPTION_POINTERS* pointers) {
    if (!g_lockReady || g_dumpDir.isEmpty()) {
        return;
    }
    EnterCriticalSection(&g_lock);
    // One dump per process life. A handler that re-entered — a fault inside
    // MiniDumpWriteDump, which is not hypothetical when the heap is corrupt —
    // would otherwise recurse until the stack ran out.
    const bool already = g_dumped;
    g_dumped = true;
    LeaveCriticalSection(&g_lock);
    if (already) {
        return;
    }

    QDir dir(g_dumpDir);
    if (!dir.exists() && !QDir().mkpath(g_dumpDir)) {
        return;
    }
    pruneOldDumps(dir);

    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString path =
        dir.filePath(dumpFileName(stamp, static_cast<unsigned long>(GetCurrentProcessId())));

    const HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE, 0,
                                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = pointers;
    // FALSE: the memory is in the CALLING process. Microsoft is explicit that
    // TRUE means "resides in the process being debugged", and setting it for a
    // self-dump makes DbgHelp read our pointers as another process's addresses.
    info.ClientPointers = FALSE;

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      static_cast<MINIDUMP_TYPE>(kDumpType), pointers != nullptr ? &info : nullptr,
                      nullptr, nullptr);
    CloseHandle(file);
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* pointers) {
    writeDump(pointers);
    // EXECUTE_HANDLER, not CONTINUE_SEARCH: continuing hands the crash to WER,
    // which puts "Krait has stopped working" on top of the dump just written.
    // The process is going down either way.
    return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate() {
    writeDump(nullptr);
    // Must not return: std::terminate calls abort() if a handler does, and
    // abort's own path would write a second dump for the same death.
    _exit(3);
}

void onPureCall() {
    writeDump(nullptr);
    _exit(3);
}

void onInvalidParameter(const wchar_t* /*expression*/, const wchar_t* /*function*/,
                        const wchar_t* /*file*/, unsigned int /*line*/, uintptr_t /*reserved*/) {
    // Every argument is null unless a DEBUG CRT is in use, so none is read:
    // dereferencing a pointer the docs call null in release builds is how a
    // crash handler becomes the crash.
    writeDump(nullptr);
    _exit(3);
}

}  // namespace

QString dumpFileName(const QString& utcStamp, unsigned long pid) {
    return QStringLiteral("krait-%1-%2.dmp").arg(utcStamp).arg(pid);
}

std::vector<QString> dumpsToPrune(std::vector<QString> existing) {
    // The names sort chronologically because the stamp leads and is fixed
    // width, so this needs no timestamps off the filesystem — which a crashing
    // process has no business asking for.
    std::sort(existing.begin(), existing.end());
    // Room for the one about to be written, hence kMaxDumps - 1.
    const std::size_t keep = kMaxDumps > 0 ? kMaxDumps - 1 : 0;
    if (existing.size() <= keep) {
        return {};
    }
    existing.resize(existing.size() - keep);
    return existing;
}

bool install(const QString& dumpDirectory) {
    if (dumpDirectory.isEmpty()) {
        return false;
    }
    // Loaded eagerly so a crash never has to hit the loader lock — the deadlock
    // Microsoft warns about for in-process dumping. If dbghelp is not here, say
    // so now rather than discovering it while falling over.
    if (LoadLibraryW(L"dbghelp.dll") == nullptr) {
        return false;
    }
    g_dumpDir = dumpDirectory;
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;

    SetUnhandledExceptionFilter(onUnhandledException);
    // Each of these is a death Windows does NOT route through the unhandled
    // exception filter, and each needs its own hook or it leaves no dump.
    std::set_terminate(onTerminate);
    _set_purecall_handler(onPureCall);
    _set_invalid_parameter_handler(onInvalidParameter);
    return true;
}

QString dumpDir() {
    return g_dumpDir;
}

}  // namespace krait::app::crash

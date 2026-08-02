#pragma once

#include <QString>

#include <cstddef>
#include <vector>

namespace krait::app::crash {

// Local crash dumps (M6 T86).
//
// No Breakpad, no Crashpad. What this needs is a file on disk with a stack in
// it, and the OS ships that — a crash-reporting dependency would be a large
// third-party surface for something MiniDumpWriteDump does in ten lines.
//
// SUBMISSION IS DELIBERATELY ABSENT. docs/plan/01-milestones.md puts "crash
// submit" last on M6's cut line with "local dumps stay" beside it, so this
// writes the file and nothing else. Nothing is uploaded, nothing phones home,
// and there is no setting that makes it — a setting whose only honest value is
// "off" is worse than no setting at all.

// What goes in the dump. Pinned here rather than at the call site because the
// choice is a SECURITY one, not a formatting one: Krait holds SSH passwords in
// memory, so every flag that captures heap or globals is excluded.
//
//   MiniDumpNormal                 stacks for every thread, and nothing else
//   MiniDumpWithThreadInfo         thread state
//   MiniDumpWithUnloadedModules    what had just been unloaded
//
// Explicitly NOT: WithDataSegs (globals — a static key buffer lands there),
// WithIndirectlyReferencedMemory (heap pages reachable from a local — any
// std::string password), and every WithFullMemory variant (the whole heap).
// A stack can still hold a password in a register spill; that is the residual
// risk of dumping at all, and it is why nothing here uploads.
inline constexpr unsigned long kDumpType = 0x0U       // MiniDumpNormal
                                           | 0x1000U  // MiniDumpWithThreadInfo
                                           | 0x20U;   // MiniDumpWithUnloadedModules

// Keep the last few and delete the rest. A crash loop must not fill the disk,
// and the fifth-oldest dump of the same bug has never helped anyone.
inline constexpr std::size_t kMaxDumps = 5;

// The dump file name for a crash at `utcStamp` in process `pid`.
//
// Split out so it can be tested: the handler runs in a process that is already
// falling over, which is the worst possible place to discover the path was
// wrong. Sortable, so "the newest" is a string comparison and not a stat().
QString dumpFileName(const QString& utcStamp, unsigned long pid);

// Which of `existing` to delete so at most kMaxDumps remain once one more is
// written. Takes and returns names rather than touching the disk, for the same
// reason.
std::vector<QString> dumpsToPrune(std::vector<QString> existing);

// Installs the handlers. `dumpDir` is created on demand at crash time, never
// here — a directory made at startup for an event that never comes is litter in
// every user's profile.
//
// Call ONCE, early, from the main thread. Returns false when DbgHelp cannot be
// loaded, in which case Krait runs on without crash dumps rather than refusing
// to start over a diagnostic.
//
// What this catches: unhandled SEH exceptions, uncaught C++ exceptions
// (std::terminate), pure-virtual calls, and CRT invalid-parameter faults. What
// it CANNOT catch, because Windows runs no handler at all for them: __fastfail,
// which is where a /GS stack-cookie failure and a detected heap corruption both
// end up. Microsoft documents those as "the program is expected to be in a
// corrupted state" with no supported interception — so a stack smash leaves no
// dump, and that is a limit of the platform rather than of this file.
bool install(const QString& dumpDir);

// The directory install() was given, or empty when it was never called.
QString dumpDir();

}  // namespace krait::app::crash

#pragma once

#include <QLatin1StringView>
#include <QString>

#include <cstdint>
#include <span>

namespace krait::app {

// One place a shell-integration block can be installed (plan T73).
//
// FLAT PAIRS, not one entry per shell. PowerShell has two profile paths
// depending on what the far end runs — `.config/powershell/` on a pwsh that
// came from a package manager, `Documents/PowerShell/` on Windows OpenSSH —
// and "which file" is the question the user actually has to be asked. A table
// keyed by shell would have to answer it with a guess.
struct ShellTarget {
    // What to call it in the confirmation surface.
    QString shell;
    // File name under <exedir>/shell-integration/.
    QString script;
    // POSIX path relative to the login directory. SFTP has one separator and it
    // is not the platform's.
    QString rc;
};

// Every candidate, in probe order.
std::span<const ShellTarget> shellTargets();

// The target whose rc path is `rc`, or null. Callers pass back an id the user
// picked from shellTargets(), so an unknown one is a stale surface rather than
// a state to model.
const ShellTarget* shellTargetFor(const QString& rc);

// The delimiters. Whole lines, and matched as whole lines: a marker that
// matched a substring would let a comment ABOUT Krait swallow the rest of
// someone's rc file.
inline constexpr QLatin1StringView kBlockBegin{"# >>> krait shell integration >>>"};
inline constexpr QLatin1StringView kBlockEnd{"# <<< krait shell integration <<<"};

// What is already in an rc file.
//
// `Damaged` is not paranoia: a half-deleted block — one marker, or an end
// before a begin — cannot be replaced without guessing where it ends, and
// guessing wrong truncates a file on somebody else's machine. The caller says
// so and leaves the file alone.
enum class BlockState : std::uint8_t { Absent, Present, Damaged };

BlockState blockState(const QString& text);

// `text` with the Krait block replaced by `payload`, appended when there was
// none, or removed entirely when `payload` is empty.
//
// Everything outside the markers is returned byte for byte, including the line
// endings: this rewrites a file the user owns, and a "helpful" normalisation
// pass is an edit nobody asked for. Only call this when blockState() is not
// Damaged.
QString spliceBlock(const QString& text, const QString& payload);

// The bundled script `fileName`, or an empty string when it is not beside the
// executable. Reading it is the caller's job — this only answers where.
QString scriptPath(const QString& fileName);

}  // namespace krait::app

#include "shell_integration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>

#include <array>

namespace krait::app {

namespace {

// Probe order, and it is the order the surface offers them in.
//
// bash points at `.bashrc` rather than `.bash_profile` because that is what
// kitty's and wezterm's own installers write, and because `.bash_profile` on
// every mainstream distro already sources it. Choosing differently here would
// make Krait the odd one out in a file three other tools also edit.
const std::array<ShellTarget, 5>& targets() {
    static const std::array<ShellTarget, 5> kTargets{{
        {.shell = QStringLiteral("bash"),
         .script = QStringLiteral("krait.bash"),
         .rc = QStringLiteral(".bashrc")},
        {.shell = QStringLiteral("zsh"),
         .script = QStringLiteral("krait.zsh"),
         .rc = QStringLiteral(".zshrc")},
        {.shell = QStringLiteral("fish"),
         .script = QStringLiteral("krait.fish"),
         .rc = QStringLiteral(".config/fish/config.fish")},
        {.shell = QStringLiteral("powershell"),
         .script = QStringLiteral("krait.ps1"),
         .rc = QStringLiteral(".config/powershell/Microsoft.PowerShell_profile.ps1")},
        // Windows OpenSSH, where $PROFILE lands under the documents folder
        // instead. A different FILE for the same shell, which is why the table
        // is keyed by path.
        {.shell = QStringLiteral("powershell"),
         .script = QStringLiteral("krait.ps1"),
         .rc = QStringLiteral("Documents/PowerShell/Microsoft.PowerShell_profile.ps1")},
    }};
    return kTargets;
}

// Whole-line marker match. `trimmed()` so an rc file that was reindented — or a
// block someone pasted with leading spaces — is still found, while a line that
// merely CONTAINS the marker text is not.
bool isMarker(const QString& line, QLatin1StringView marker) {
    return line.trimmed() == marker;
}

}  // namespace

std::span<const ShellTarget> shellTargets() {
    return targets();
}

const ShellTarget* shellTargetFor(const QString& rc) {
    for (const ShellTarget& target : targets()) {
        if (target.rc == rc) {
            return &target;
        }
    }
    return nullptr;
}

BlockState blockState(const QString& text) {
    int begin = -1;
    int end = -1;
    int begins = 0;
    int ends = 0;
    const QStringList lines = text.split(u'\n');
    for (int i = 0; i < lines.size(); ++i) {
        if (isMarker(lines.at(i), kBlockBegin)) {
            ++begins;
            if (begin < 0) {
                begin = i;
            }
        } else if (isMarker(lines.at(i), kBlockEnd)) {
            ++ends;
            if (end < 0) {
                end = i;
            }
        }
    }
    if (begins == 0 && ends == 0) {
        return BlockState::Absent;
    }
    if (begins == 1 && ends == 1 && begin < end) {
        return BlockState::Present;
    }
    // One marker without the other, either order, or a second copy of either:
    // there is no single span to replace, so there is nothing safe to do.
    return BlockState::Damaged;
}

QString spliceBlock(const QString& text, const QString& payload) {
    QStringList block;
    if (!payload.isEmpty()) {
        block.append(kBlockBegin);
        QString body = payload;
        // One trailing newline, not all of them: a script file ends with one,
        // and keeping it would put a blank line before the end marker every
        // time the block is rewritten.
        if (body.endsWith(u'\n')) {
            body.chop(1);
        }
        block.append(body.split(u'\n'));
        block.append(kBlockEnd);
    }

    QStringList lines = text.split(u'\n');
    int begin = -1;
    int end = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (begin < 0 && isMarker(lines.at(i), kBlockBegin)) {
            begin = i;
        } else if (begin >= 0 && isMarker(lines.at(i), kBlockEnd)) {
            end = i;
            break;
        }
    }

    if (begin >= 0 && end > begin) {
        lines.erase(lines.begin() + begin, lines.begin() + end + 1);
        for (int i = 0; i < block.size(); ++i) {
            lines.insert(begin + i, block.at(i));
        }
        return lines.join(u'\n');
    }

    if (block.isEmpty()) {
        return text;  // nothing to remove
    }
    // Appended BEFORE the trailing empty element, which is what split() leaves
    // behind for a file that ends with a newline — so a well-formed rc file
    // stays well-formed. A file that does not end with one gets the newline it
    // was missing, because there is no way to append after an unterminated
    // line.
    if (!lines.isEmpty() && lines.constLast().isEmpty()) {
        lines.remove(lines.size() - 1);
        lines.append(block);
        lines.append(QString());
    } else {
        lines.append(block);
        lines.append(QString());
    }
    return lines.join(u'\n');
}

QString scriptPath(const QString& fileName) {
    const QString path = QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("shell-integration/") + fileName);
    return QFile::exists(path) ? path : QString();
}

}  // namespace krait::app

#include "paste.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <array>

namespace krait::app::input {
namespace {

// Commands whose damage is not undoable, or which escalate privilege. Kept
// SHORT on purpose: a long list trains people to click through the banner, and
// a guard everyone dismisses protects nobody. Each entry is here because a
// mis-paste of it has cost somebody a machine.
//
// Matched on word boundaries so `sudoku` and `format.py` do not trip it.
constexpr std::array<const char*, 8> kDangerous{
    R"(\bsudo\b)",
    R"(\bdoas\b)",
    R"(\brm\s+(-[a-zA-Z]*[rf][a-zA-Z]*\s+)+)",  // rm -rf, rm -fr, rm -r -f
    R"(\bmkfs(\.\w+)?\b)",
    R"(\bdd\s+if=)",
    R"(\bchmod\s+(-[a-zA-Z]+\s+)*777\b)",
    R"((curl|wget)\b[^|]*\|\s*(sudo\s+)?(ba|z|k|)sh\b)",  // curl ... | sh
    R"(\bformat\s+[a-zA-Z]:)",                            // the Windows one
};

bool looksDangerous(const QString& text) {
    for (const char* pattern : kDangerous) {
        const QRegularExpression re(QString::fromLatin1(pattern),
                                    QRegularExpression::CaseInsensitiveOption);
        if (re.match(text).hasMatch()) {
            return true;
        }
    }
    return false;
}

}  // namespace

PasteResult preparePaste(const QString& text, bool bracketed) {
    PasteResult result;

    // Line endings first, as a whole-string pass. Done inside the character
    // loop it has to look AHEAD — CR arrives before LF — and getting that
    // backwards turns every CRLF into two line endings, so the shell sees a
    // blank command between every pair of real ones.
    QString normalised = text;
    const qsizetype before = normalised.size();
    normalised.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    normalised.replace(u'\r', u'\n');
    // Unicode's OTHER line breaks, normalised before anything looks at the
    // text. This is a guard BYPASS otherwise, not a tidiness point: two commands
    // joined with U+2028 count as ONE line to the risk classifier below, so a
    // dangerous second half rides through as a harmless single-line paste.
    normalised.replace(QChar(0x0085), u'\n');  // NEL
    normalised.replace(QChar(0x2028), u'\n');  // LINE SEPARATOR
    normalised.replace(QChar(0x2029), u'\n');  // PARAGRAPH SEPARATOR
    result.sanitised = normalised.size() != before;

    QString clean;
    clean.reserve(normalised.size());
    for (const QChar ch : normalised) {
        const char16_t code = ch.unicode();
        if (code == u'\n' || code == u'\t') {
            clean += ch;
            continue;
        }
        // Everything else below 0x20, plus DEL. ESC is the one that matters,
        // but there is no C0 a paste has any business carrying.
        if (code < 0x20 || code == 0x7F) {
            result.sanitised = true;
            continue;
        }
        // C1, U+0080-U+009F. Defence in depth: our parser honours 8-bit C1 only
        // when its policy flag is on, and it is off under UTF-8 — but U+009B IS
        // CSI to a parser that does honour it, so a paste carrying one is an
        // escape sequence waiting for a configuration change or a backend that
        // transcodes. No legitimate pasted text contains C1 either way.
        if (code >= 0x80 && code <= 0x9F) {
            result.sanitised = true;
            continue;
        }
        clean += ch;
    }

    // Risk, highest first. `sudo` in a one-liner is more worth saying than
    // "this is multiline", so the checks are ordered rather than accumulated.
    const QString withoutTail = clean.endsWith(u'\n') ? clean.chopped(1) : clean;
    if (looksDangerous(clean)) {
        result.risk = PasteRisk::DangerousCommand;
    } else if (withoutTail.contains(u'\n')) {
        result.risk = PasteRisk::Multiline;
    } else if (clean.endsWith(u'\n')) {
        result.risk = PasteRisk::ExecutesOnPaste;
    }

    QByteArray payload = clean.toUtf8();
    payload.replace('\n', '\r');  // what a terminal sends for Enter

    if (bracketed) {
        // Neutralise any END marker the payload carries. ESC is already gone,
        // so a literal ESC[201~ cannot survive the loop above — but "[201~"
        // can, and a future change to the sanitiser must not silently make this
        // exploitable again. Belt and braces on the one thing that turns a
        // paste into arbitrary keystrokes.
        payload.replace("[201~", "[201 ~");
        result.bytes = QByteArray("\x1B[200~") + payload + "\x1B[201~";
    } else {
        result.bytes = payload;
    }
    return result;
}

QString describeRisk(PasteRisk risk) {
    switch (risk) {
    case PasteRisk::DangerousCommand:
        return QCoreApplication::translate("PasteGuard",
                                           "This paste contains a command that can destroy data "
                                           "or escalate privileges. Read it before allowing it.");
    case PasteRisk::ExecutesOnPaste:
        return QCoreApplication::translate(
            "PasteGuard", "This paste ends with a newline, so the shell will run it immediately.");
    case PasteRisk::Multiline:
        return QCoreApplication::translate(
            "PasteGuard", "This paste has more than one line. Every line will run.");
    case PasteRisk::None:
        break;
    }
    return {};
}

}  // namespace krait::app::input

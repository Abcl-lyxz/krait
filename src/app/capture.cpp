#include "capture.h"

#include "session/profile.h"
#include "sftp_model.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace krait::app {
namespace {

constexpr int kBytesPerLine = 16;
constexpr char kHex[] = "0123456789abcdef";

void appendByte(std::string* out, std::uint8_t byte) {
    out->push_back(kHex[byte >> 4]);
    out->push_back(kHex[byte & 0x0F]);
}

// The offset column, eight hex digits. Wide enough for 4 GB of session, and a
// session longer than that has bigger problems than a narrow column.
void appendOffset(std::string* out, std::uint64_t offset) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        out->push_back(kHex[(offset >> shift) & 0x0F]);
    }
}

}  // namespace

std::string formatHexdump(const QByteArray& bytes, std::uint64_t offset) {
    std::string out;
    // Three characters per byte in the hex column, one in the text column, plus
    // the offset and separators — reserved so a flood does not reallocate per
    // line.
    out.reserve(static_cast<std::size_t>(bytes.size()) * 5 + 64);

    for (qsizetype at = 0; at < bytes.size(); at += kBytesPerLine) {
        const qsizetype take = std::min<qsizetype>(kBytesPerLine, bytes.size() - at);
        appendOffset(&out, offset + static_cast<std::uint64_t>(at));
        out += "  ";
        for (qsizetype i = 0; i < kBytesPerLine; ++i) {
            if (i < take) {
                appendByte(&out, static_cast<std::uint8_t>(bytes[at + i]));
            } else {
                out += "  ";  // pad, so the text column stays aligned
            }
            out.push_back(' ');
            if (i == 7) {
                out.push_back(' ');  // the traditional half-way gap
            }
        }
        out += " |";
        for (qsizetype i = 0; i < take; ++i) {
            const auto byte = static_cast<std::uint8_t>(bytes[at + i]);
            // Printable ASCII only. Anything else is a dot — including bytes
            // that would be valid UTF-8 continuations, because a hexdump that
            // decodes text is a hexdump that hides the thing being looked for.
            out.push_back(byte >= 0x20 && byte < 0x7F ? static_cast<char>(byte) : '.');
        }
        out += "|\r\n";
    }
    return out;
}

SessionLog::~SessionLog() {
    // std::ofstream::flush can throw if the stream has exceptions enabled, and
    // an exception leaving a destructor terminates the process. Losing the tail
    // of a log because the disk filled is bad; taking the session down with it
    // while the user is mid-command is worse.
    try {
        close();
    } catch (...) {  // NOLINT(bugprone-empty-catch): see above
    }
}

bool SessionLog::open(const QString& path, LogFormat format) {
    close();
    m_format = format;
    m_failed = false;
    m_stripOut = session::StripState::Ground;
    m_stripIn = session::StripState::Ground;
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        m_error = QStringLiteral("could not create %1").arg(info.absolutePath());
        return false;
    }
    m_stream.open(path.toStdString(), std::ios::binary | std::ios::app);
    if (!m_stream.is_open()) {
        m_error = QStringLiteral("could not write %1").arg(path);
        return false;
    }
    m_path = path;
    m_error.clear();
    m_lastDirection = '\0';
    m_atLineStart = true;
    if (m_format == LogFormat::Raw) {
        // No header. Raw means byte-exact: a banner at the top would be the one
        // thing in the file the far end did not send, and it would land in the
        // middle of the screen on replay.
        return true;
    }
    const QString header = QStringLiteral("=== krait session log, started %1 ===\n")
                               .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    const QByteArray utf8 = header.toUtf8();
    m_stream.write(utf8.constData(), utf8.size());
    return true;
}

void SessionLog::close() {
    if (m_stream.is_open()) {
        m_stream.flush();
        m_stream.close();
    }
}

bool SessionLog::takeFailure() {
    const bool failed = m_failed;
    m_failed = false;
    return failed;
}

void SessionLog::checkStream() {
    if (m_failed || !m_stream.is_open() || m_stream.good()) {
        return;
    }
    m_failed = true;
    // The disk filled, the share went away, the stick was pulled. Named once,
    // then the stream is closed so nothing keeps pretending to record.
    m_error = QStringLiteral("stopped writing %1").arg(m_path);
    m_stream.close();
}

void SessionLog::writeChunk(char direction, const QByteArray& raw_bytes) {
    if (!m_stream.is_open() || raw_bytes.isEmpty()) {
        return;
    }
    QByteArray stripped;
    if (m_format == LogFormat::Text) {
        // Stripped BEFORE the framing below, and with per-direction state, so a
        // sequence split across two reads cannot walk the stripper (the reason
        // TriggerEngine carries a StripState at all).
        session::StripState& state = direction == '<' ? m_stripOut : m_stripIn;
        stripped = QByteArray::fromStdString(session::plainText(
            std::string_view(raw_bytes.constData(), static_cast<std::size_t>(raw_bytes.size())),
            state));
        if (stripped.isEmpty()) {
            return;  // a chunk that was nothing but escape sequences
        }
    }
    const QByteArray& bytes = m_format == LogFormat::Text ? stripped : raw_bytes;
    for (const char raw : bytes) {
        const auto byte = static_cast<std::uint8_t>(raw);
        if (m_atLineStart || direction != m_lastDirection) {
            if (!m_atLineStart) {
                m_stream.put('\n');
            }
            const QByteArray stamp =
                QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")).toUtf8();
            m_stream.write(stamp.constData(), stamp.size());
            m_stream.put(' ');
            m_stream.put(direction);
            m_stream.put(' ');
            m_lastDirection = direction;
            m_atLineStart = false;
        }
        if (byte == '\n') {
            m_stream.put('\n');
            m_atLineStart = true;
            continue;
        }
        if (byte == '\r') {
            continue;  // the newline above already ends the line
        }
        if (byte >= 0x20 && byte != 0x7F) {
            m_stream.put(raw);
            continue;
        }
        // Escaped rather than written raw: a log full of control codes is a log
        // that reformats itself when anyone opens it, and the escape sequences
        // are usually the interesting part.
        m_stream.put('\\');
        m_stream.put('x');
        m_stream.put(kHex[byte >> 4]);
        m_stream.put(kHex[byte & 0x0F]);
    }
    // Flushed per chunk: a session log exists to survive whatever is about to
    // happen to the session, and a buffered tail is exactly the part that gets
    // lost when it does.
    m_stream.flush();
    checkStream();
}

void SessionLog::writeOutput(const QByteArray& bytes) {
    if (m_format == LogFormat::Raw) {
        if (!m_stream.is_open() || bytes.isEmpty()) {
            return;
        }
        m_stream.write(bytes.constData(), bytes.size());
        m_stream.flush();
        checkStream();
        return;
    }
    writeChunk('<', bytes);
}

void SessionLog::writeInput(const QByteArray& bytes) {
    if (m_format == LogFormat::Raw) {
        return;  // see LogFormat::Raw — a byte-exact stream has no second lane
    }
    writeChunk('>', bytes);
}

namespace {

// One substituted template value, reduced to something Win32 will accept as a
// single path component. See expandLogPath for why this is where the trust
// boundary sits.
QString safeField(const QString& raw) {
    if (raw.isEmpty()) {
        return QStringLiteral("none");  // a local shell has no host
    }
    const QString slug = QString::fromStdString(session::slugify(raw.toStdString()));
    // slugify never returns empty and leaves only [a-z0-9-], so everything that
    // makes a path component dangerous is already gone. The one thing it cannot
    // know about is a DOS device name — "con" slugifies to "con", and CON is
    // still a device — which is precisely what isSafeLeafName checks.
    return isSafeLeafName(slug) ? slug : slug + QStringLiteral("-log");
}

}  // namespace

QString expandLogPath(const QString& configDir, const QString& tmpl, const LogFields& fields,
                      const QDateTime& when) {
    QString out = tmpl.isEmpty() ? QString::fromLatin1(kDefaultLogTemplate) : tmpl;
    out.replace(QStringLiteral("{session}"), safeField(fields.session));
    out.replace(QStringLiteral("{host}"), safeField(fields.host));
    // Ours, not anyone's input, so they need no sanitising — and fixed-width, so
    // the files sort by name into the order they were written.
    out.replace(QStringLiteral("{date}"), when.toString(QStringLiteral("yyyyMMdd")));
    out.replace(QStringLiteral("{time}"), when.toString(QStringLiteral("HHmmss")));
    // An unknown placeholder is left alone on purpose: "{hostname}" then shows
    // up in the file name, which is the fastest way anyone will notice the typo.
    return QDir(configDir).filePath(out);
}

}  // namespace krait::app

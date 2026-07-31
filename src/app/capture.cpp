#include "capture.h"

#include "session/profile.h"

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

bool SessionLog::open(const QString& path) {
    close();
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

void SessionLog::writeChunk(char direction, const QByteArray& bytes) {
    if (!m_stream.is_open() || bytes.isEmpty()) {
        return;
    }
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
}

void SessionLog::writeOutput(const QByteArray& bytes) {
    writeChunk('<', bytes);
}

void SessionLog::writeInput(const QByteArray& bytes) {
    writeChunk('>', bytes);
}

QString sessionLogPath(const QString& configDir, const QString& sessionName) {
    const std::string slug = session::slugify(sessionName.isEmpty() ? std::string("session")
                                                                    : sessionName.toStdString());
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    return QDir(configDir).filePath(
        QStringLiteral("logs/%1-%2.log").arg(QString::fromStdString(slug), stamp));
}

}  // namespace krait::app

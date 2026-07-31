#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <fstream>
#include <string>

namespace krait::app {

// Seeing and keeping what crossed the wire (plan T57).
//
// Both halves exist for the same reason the serial and raw backends do: when a
// device is misbehaving, the question is what it ACTUALLY sent, and a terminal
// that only shows the interpretation cannot answer it.

// One canonical hexdump line per sixteen bytes: offset, hex, printable column.
//
// `offset` is the running position in the STREAM, not in `bytes` — a hexdump
// whose offsets restart at every read is useless for finding the byte someone
// is asking about, and reads arrive at the mercy of TCP.
//
// Returns text with CRLF line endings, because it is fed to the VT parser and
// a bare LF would leave the cursor mid-line.
std::string formatHexdump(const QByteArray& bytes, std::uint64_t offset);

// A timestamped record of a session.
//
// rules/net.md forbids secrets in logs, and this deliberately does not try to
// filter: it captures the session byte for byte because that is what it is for,
// so a password typed while it is running is in the file. The mitigation is
// that it is OFF unless a person turns it on, per session, and the banner names
// the file — an invisible log is how a secret ends up somewhere nobody
// remembers.
class SessionLog {
  public:
    SessionLog() = default;
    ~SessionLog();

    SessionLog(const SessionLog&) = delete;
    SessionLog& operator=(const SessionLog&) = delete;

    // Opens `path`, creating parent directories. False when it cannot be
    // written; error() says why.
    bool open(const QString& path);
    void close();

    bool isOpen() const { return m_stream.is_open(); }

    const QString& path() const { return m_path; }

    const QString& error() const { return m_error; }

    // Session bytes. Direction is recorded because a log that cannot tell what
    // the user typed from what the device answered cannot settle the argument
    // it was started to settle.
    void writeOutput(const QByteArray& bytes);
    void writeInput(const QByteArray& bytes);

  private:
    void writeChunk(char direction, const QByteArray& bytes);

    std::ofstream m_stream;
    QString m_path;
    QString m_error;
    // Only stamp when the direction changes or a line ends: a timestamp per
    // byte would be a file of timestamps with a session hidden in it.
    char m_lastDirection = '\0';
    bool m_atLineStart = true;
};

// Where a session's log goes: <dir>/logs/<name>-<YYYYMMDD-HHMMSS>.log. The name
// is slugified, so a session called "prod/eu — web 1" cannot produce a path
// with a directory separator in it.
QString sessionLogPath(const QString& configDir, const QString& sessionName);

}  // namespace krait::app

#pragma once

#include "session/triggers.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <fstream>
#include <string>

class QDateTime;

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

// What a session log writes. Three, because they answer three different
// questions and a file in the wrong one of them is useless for the other two.
enum class LogFormat : std::uint8_t {
    // Output bytes exactly as they arrived, nothing added. `type` the file back
    // into a terminal and the session repaints; it is the only format worth
    // diffing against a packet capture.
    //
    // Input is NEVER written in this format whatever logging.includeInput says:
    // there is no room for a direction marker in a byte-exact stream, so
    // interleaving the two would silently produce a file that is neither
    // replayable nor readable.
    Raw,
    // Timestamped, direction-marked, control bytes written as \xNN. The
    // default, and the format T57 shipped: it answers "what did the device
    // send, and when", which is what a log is usually started for.
    Escaped,
    // Escape sequences removed (session::plainText), then timestamped and
    // direction-marked. What goes in a bug report — see the warning above.
    Text,
};

// A record of a session.
//
// WHAT A LOG FILE CAN CONTAIN. rules/net.md forbids secrets in logs, and this
// class cannot honour that by filtering: a session log is a transcript, and a
// transcript that dropped the interesting line would be a transcript nobody
// could trust. So the honest statement, which docs/configuration.md repeats in
// the user's own words:
//
//   * With `includeInput`, everything typed is in the file — INCLUDING a
//     password typed at an echo-off prompt. That is why it defaults to off.
//   * Even with input off, a log is NOT safe to attach to a bug report. A
//     normal shell echoes what you type, so at an ordinary prompt your
//     keystrokes are in the OUTPUT stream anyway. Echoing stops only where the
//     far end turns echo off, which is exactly the password prompt — so
//     output-only buys you the password prompt case and nothing else.
//   * The remote side chooses what it sends. `env`, a token in a git remote,
//     a private key printed by a careless script, an MOTD naming internal
//     hosts — all of it lands verbatim.
//
// There is no redacting logger in this repo to route this through, and one
// would not help if there were: rules/net.md aims that requirement at
// diagnostic logging in src/net/, where the code chooses what to write. Here
// the code's whole job is to write what it was given.
//
// The mitigations are therefore procedural, and all three matter: it is off
// unless a person turns it on, it is per session rather than global, and the
// tab says so on screen for as long as it runs. An invisible log is how a
// secret ends up somewhere nobody remembers.
class SessionLog {
  public:
    SessionLog() = default;
    ~SessionLog();

    SessionLog(const SessionLog&) = delete;
    SessionLog& operator=(const SessionLog&) = delete;

    // Opens `path`, creating parent directories. False when it cannot be
    // written; error() says why.
    bool open(const QString& path, LogFormat format = LogFormat::Escaped);
    void close();

    bool isOpen() const { return m_stream.is_open(); }

    const QString& path() const { return m_path; }

    const QString& error() const { return m_error; }

    // Whether the stream went bad AFTER a successful open — a full disk, a
    // pulled USB stick, a network share that went away — and CONSUMES that
    // fact, so a caller polling per chunk raises one banner rather than one per
    // read at whatever rate the far end is sending. A logging session that
    // quietly stopped recording is worse than one that never started, and a
    // banner per chunk is how a user learns to dismiss them without reading.
    bool takeFailure();

    // Session bytes. Direction is recorded because a log that cannot tell what
    // the user typed from what the device answered cannot settle the argument
    // it was started to settle.
    void writeOutput(const QByteArray& bytes);
    void writeInput(const QByteArray& bytes);

  private:
    void writeChunk(char direction, const QByteArray& raw_bytes);
    // Records a write failure once and stops. Non-const: the whole point is
    // that the state outlives the chunk that provoked it.
    void checkStream();

    std::ofstream m_stream;
    QString m_path;
    QString m_error;
    LogFormat m_format = LogFormat::Escaped;
    // Where plainText() was when the previous chunk ran out, for Text format.
    // Per direction, because the two streams interleave and a half-finished
    // CSI in the output must not swallow the next thing the user types.
    session::StripState m_stripOut = session::StripState::Ground;
    session::StripState m_stripIn = session::StripState::Ground;
    // Only stamp when the direction changes or a line ends: a timestamp per
    // byte would be a file of timestamps with a session hidden in it.
    char m_lastDirection = '\0';
    bool m_atLineStart = true;
    bool m_failed = false;
};

// The values a log path template can name that are not the clock.
struct LogFields {
    QString session;
    QString host;
};

// The default when `logging.pathTemplate` is empty. Reproduces exactly what
// T57 wrote, so turning the setting on changes nothing until it is edited.
inline constexpr auto kDefaultLogTemplate = "logs/{session}-{date}-{time}.log";

// Expands a log path template against `configDir`.
//
// Placeholders: {session} {host} {date} {time}.
//
// WHAT IS TRUSTED AND WHAT IS NOT. The TEMPLATE is trusted — the user typed it
// into their own settings file, and a directory separator in it is how they say
// "one folder per host". The SUBSTITUTED VALUES are not: a profile name usually
// arrives from an importer, and a host name is remote-influenced. Every value
// therefore goes through session::slugify(), which leaves only [a-z0-9-] — so a
// separator, a "..", a control character, a drive letter, a wildcard and a
// trailing dot or space are all gone by construction rather than by a blocklist
// — and is then checked against isSafeLeafName(), which is the part that knows
// "con" is still a device name on Windows.
//
// `when` is a parameter rather than a clock read so the substitution is
// testable, the same arrangement TriggerEngine::feed uses for its rate limiter.
QString expandLogPath(const QString& configDir, const QString& tmpl, const LogFields& fields,
                      const QDateTime& when);

}  // namespace krait::app

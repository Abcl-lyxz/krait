#pragma once

#include "../error.h"
#include "../ibackend.h"
#include "serial_ports.h"

#include <QByteArray>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace krait::net {

struct SerialConfig {
    // "COM7". PuTTY keeps the serial line in the same field it uses for a host
    // name and so do we — a profile has one "where", and inventing a second
    // field for it would mean every importer and every editor learning both.
    std::string port;
    int baud = 115200;
    int dataBits = 8;
    // 'N', 'E', 'O', 'M', 'S'. A character rather than an enum because that is
    // how every serial tool in existence spells it, including the one the user
    // is migrating from.
    char parity = 'N';
    // 1 or 2. 1.5 exists in the DCB and on almost no real hardware.
    int stopBits = 1;
    // "none", "rtscts" or "xonxoff".
    std::string flow = "none";
    // Whether to raise DTR and RTS on open. Most adapters want this; a few
    // devices treat DTR as a reset line, which is why it is a setting and not
    // an assumption.
    bool dtr = true;
    bool rts = true;
    // 0 disables. On replug, the port is reopened when a device with the same
    // VID/PID reappears — the milestone's headline behaviour, and the reason
    // enumeration bothers with hardware ids at all.
    int maxReconnectAttempts = 5;
};

// A serial port (plan T56).
//
// Two worker threads, the same shape as ConptyBackend: a reader parked in
// ReadFile and a writer draining a queue. Not one thread, because a read that
// is waiting cannot also write; not zero, because rules/net.md forbids blocking
// IO on the UI thread and every serial call is blocking.
//
// The reader's ReadFile is bounded by COMMTIMEOUTS rather than left to hang:
// ReadIntervalTimeout and ReadTotalTimeoutMultiplier at MAXDWORD with a finite
// ReadTotalTimeoutConstant is the documented recipe for "return what is there,
// or wait up to N ms for the first byte". That N is also how quickly the reader
// notices it has been asked to stop.
class SerialBackend : public IBackend {
    Q_OBJECT

  public:
    explicit SerialBackend(SerialConfig config,
                           QObject* parent = nullptr);  // owned by parent
    ~SerialBackend() override;

    bool start(int cols, int rows) override;
    void writeInput(const QByteArray& bytes) override;
    // A serial line has no window size. Accepted and ignored, rather than
    // omitted, because the seam is what makes the backends interchangeable.
    void resize(int cols, int rows) override;
    void stop() override;

    bool isOpen() const { return m_open.load(); }

  public slots:
    // The line controls the milestone asks for. Each is a momentary or level
    // change on a wire, which is why they are actions rather than settings:
    // toggling DTR is how you reset an Arduino, and sending a break is how you
    // get a Cisco console's attention.
    void setDtr(bool on);
    void setRts(bool on);
    // Holds the line in break for `ms`, then clears it. A break is defined by
    // its duration, so a caller passing 0 gets nothing rather than a line left
    // permanently broken.
    void sendBreak(int ms);

  signals:
    void connected();
    // The device went away — usually because someone unplugged it. Distinct
    // from an error: on a USB adapter this is a normal Tuesday, and the
    // reconnect below is the whole point of the feature.
    void deviceRemoved();
    void reconnecting(int attempt, int ofAttempts, int delayMs);

  private:
    bool openPort(QString* whyNot);
    void closePort();
    void readerLoop();
    void writerLoop();
    // Waits for a port matching the one we lost to come back, then reopens.
    // Runs on its own thread so the wait never touches the GUI.
    void reconnectLoop();
    void fail(ErrorCode code, const QString& message);
    // EscapeCommFunction, with the constant checked rather than remembered:
    // SETRTS is 3 and SETDTR is 5, which is the transposition waiting to
    // happen.
    bool escape(unsigned long function);

    SerialConfig m_config;
    // What the port looked like when it opened, so a replug can be recognised
    // as the SAME adapter rather than whatever took the name.
    serial::PortInfo m_identity;

    void* m_handle = nullptr;  // HANDLE; void* so windows.h stays in the .cpp
    std::thread m_reader;
    std::thread m_writer;
    std::thread m_reconnect;
    std::mutex m_writeMutex;
    std::condition_variable m_writeCv;
    std::deque<QByteArray> m_writeQueue;
    std::atomic<bool> m_shutdown{false};
    std::atomic<bool> m_open{false};
    bool m_started = false;
};

}  // namespace krait::net

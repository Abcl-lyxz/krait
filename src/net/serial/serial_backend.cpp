#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "serial_backend.h"

#include <windows.h>

#include <QMetaObject>
#include <QTimer>

#include <array>
#include <chrono>
#include <utility>

namespace krait::net {
namespace {

// How long ReadFile waits for a first byte before returning empty-handed. Also
// the worst case for noticing a stop request, which is why it is not larger.
constexpr DWORD kReadTimeoutMs = 200;

// A write that cannot complete in this long is a line whose flow control is
// holding us off — a real state on a serial port, and not one to hang in.
constexpr DWORD kWriteTimeoutMs = 2000;

constexpr int kReadBufferBytes = 4096;

// How often the reconnect thread looks for the device to come back.
//
// ponytail: polling, not CM_Register_Notification. The callback API is the
// better answer and needs no window handle, but this only runs while
// DISCONNECTED — a second per attempt, for at most a few attempts — so it costs
// one enumeration a second during an interval that has nothing else happening.
// Upgrade path if the port list ever gets expensive to enumerate, or if instant
// reconnect starts to matter.
constexpr int kReplugPollMs = 1000;

// `\\.\COM10` is required past COM9 and legal for all of them, so it is used
// unconditionally rather than conditionally getting it wrong.
std::wstring devicePath(const std::string& port) {
    const std::wstring wide(port.begin(), port.end());
    return LR"(\\.\)" + wide;
}

}  // namespace

SerialBackend::SerialBackend(SerialConfig config, QObject* parent)
    : IBackend(parent), m_config(std::move(config)) {}

SerialBackend::~SerialBackend() {
    stop();
}

bool SerialBackend::openPort(QString* whyNot) {
    const HANDLE handle =
        ::CreateFileW(devicePath(m_config.port).c_str(), GENERIC_READ | GENERIC_WRITE,
                      // Zero share mode and OPEN_EXISTING are both REQUIRED for
                      // a communications resource, and hTemplateFile must be
                      // null. Exclusive access is the point: two programs on
                      // one serial line interleave bytes.
                      0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        *whyNot =
            error == ERROR_FILE_NOT_FOUND
                ? tr("There is no %1 on this machine.").arg(QString::fromStdString(m_config.port))
                : tr("Could not open %1 (%2). Another program may have it.")
                      .arg(QString::fromStdString(m_config.port))
                      .arg(error);
        return false;
    }

    // Filled from the CURRENT state first: SetCommState is documented as
    // needing a DCB that came from GetCommState, because the members this code
    // does not care about still have to hold sane values.
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (::GetCommState(handle, &dcb) == FALSE) {
        ::CloseHandle(handle);
        *whyNot =
            tr("%1 did not answer as a serial port.").arg(QString::fromStdString(m_config.port));
        return false;
    }
    dcb.BaudRate = static_cast<DWORD>(m_config.baud);
    dcb.ByteSize = static_cast<BYTE>(m_config.dataBits);
    // Windows does not support non-binary mode at all, and the documentation
    // says so outright: this member must be TRUE.
    dcb.fBinary = TRUE;
    switch (m_config.parity) {
    case 'E':
        dcb.Parity = EVENPARITY;
        break;
    case 'O':
        dcb.Parity = ODDPARITY;
        break;
    case 'M':
        dcb.Parity = MARKPARITY;
        break;
    case 'S':
        dcb.Parity = SPACEPARITY;
        break;
    default:
        dcb.Parity = NOPARITY;
        break;
    }
    dcb.fParity = dcb.Parity != NOPARITY ? TRUE : FALSE;
    dcb.StopBits = m_config.stopBits == 2 ? TWOSTOPBITS : ONESTOPBIT;

    const bool rtscts = m_config.flow == "rtscts";
    const bool xonxoff = m_config.flow == "xonxoff";
    dcb.fOutxCtsFlow = rtscts ? TRUE : FALSE;
    // HANDSHAKE hands the line to the driver — and then EscapeCommFunction is
    // documented as an ERROR to call, so the RTS action would silently stop
    // working. Only the non-handshake modes leave it ours to drive.
    dcb.fRtsControl =
        rtscts ? RTS_CONTROL_HANDSHAKE : (m_config.rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE);
    dcb.fDtrControl = m_config.dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    dcb.fOutX = xonxoff ? TRUE : FALSE;
    dcb.fInX = xonxoff ? TRUE : FALSE;
    // SetCommState is documented to FAIL when these two are equal, and a
    // zero-initialised DCB has them both at 0.
    dcb.XonChar = 0x11;
    dcb.XoffChar = 0x13;
    dcb.fAbortOnError = FALSE;

    if (::SetCommState(handle, &dcb) == FALSE) {
        ::CloseHandle(handle);
        *whyNot = tr("%1 rejected %2 baud %3%4%5.")
                      .arg(QString::fromStdString(m_config.port))
                      .arg(m_config.baud)
                      .arg(m_config.dataBits)
                      .arg(QChar::fromLatin1(m_config.parity))
                      .arg(m_config.stopBits);
        return false;
    }

    // The documented "return what is there, else wait for a first byte, else
    // time out" recipe. The alternative — MAXDWORD interval with zero totals —
    // returns instantly forever and spins a core.
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = kReadTimeoutMs;
    timeouts.WriteTotalTimeoutConstant = kWriteTimeoutMs;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    if (::SetCommTimeouts(handle, &timeouts) == FALSE) {
        ::CloseHandle(handle);
        *whyNot = tr("Could not set timeouts on %1.").arg(QString::fromStdString(m_config.port));
        return false;
    }

    ::PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    m_handle = handle;
    m_open = true;

    // Remembered while the device is present, because after it is gone there
    // is nothing left to ask.
    for (const serial::PortInfo& info : serial::enumeratePorts()) {
        if (info.name == m_config.port) {
            m_identity = info;
            break;
        }
    }
    return true;
}

bool SerialBackend::start(int /*cols*/, int /*rows*/) {
    if (m_started) {
        return true;
    }
    QString whyNot;
    if (!openPort(&whyNot)) {
        fail(ErrorCode::ConnectFailed, whyNot);
        return false;
    }
    m_started = true;
    m_shutdown = false;
    m_reader = std::thread([this] { readerLoop(); });
    m_writer = std::thread([this] { writerLoop(); });
    emit connected();
    return true;
}

void SerialBackend::readerLoop() {
    std::array<char, kReadBufferBytes> buffer{};
    while (!m_shutdown.load()) {
        if (!m_open.load()) {
            // Between a removal and a reconnect. Sleeping rather than spinning;
            // the reconnect thread will reopen and this picks it up.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        DWORD read = 0;
        if (::ReadFile(m_handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                       nullptr) == FALSE) {
            if (m_shutdown.load()) {
                return;
            }
            // A removed USB adapter fails every read from here on. This is the
            // replug path, not an error: on a USB serial adapter it is a normal
            // Tuesday, and a banner for something the next second fixes is a
            // banner people learn to dismiss without reading.
            const DWORD error = ::GetLastError();
            m_open = false;
            QMetaObject::invokeMethod(
                this,
                [this, error] {
                    qInfo("serial: %s went away (%lu)", m_config.port.c_str(), error);
                    emit deviceRemoved();
                    if (m_config.maxReconnectAttempts <= 0) {
                        fail(ErrorCode::PeerClosed,
                             tr("%1 is no longer connected.")
                                 .arg(QString::fromStdString(m_config.port)));
                        return;
                    }
                    if (m_reconnect.joinable()) {
                        m_reconnect.join();
                    }
                    m_reconnect = std::thread([this] { reconnectLoop(); });
                },
                Qt::QueuedConnection);
            continue;
        }
        if (read == 0) {
            continue;  // the timeout expired with nothing on the line
        }
        emit outputReceived(QByteArray(buffer.data(), static_cast<qsizetype>(read)));
    }
}

void SerialBackend::writerLoop() {
    while (true) {
        QByteArray chunk;
        {
            std::unique_lock<std::mutex> lock(m_writeMutex);
            m_writeCv.wait(lock, [this] { return m_shutdown.load() || !m_writeQueue.empty(); });
            if (m_shutdown.load()) {
                return;
            }
            chunk = m_writeQueue.front();
            m_writeQueue.pop_front();
        }
        if (!m_open.load()) {
            // Dropped rather than queued forever. Keystrokes typed at an
            // unplugged adapter are not owed delivery when it comes back — and
            // a queue that survived a reconnect would replay them into whatever
            // is on the other end now.
            continue;
        }
        DWORD written = 0;
        if (::WriteFile(m_handle, chunk.constData(), static_cast<DWORD>(chunk.size()), &written,
                        nullptr) == FALSE) {
            m_open = false;
        }
    }
}

void SerialBackend::reconnectLoop() {
    for (int attempt = 1; attempt <= m_config.maxReconnectAttempts && !m_shutdown.load();
         ++attempt) {
        QMetaObject::invokeMethod(
            this,
            [this, attempt] {
                emit reconnecting(attempt, m_config.maxReconnectAttempts, kReplugPollMs);
            },
            Qt::QueuedConnection);

        // Bounded, and interruptible: the sleep is split so stop() is noticed
        // within a tick rather than after a full second.
        for (int waited = 0; waited < kReplugPollMs && !m_shutdown.load(); waited += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (m_shutdown.load()) {
            return;
        }

        // The SAME adapter, by VID/PID where the device has them — not merely
        // something that took the name. Windows reuses COM numbers, so a
        // different adapter in the same hub can land on the port we lost, and
        // reopening that would connect the session to a device nobody chose.
        for (const serial::PortInfo& info : serial::enumeratePorts()) {
            if (!info.sameDevice(m_identity)) {
                continue;
            }
            m_config.port = info.name;  // it may come back on a different number
            QString whyNot;
            if (openPort(&whyNot)) {
                QMetaObject::invokeMethod(
                    this,
                    [this] {
                        qInfo("serial: %s is back", m_config.port.c_str());
                        emit connected();
                    },
                    Qt::QueuedConnection);
                return;
            }
            break;  // found it but could not open it; wait and try again
        }
    }
    if (m_shutdown.load()) {
        return;
    }
    QMetaObject::invokeMethod(
        this,
        [this] {
            fail(ErrorCode::PeerClosed,
                 tr("%1 did not come back.").arg(QString::fromStdString(m_config.port)));
        },
        Qt::QueuedConnection);
}

void SerialBackend::writeInput(const QByteArray& bytes) {
    if (bytes.isEmpty()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(m_writeMutex);
        m_writeQueue.push_back(bytes);
    }
    m_writeCv.notify_one();
}

void SerialBackend::resize(int /*cols*/, int /*rows*/) {}

bool SerialBackend::escape(unsigned long function) {
    if (!m_open.load()) {
        return false;
    }
    return ::EscapeCommFunction(m_handle, function) != FALSE;
}

void SerialBackend::setDtr(bool on) {
    escape(on ? SETDTR : CLRDTR);
}

void SerialBackend::setRts(bool on) {
    if (m_config.flow == "rtscts") {
        // Documented as an error while the driver owns the line for
        // handshaking. Refused here rather than issued and silently ignored.
        qWarning("serial: RTS is driven by hardware flow control on %s", m_config.port.c_str());
        return;
    }
    escape(on ? SETRTS : CLRRTS);
}

void SerialBackend::sendBreak(int ms) {
    if (ms <= 0 || !escape(SETBREAK)) {
        return;
    }
    // Cleared from a timer rather than by sleeping here: sendBreak is a slot,
    // and holding the caller for a quarter of a second to time a wire state is
    // exactly the blocking rules/ui.md forbids.
    QTimer::singleShot(ms, this, [this] { escape(CLRBREAK); });
}

void SerialBackend::closePort() {
    m_open = false;
    if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(m_handle);
        m_handle = nullptr;
    }
}

void SerialBackend::fail(ErrorCode code, const QString& message) {
    emit errorOccurred(errorCodeName(code), message);
}

void SerialBackend::stop() {
    if (m_shutdown.exchange(true)) {
        return;  // idempotent
    }
    m_writeCv.notify_all();
    // Closing the handle is what unblocks a reader parked in ReadFile — though
    // it would return within the read timeout anyway, which is why that timeout
    // is 200 ms and not something leisurely.
    closePort();
    for (std::thread* worker : {&m_reader, &m_writer, &m_reconnect}) {
        if (worker->joinable()) {
            worker->join();
        }
    }
    m_started = false;
}

}  // namespace krait::net

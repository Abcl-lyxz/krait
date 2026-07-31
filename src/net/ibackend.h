#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

namespace krait::net {

// The backend seam (CLAUDE.md): ssh, conpty, telnet, raw, serial all implement
// this. It is a QObject and OWNS the signal contract, rather than documenting
// one that each backend re-declares: with five backends coming, a re-declared
// signal is five chances for one of them to disagree about an argument, and a
// consumer that holds `IBackend*` connects to the base once for all of them.
//
// Output bytes are HOSTILE — they go straight into the VT core, never
// interpreted here.
class IBackend : public QObject {
    Q_OBJECT

  public:
    explicit IBackend(QObject* parent = nullptr) : QObject(parent) {}  // owned by parent

    // Spawns/connects. Returns false with errorOccurred emitted on failure.
    virtual bool start(int cols, int rows) = 0;
    // Application -> terminal input. Must never block the calling (UI)
    // thread (net.md); implementations queue to a writer.
    virtual void writeInput(const QByteArray& bytes) = 0;
    virtual void resize(int cols, int rows) = 0;
    // Idempotent; joins worker threads. Every wait has a timeout (net.md).
    virtual void stop() = 0;

  signals:
    // Emitted from worker threads, so every connection to it is queued.
    void outputReceived(const QByteArray& bytes);
    // `code` is an ErrorCode name from error.h — the per-tab banner key
    // (net.md: banner codes, never dialogs). `message` never carries secrets.
    void errorOccurred(const QString& code, const QString& message);
    // The remote side ended CLEANLY. Not an error: a shell that ran `exit` and
    // a connection that dropped are different events and must read differently.
    void exited(int exitCode);
};

}  // namespace krait::net

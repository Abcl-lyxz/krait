#pragma once

#include <QByteArray>

namespace krait::net {

// The backend seam (CLAUDE.md): ssh, conpty, telnet, raw, serial all
// implement this. Concrete backends are QObjects and additionally provide
// the signals `outputReceived(QByteArray)`, `errorOccurred(QString)` and
// `exited(int)` (queued to the GUI thread). Output bytes are HOSTILE — they
// go straight into the VT core, never interpreted here.
class IBackend {
  public:
    virtual ~IBackend() = default;

    // Spawns/connects. Returns false with errorOccurred emitted on failure.
    virtual bool start(int cols, int rows) = 0;
    // Application -> terminal input. Must never block the calling (UI)
    // thread (net.md); implementations queue to a writer.
    virtual void writeInput(const QByteArray& bytes) = 0;
    virtual void resize(int cols, int rows) = 0;
    // Idempotent; joins worker threads. Every wait has a timeout (net.md).
    virtual void stop() = 0;
};

}  // namespace krait::net

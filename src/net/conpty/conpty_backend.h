#pragma once

#include "../error.h"
#include "../ibackend.h"
#include <windows.h>

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace krait::net {

// Turns a configured shell command (empty = the default, PowerShell) into an
// absolute executable path and the command line to hand CreateProcessW. False
// means nothing by that name was found; `whyNot` is the banner detail.
//
// A free function rather than a ConptyBackend member because it depends on
// nothing but its argument — which is also what makes the security-relevant
// part of it (see the .cpp: the process search order is a binary-planting
// hole) testable without spawning a pseudoconsole.
bool resolveShellCommand(const QString& command, std::wstring* exePath, std::wstring* commandLine,
                         QString* whyNot);

// Local-shell backend over the BUNDLED ConPTY (ADR-0011: our pinned
// conpty.dll + OpenConsole.exe from third_party/openconsole, never the
// inbox one — startup fails loudly if the bundle is missing).
// Reader/writer run on their own threads; output reaches the GUI thread
// via the queued outputReceived signal.
class ConptyBackend : public IBackend {
    Q_OBJECT

  public:
    explicit ConptyBackend(QObject* parent = nullptr);  // owned by parent
    ~ConptyBackend() override;

    // The shell to spawn, from the session profile. Empty keeps the default
    // (PowerShell). Must be set before start(); changing it afterwards does
    // nothing, because the process is already running.
    //
    // This is LOCAL configuration, not remote input — but it still names an
    // executable, so resolveCommand() pins it to an absolute path rather than
    // letting the process search order pick one up (see the .cpp).
    void setCommand(const QString& command);

    bool start(int cols, int rows) override;
    void writeInput(const QByteArray& bytes) override;
    void resize(int cols, int rows) override;
    void stop() override;

  private:
    bool loadBundledConpty(QString* whyNot);
    void readerLoop();
    void writerLoop();
    void fail(ErrorCode code, const QString& message);
    // Frees everything a failed start() may have half-created. No worker
    // threads exist at that point, so ordering is unconstrained. Leaves the
    // object retryable (all members nulled).
    void abortStart();

    using CreateFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
    using ResizeFn = HRESULT(WINAPI*)(HPCON, COORD);
    using CloseFn = VOID(WINAPI*)(HPCON);

    HMODULE m_conptyDll = nullptr;
    CreateFn m_create = nullptr;
    ResizeFn m_resize = nullptr;
    CloseFn m_close = nullptr;

    HPCON m_pty = nullptr;
    HANDLE m_inWrite = INVALID_HANDLE_VALUE;  // we write child input here
    HANDLE m_outRead = INVALID_HANDLE_VALUE;  // we read child output here
    PROCESS_INFORMATION m_process{};
    LPPROC_THREAD_ATTRIBUTE_LIST m_attrList = nullptr;

    std::thread m_reader;
    std::thread m_writer;
    std::mutex m_writeMutex;
    std::condition_variable m_writeCv;
    std::deque<QByteArray> m_writeQueue;
    std::atomic<bool> m_shutdown{false};  // read by workers off-lock
    bool m_started = false;
    QString m_command;  // empty = the default shell
};

}  // namespace krait::net

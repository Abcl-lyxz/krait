#include "conpty_backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>

#include <vector>

namespace krait::net {

namespace {
constexpr DWORD kReadChunk = 16 * 1024;
// Short: stop() runs on the GUI thread for M0 (single window). Async
// teardown with a cancel path is the M1 tab-close debt (net.md).
constexpr DWORD kProcessWaitMs = 500;
}  // namespace

ConptyBackend::ConptyBackend(QObject* parent) : IBackend(parent) {}

ConptyBackend::~ConptyBackend() {
    stop();
}

void ConptyBackend::fail(ErrorCode code, const QString& message) {
    emit errorOccurred(errorCodeName(code), message);
}

bool ConptyBackend::loadBundledConpty(QString* whyNot) {
    const QString dir = QCoreApplication::applicationDirPath() + "/openconsole";
    const QString dll = dir + "/conpty.dll";
    if (!QFile::exists(dll)) {
        *whyNot = "bundled conpty.dll not found at " + dll;
        return false;
    }
    m_conptyDll = LoadLibraryW(reinterpret_cast<const wchar_t*>(dll.utf16()));
    if (m_conptyDll == nullptr) {
        *whyNot = "LoadLibrary failed for " + dll;
        return false;
    }
    m_create = reinterpret_cast<CreateFn>(GetProcAddress(m_conptyDll, "ConptyCreatePseudoConsole"));
    m_resize = reinterpret_cast<ResizeFn>(GetProcAddress(m_conptyDll, "ConptyResizePseudoConsole"));
    m_close = reinterpret_cast<CloseFn>(GetProcAddress(m_conptyDll, "ConptyClosePseudoConsole"));
    if (m_create == nullptr || m_resize == nullptr || m_close == nullptr) {
        *whyNot = "bundled conpty.dll is missing Conpty* exports";
        return false;
    }
    return true;
}

void ConptyBackend::abortStart() {
    if (m_process.hProcess != nullptr) {
        TerminateProcess(m_process.hProcess, 1);
        CloseHandle(m_process.hProcess);
        CloseHandle(m_process.hThread);
        m_process = {};
    }
    if (m_close != nullptr && m_pty != nullptr) {
        m_close(m_pty);
        m_pty = nullptr;
    }
    if (m_outRead != INVALID_HANDLE_VALUE) {
        CloseHandle(m_outRead);
        m_outRead = INVALID_HANDLE_VALUE;
    }
    if (m_inWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(m_inWrite);
        m_inWrite = INVALID_HANDLE_VALUE;
    }
    if (m_attrList != nullptr) {
        DeleteProcThreadAttributeList(m_attrList);
        HeapFree(GetProcessHeap(), 0, m_attrList);
        m_attrList = nullptr;
    }
    if (m_conptyDll != nullptr) {
        FreeLibrary(m_conptyDll);
        m_conptyDll = nullptr;
        m_create = nullptr;
        m_resize = nullptr;
        m_close = nullptr;
    }
}

bool ConptyBackend::start(int cols, int rows) {
    if (m_started) {
        return true;
    }
    QString whyNot;
    if (!loadBundledConpty(&whyNot)) {
        // ADR-0011: bundled or nothing — never silently use inbox conpty.
        abortStart();
        fail(ErrorCode::PtyCreateFailed, whyNot);
        return false;
    }

    HANDLE inRead = INVALID_HANDLE_VALUE;
    HANDLE outWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&inRead, &m_inWrite, nullptr, 0)) {
        abortStart();
        fail(ErrorCode::PtyCreateFailed, "CreatePipe (input) failed");
        return false;
    }
    if (!CreatePipe(&m_outRead, &outWrite, nullptr, 0)) {
        CloseHandle(inRead);
        abortStart();
        fail(ErrorCode::PtyCreateFailed, "CreatePipe (output) failed");
        return false;
    }

    const COORD size{static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    const HRESULT hr = m_create(size, inRead, outWrite, 0, &m_pty);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (FAILED(hr)) {
        m_pty = nullptr;
        abortStart();
        fail(ErrorCode::PtyCreateFailed,
             QString::asprintf("ConptyCreatePseudoConsole hr=0x%08lx", hr));
        return false;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    m_attrList =
        static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, attrSize));
    if (m_attrList == nullptr || !InitializeProcThreadAttributeList(m_attrList, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(m_attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, m_pty,
                                   sizeof(m_pty), nullptr, nullptr)) {
        abortStart();
        fail(ErrorCode::SpawnFailed, "proc-thread attribute setup failed");
        return false;
    }
    si.lpAttributeList = m_attrList;
    // Null std handles + USESTDHANDLES: without this the child can pick up
    // the parent's redirected std pipes and bypass the pty entirely.
    si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nullptr;
    si.StartupInfo.hStdOutput = nullptr;
    si.StartupInfo.hStdError = nullptr;

    // Absolute path: a bare "powershell.exe" would search the app/current
    // directory first (binary planting, net.md hostile-input posture).
    wchar_t psPath[MAX_PATH];
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
                              psPath, MAX_PATH);
    wchar_t cmdline[] = L"powershell.exe -NoLogo";
    if (!CreateProcessW(psPath, cmdline, nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, nullptr, &si.StartupInfo, &m_process)) {
        abortStart();
        fail(ErrorCode::SpawnFailed,
             QString::asprintf("CreateProcess failed (%lu)", GetLastError()));
        return false;
    }

    m_shutdown = false;
    m_reader = std::thread([this] { readerLoop(); });
    m_writer = std::thread([this] { writerLoop(); });
    SetThreadDescription(m_reader.native_handle(), L"krait-conpty-reader");
    SetThreadDescription(m_writer.native_handle(), L"krait-conpty-writer");
    m_started = true;
    return true;
}

void ConptyBackend::readerLoop() {
    std::vector<char> buffer(kReadChunk);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(m_outRead, buffer.data(), kReadChunk, &read, nullptr) || read == 0) {
            break;  // pipe closed: child gone or stop() in progress
        }
        const QByteArray chunk(buffer.data(), static_cast<qsizetype>(read));
        // Queued: lands on the GUI thread that owns this object.
        QMetaObject::invokeMethod(
            this, [this, chunk] { emit outputReceived(chunk); }, Qt::QueuedConnection);
    }
    if (m_shutdown) {
        return;  // user-initiated stop() owns process teardown/reporting
    }
    DWORD exitCode = 0;
    bool childGone = true;
    if (m_process.hProcess != nullptr) {
        // Whether the CHILD is gone decides which of these this was. A pipe
        // that closed while the shell is still running means the pty died
        // under us — conhost killed, or the handle broken from outside — and
        // that is a failure the user has to be told about. A pipe that closed
        // because the shell exited is not an error at all, and reporting one
        // is how a banner teaches people to ignore banners.
        childGone = WaitForSingleObject(m_process.hProcess, kProcessWaitMs) == WAIT_OBJECT_0;
        GetExitCodeProcess(m_process.hProcess, &exitCode);
    }
    if (!childGone) {
        // Queued, like every other emission from this thread: fail() emits
        // directly and its other callers are on the GUI thread, but this one
        // is not.
        const QString message =
            tr("The console host closed unexpectedly. The session is over; the shell may "
               "still be running.");
        QMetaObject::invokeMethod(
            this, [this, message] { fail(ErrorCode::PeerClosed, message); }, Qt::QueuedConnection);
        return;
    }
    QMetaObject::invokeMethod(
        this, [this, exitCode] { emit exited(static_cast<int>(exitCode)); }, Qt::QueuedConnection);
}

void ConptyBackend::writerLoop() {
    for (;;) {
        QByteArray bytes;
        {
            std::unique_lock lock(m_writeMutex);
            m_writeCv.wait(lock, [this] { return m_shutdown || !m_writeQueue.empty(); });
            if (m_shutdown) {
                return;
            }
            bytes = std::move(m_writeQueue.front());
            m_writeQueue.pop_front();
        }
        DWORD written = 0;
        if (!WriteFile(m_inWrite, bytes.constData(), static_cast<DWORD>(bytes.size()), &written,
                       nullptr)) {
            return;  // pipe gone or write canceled during stop()
        }
    }
}

void ConptyBackend::writeInput(const QByteArray& bytes) {
    {
        const std::lock_guard lock(m_writeMutex);
        m_writeQueue.push_back(bytes);
    }
    m_writeCv.notify_one();
}

void ConptyBackend::resize(int cols, int rows) {
    if (m_pty != nullptr && m_resize != nullptr) {
        m_resize(m_pty, COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
    }
}

void ConptyBackend::stop() {
    if (!m_started) {
        return;
    }
    m_started = false;
    {
        const std::lock_guard lock(m_writeMutex);
        m_shutdown = true;
    }
    m_writeCv.notify_all();

    // Order matters (review finding): unblock and JOIN the workers before
    // any handle they use is closed — closed handle values recycle.
    if (m_close != nullptr && m_pty != nullptr) {
        m_close(m_pty);  // pty teardown EOFs the output pipe for the reader
        m_pty = nullptr;
    }
    if (m_outRead != INVALID_HANDLE_VALUE) {
        CancelIoEx(m_outRead, nullptr);
    }
    if (m_writer.joinable()) {
        CancelSynchronousIo(m_writer.native_handle());  // a stuck WriteFile
        m_writer.join();
    }
    if (m_reader.joinable()) {
        m_reader.join();
    }

    if (m_process.hProcess != nullptr) {
        if (WaitForSingleObject(m_process.hProcess, kProcessWaitMs) == WAIT_TIMEOUT) {
            TerminateProcess(m_process.hProcess, 1);
        }
        CloseHandle(m_process.hProcess);
        CloseHandle(m_process.hThread);
        m_process = {};
    }
    if (m_outRead != INVALID_HANDLE_VALUE) {
        CloseHandle(m_outRead);
        m_outRead = INVALID_HANDLE_VALUE;
    }
    if (m_inWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(m_inWrite);
        m_inWrite = INVALID_HANDLE_VALUE;
    }
    if (m_attrList != nullptr) {
        DeleteProcThreadAttributeList(m_attrList);
        HeapFree(GetProcessHeap(), 0, m_attrList);
        m_attrList = nullptr;
    }
    if (m_conptyDll != nullptr) {
        FreeLibrary(m_conptyDll);
        m_conptyDll = nullptr;
        m_create = nullptr;
        m_resize = nullptr;
        m_close = nullptr;
    }
}

}  // namespace krait::net

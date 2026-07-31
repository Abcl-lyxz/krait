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

void ConptyBackend::setCommand(const QString& command) {
    m_command = command;
}

namespace {

// %VAR% expansion. Returns false on failure rather than silently handing the
// unexpanded string on, which would try to launch a file literally named
// "%LOCALAPPDATA%\...".
bool expandEnv(const std::wstring& text, std::wstring* out) {
    // Heap, not the stack: the documented ceiling on an expansion is 32K chars,
    // and a 64 KB local in a function called on the GUI thread is not a trade
    // worth making for one string built once per session.
    std::vector<wchar_t> buffer(MAX_PATH);
    for (int attempt = 0; attempt < 2; ++attempt) {
        // The count INCLUDES the terminating null on success AND when the
        // buffer was too small (learn.microsoft.com), so comparing it with the
        // buffer size is the only way to tell those apart — the return value
        // alone cannot.
        const DWORD written = ExpandEnvironmentStringsW(text.c_str(), buffer.data(),
                                                        static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return false;
        }
        if (written <= buffer.size()) {
            *out = buffer.data();
            return true;
        }
        buffer.resize(written);
    }
    return false;
}

// Splits "C:\path with space\sh.exe -l" or "\"C:\p\sh.exe\" -l" into the
// executable and the rest. A leading quote wins, which is the only way to name
// a path containing a space.
void splitCommand(const std::wstring& raw, std::wstring* exe, std::wstring* args) {
    if (!raw.empty() && raw.front() == L'"') {
        const std::size_t close = raw.find(L'"', 1);
        if (close != std::wstring::npos) {
            *exe = raw.substr(1, close - 1);
            *args = raw.substr(close + 1);
            return;
        }
        // Unterminated quote: treat the whole thing as the path. Guessing where
        // the argument list starts would be worse than failing to find it.
        *exe = raw.substr(1);
        args->clear();
        return;
    }
    const std::size_t space = raw.find(L' ');
    *exe = raw.substr(0, space);
    *args = space == std::wstring::npos ? std::wstring() : raw.substr(space);
}

}  // namespace

bool resolveShellCommand(const QString& command, std::wstring* exePath, std::wstring* commandLine,
                         QString* whyNot) {
    // QCoreApplication::translate at each site, with the class's context because
    // this is a free function and tr() is not available. NOT wrapped in a local
    // helper: lupdate only extracts literals passed straight to the call, and a
    // one-line `text(...)` lambda would hide every one of these from it —
    // the same mistake error_banner.h documents costing M1 eight strings.
    const QString wanted = command.trimmed();
    if (wanted.isEmpty()) {
        if (!expandEnv(L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
                       exePath)) {
            *whyNot = QCoreApplication::translate(
                "krait::net::ConptyBackend", "Could not expand %SystemRoot% to find PowerShell.");
            return false;
        }
        *commandLine = L"\"" + *exePath + L"\" -NoLogo";
        return true;
    }

    std::wstring exe;
    std::wstring args;
    splitCommand(wanted.toStdWString(), &exe, &args);
    std::wstring expanded;
    if (exe.empty() || !expandEnv(exe, &expanded)) {
        *whyNot = QCoreApplication::translate("krait::net::ConptyBackend",
                                              "Could not expand the configured shell command.");
        return false;
    }

    // net.md's hostile-input posture, applied to LOCAL config because the hole
    // is the same shape: CreateProcessW's own search (the one it uses when
    // lpApplicationName is null) looks in the calling process's directory and
    // then the CURRENT directory before System32, so a planted powershell.exe
    // beside a downloaded file wins. Microsoft's documented mitigation is to
    // resolve the path ourselves and pass it as lpApplicationName, quoted in
    // the command line — which is what the rest of this function does.
    //
    // SearchPathW has the SAME default weakness (current directory first,
    // governed by the SafeProcessSearchMode registry value, which defaults to
    // 0), and SetSearchPathMode is what turns it off per process. PERMANENT
    // because nothing in this process ever wants it back on; it is idempotent
    // per process, hence the function-local static.
    static const bool safeSearch = SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE |
                                                     BASE_SEARCH_PATH_PERMANENT) != FALSE;
    if (!safeSearch) {
        // Not fatal — an absolute path does not need the search at all — but it
        // is exactly the kind of thing that must never be silent.
        qWarning("conpty: safe search mode unavailable (%lu); relative shell commands will "
                 "search the current directory first",
                 GetLastError());
    }

    wchar_t resolved[MAX_PATH] = {};
    // On success this returns the length WITHOUT the null; when the buffer is
    // too small it returns the size WITH it. Either way >= the buffer means we
    // did not get a usable path.
    const DWORD length =
        SearchPathW(nullptr, expanded.c_str(), L".exe", MAX_PATH, resolved, nullptr);
    if (length == 0 || length >= MAX_PATH) {
        *whyNot = QCoreApplication::translate("krait::net::ConptyBackend", "Could not find %1.")
                      .arg(QString::fromStdWString(expanded));
        return false;
    }
    *exePath = resolved;
    *commandLine = L"\"" + *exePath + L"\"" + args;
    return true;
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

    std::wstring exePath;
    std::wstring commandLine;
    QString whyNoCommand;
    if (!resolveShellCommand(m_command, &exePath, &commandLine, &whyNoCommand)) {
        abortStart();
        fail(ErrorCode::SpawnFailed, whyNoCommand);
        return false;
    }
    // commandLine.data() and not a literal: CreateProcessW is documented to
    // MODIFY the buffer it is given, so a string literal there is an access
    // violation waiting for the right Windows build.
    if (!CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo,
                        &m_process)) {
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

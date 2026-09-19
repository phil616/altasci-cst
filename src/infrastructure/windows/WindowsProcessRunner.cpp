#include "WindowsPlatform.h"
#include "Win32Support.h"
#include "domain/Configuration.h"
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QStringDecoder>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace cst {
namespace {
class Attributes {
public:
    Attributes() {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size); storage_.resize(size);
        value_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(value_, 1, 0, &size)) win::fail("InitializeProcThreadAttributeList");
    }
    ~Attributes() { DeleteProcThreadAttributeList(value_); }
    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return value_; }
private:
    std::vector<BYTE> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST value_ = nullptr;
};
class PseudoConsole {
public:
    HPCON value = nullptr;
    ~PseudoConsole() { close(); }
    void close() { if (const auto handle = std::exchange(value, nullptr)) ClosePseudoConsole(handle); }
};
class ManagedProcess final : public IManagedProcess {
public:
    ManagedProcess(const ProcessSpec &spec, QString helper, std::function<void(ProcessOutput)> output)
        : spec_(spec), helper_(std::move(helper)), output_(std::move(output)) {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        win::Handle outWrite, errWrite, inputRead;
        const auto pipe = [&](win::Handle &read, win::Handle &write) {
            HANDLE r = nullptr, w = nullptr;
            if (!CreatePipe(&r, &w, &security, 0)) win::fail("CreatePipe");
            read.reset(r); write.reset(w);
        };
        pipe(stdout_, outWrite);
        if (!SetHandleInformation(stdout_.get(), HANDLE_FLAG_INHERIT, 0)) win::fail("stdout inheritance");
        if (spec.inputEnabled || spec.terminal) {
            pipe(inputRead, input_);
            if (!SetHandleInformation(input_.get(), HANDLE_FLAG_INHERIT, 0)) win::fail("stdin inheritance");
        } else {
            inputRead.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
            if (!inputRead) win::fail("CreateFile(NUL)");
        }
        Attributes attributes;
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes.get();
        DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
        if (spec.terminal) {
            const auto status = CreatePseudoConsole({100, 30}, inputRead.get(), outWrite.get(), 0, &console_.value);
            if (FAILED(status)) win::fail("CreatePseudoConsole", static_cast<DWORD>(status));
            if (!UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                           console_.value, sizeof(HPCON), nullptr, nullptr)) win::fail("Pseudoconsole attribute");
            stderrDone_.store(true);
        } else {
            pipe(stderr_, errWrite);
            if (!SetHandleInformation(stderr_.get(), HANDLE_FLAG_INHERIT, 0)) win::fail("stderr inheritance");
            HANDLE inherited[] = {outWrite.get(), errWrite.get(), inputRead.get()};
            if (!UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) win::fail("Inherited handle list");
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            startup.StartupInfo.wShowWindow = SW_HIDE;
            startup.StartupInfo.hStdOutput = outWrite.get(); startup.StartupInfo.hStdError = errWrite.get(); startup.StartupInfo.hStdInput = inputRead.get();
            flags |= CREATE_NEW_CONSOLE;
        }
        job_.reset(CreateJobObjectW(nullptr, nullptr)); if (!job_) win::fail("CreateJobObject");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) win::fail("Job kill-on-close");
        auto command = spec.shellCommandLine.isEmpty() ? windowsCommandLine(spec.program, spec.arguments) : quoteWindowsArgument(spec.program) + ' ' + spec.shellCommandLine;
        if (command.size() >= 32767 || command.contains(QChar::Null)) throw std::invalid_argument("无效或过长的 Windows 命令行");
        auto environment = environmentBlock(spec.environment); PROCESS_INFORMATION information{};
        if (!CreateProcessW(win::wide(spec.program), reinterpret_cast<wchar_t *>(command.data()), nullptr, nullptr, spec.terminal ? FALSE : TRUE,
                            flags, environment.data(), win::wide(spec.workingDirectory), &startup.StartupInfo, &information)) win::fail("CreateProcess：" + spec.program);
        process_.reset(information.hProcess); win::Handle thread(information.hThread); pid_ = information.dwProcessId;
        if (!AssignProcessToJobObject(job_.get(), process_.get())) {
            const auto error = GetLastError(); TerminateProcess(process_.get(), win::forcedExit); WaitForSingleObject(process_.get(), 5000);
            win::fail("AssignProcessToJobObject", error);
        }
        try {
            outThread_ = std::thread([this] { read(stdout_.get(), spec_.terminal ? "terminal" : "stdout", stdoutDone_); });
            if (!spec.terminal) errThread_ = std::thread([this] { read(stderr_.get(), "stderr", stderrDone_); });
            if (input_) inputThread_ = std::thread([this] { inputLoop(); });
            if (ResumeThread(thread.get()) == DWORD(-1)) win::fail("ResumeThread");
        } catch (...) {
            TerminateJobObject(job_.get(), win::forcedExit); console_.close(); shutdownReaders_.store(true); finishThreads(); throw;
        }
        outWrite.reset(); errWrite.reset(); inputRead.reset();
    }
    ~ManagedProcess() override {
        try { forceStop(); } catch (...) { if (job_) TerminateJobObject(job_.get(), win::forcedExit); }
        console_.close(); shutdownReaders_.store(true); finishThreads();
    }
    quint32 rootPid() const override { return pid_; }
    bool rootRunning() const override { return WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT; }
    bool treeEmpty() const override {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        if (!QueryInformationJobObject(job_.get(), JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr)) win::fail("Query job state");
        return info.ActiveProcesses == 0;
    }
    bool empty() const override { return treeEmpty(); }
    QList<quint32> processIds() const override {
        DWORD capacity = 32;
        for (;;) {
            const auto size = sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + sizeof(ULONG_PTR) * capacity;
            std::vector<BYTE> buffer(size); auto *list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST *>(buffer.data());
            if (QueryInformationJobObject(job_.get(), JobObjectBasicProcessIdList, list, static_cast<DWORD>(size), nullptr)) {
                QList<quint32> result; for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) result.append(static_cast<quint32>(list->ProcessIdList[i])); return result;
            }
            if (GetLastError() != ERROR_MORE_DATA) win::fail("Query job process IDs");
            if (capacity > 1048576) throw std::runtime_error("Job process list exceeds limit");
            capacity *= 2;
        }
    }
    std::optional<ProcessResult> result() const override {
        if (rootRunning()) return std::nullopt;
        DWORD code = 0; if (!GetExitCodeProcess(process_.get(), &code)) win::fail("GetExitCodeProcess");
        return ProcessResult{static_cast<qint32>(code), code >= 0xC0000000u};
    }
    void writeInput(const QByteArray &bytes) override {
        if (!input_ || treeEmpty()) throw std::runtime_error("任务输入已关闭");
        std::lock_guard lock(inputMutex_);
        if (inputStopping_ || inputSize_ + bytes.size() > 65536) throw std::runtime_error("任务输入缓冲已满或正在停止");
        inputQueue_.push_back(bytes); inputSize_ += bytes.size(); inputWake_.notify_one();
    }
    void resizeTerminal(int columns, int rows) override {
        std::lock_guard lock(stopMutex_);
        if (console_.value) {
            const auto status = ResizePseudoConsole(console_.value, {static_cast<SHORT>(std::clamp(columns, 2, 500)), static_cast<SHORT>(std::clamp(rows, 2, 200))});
            if (FAILED(status)) win::fail("ResizePseudoConsole", static_cast<DWORD>(status));
        }
    }
    void stop(int graceMs) override {
        if (!treeEmpty() && graceMs > 0) {
            if (spec_.terminal) { try { writeInput(QByteArray(1, '\x03')); } catch (...) {} }
            else if (!helper_.isEmpty()) {
                const auto pids = processIds();
                if (!pids.isEmpty()) {
                    WindowsProcessRunner runner({});
                    auto helper = runner.start({helper_, {QString::number(pids.first())}, QFileInfo(helper_).absolutePath(), spec_.environment, {}, {}, {}, {}}, {});
                    const auto until = GetTickCount64() + 2000;
                    while (!helper->empty() && GetTickCount64() < until) QThread::msleep(10);
                    helper->forceStop();
                }
            }
            const auto until = GetTickCount64() + static_cast<ULONGLONG>(graceMs);
            while (!treeEmpty() && GetTickCount64() < until) QThread::msleep(10);
        }
        forceStop();
    }
    void forceStop() override {
        std::lock_guard lock(stopMutex_);
        if (!treeEmpty() && !TerminateJobObject(job_.get(), win::forcedExit)) win::fail("TerminateJobObject");
        const auto until = GetTickCount64() + 5000;
        while (!treeEmpty() && GetTickCount64() < until) QThread::msleep(10);
        if (!treeEmpty()) throw std::runtime_error("停止超时：Job 中仍有进程");
        // The output reader remains active while ClosePseudoConsole drains its final frame.
        console_.close();
        const auto drainUntil = GetTickCount64() + 2000;
        while ((!stdoutDone_.load() || !stderrDone_.load()) && GetTickCount64() < drainUntil) QThread::msleep(5);
        shutdownReaders_.store(true);
        finishThreads();
    }
private:
    void finishThreads() {
        { std::lock_guard lock(inputMutex_); inputStopping_ = true; inputWake_.notify_all(); }
        if (inputThread_.joinable()) {
            CancelSynchronousIo(inputThread_.native_handle()); inputThread_.join();
        }
        if (outThread_.joinable()) outThread_.join();
        if (errThread_.joinable()) errThread_.join();
    }
    void inputLoop() {
        for (;;) {
            QByteArray bytes;
            { std::unique_lock lock(inputMutex_); inputWake_.wait(lock, [this] { return inputStopping_ || !inputQueue_.empty(); });
              if (inputStopping_) return;
              bytes = std::move(inputQueue_.front()); inputQueue_.pop_front(); inputSize_ -= bytes.size(); }
            qsizetype offset = 0;
            while (offset < bytes.size()) { DWORD count = 0;
                if (!WriteFile(input_.get(), bytes.constData() + offset, static_cast<DWORD>(bytes.size() - offset), &count, nullptr) || count == 0) return;
                offset += count;
            }
        }
    }
    void read(HANDLE pipe, const QString &channel, std::atomic_bool &done) noexcept {
        QStringDecoder decoder(QStringDecoder::Utf8);
        QByteArray pending; char buffer[8192]; ULONGLONG lastDelivery = GetTickCount64();
        const auto deliver = [&](const QByteArray &bytes) {
            ProcessOutput item; item.channel = channel; item.timestamp = QDateTime::currentDateTimeUtc();
            item.bytes = bytes; item.projectId = spec_.projectId; item.runId = spec_.operationId; item.attemptId = spec_.attemptId;
            if (spec_.terminal || spec_.encoding == "utf-8") { item.text = decoder(bytes); item.decodeError = decoder.hasError(); }
            else {
                const auto cp = spec_.encoding == "oem" ? GetOEMCP() : GetACP();
                const auto n = MultiByteToWideChar(cp, 0, bytes.constData(), static_cast<int>(bytes.size()), nullptr, 0);
                item.text.resize(n); if (n) MultiByteToWideChar(cp, 0, bytes.constData(), static_cast<int>(bytes.size()), reinterpret_cast<wchar_t *>(item.text.data()), n);
            }
            if (output_) { try { output_(std::move(item)); } catch (...) {} }
        };
        while (!shutdownReaders_.load()) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
            if (available) {
                DWORD count = 0;
                if (!ReadFile(pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) || !count) break;
                if (spec_.terminal) deliver(QByteArray(buffer, static_cast<qsizetype>(count)));
                else {
                    pending.append(buffer, static_cast<qsizetype>(count)); qsizetype newline;
                    while ((newline = pending.indexOf('\n')) >= 0) { deliver(pending.left(newline + 1)); pending.remove(0, newline + 1); lastDelivery = GetTickCount64(); }
                }
            } else QThread::msleep(5);
            if (!pending.isEmpty() && (pending.size() >= 8192 || GetTickCount64() - lastDelivery >= 50)) { deliver(pending); pending.clear(); lastDelivery = GetTickCount64(); }
        }
        if (!pending.isEmpty()) deliver(pending);
        done.store(true);
    }
    ProcessSpec spec_;
    QString helper_;
    std::function<void(ProcessOutput)> output_;
    win::Handle job_, process_, stdout_, stderr_, input_;
    PseudoConsole console_;
    quint32 pid_ = 0;
    std::thread outThread_, errThread_, inputThread_;
    std::atomic_bool shutdownReaders_{false}, stdoutDone_{false}, stderrDone_{false};
    std::mutex stopMutex_, inputMutex_;
    std::condition_variable inputWake_;
    std::deque<QByteArray> inputQueue_;
    qsizetype inputSize_ = 0;
    bool inputStopping_ = false;
};
}
WindowsProcessRunner::WindowsProcessRunner(QString helper) : signalHelper_(std::move(helper)) {
    const auto block = GetEnvironmentStringsW(); if (!block) win::fail("GetEnvironmentStrings");
    for (const wchar_t *entry = block; *entry; entry += wcslen(entry) + 1) {
        const auto text = QString::fromWCharArray(entry); const auto separator = text.indexOf('=', text.startsWith('=') ? 1 : 0);
        if (separator > 0) inherited_.insert(text.left(separator).toUpper(), text.mid(separator + 1));
    }
    FreeEnvironmentStringsW(block);
}
std::shared_ptr<IManagedProcess> WindowsProcessRunner::start(const ProcessSpec &spec, std::function<void(ProcessOutput)> output) {
    if (!isWindowsAbsolutePath(spec.program) || !spec.program.endsWith(".exe", Qt::CaseInsensitive))
        throw std::invalid_argument("托管进程需要原生 EXE 绝对路径；批处理请使用 shell 模式");
    return std::make_shared<ManagedProcess>(spec, signalHelper_, std::move(output));
}
Environment WindowsProcessRunner::inheritedEnvironment() const { return inherited_; }
QString WindowsProcessRunner::resolveExecutable(const QString &program, const QStringList &tools) const {
    auto environment = inherited_; environment["PATH"] = tools.join(';') + ';' + environment.value("PATH");
    return resolveInEnvironment(program, environment, {});
}
QString WindowsProcessRunner::resolveInEnvironment(const QString &program, const Environment &environment, const QString &cwd) const {
    if (program.isEmpty() || program.contains(QChar::Null)) throw std::invalid_argument("程序名为空或包含 NUL");
    if (program.endsWith(".cmd", Qt::CaseInsensitive) || program.endsWith(".bat", Qt::CaseInsensitive))
        throw std::invalid_argument("批处理请使用 shell 模式；npm 脚本请选择 npm 模式");
    QString candidate = program; candidate.replace('\\', '/');
    if (!isWindowsAbsolutePath(candidate) && candidate.contains('/')) candidate = QDir(cwd).absoluteFilePath(candidate);
    if (isWindowsAbsolutePath(candidate)) {
        if (QFileInfo(candidate).isFile()) return QDir::toNativeSeparators(QFileInfo(candidate).absoluteFilePath());
        throw std::runtime_error(("可执行文件不存在：" + candidate).toUtf8().constData());
    }
    const auto name = candidate.contains('.') ? candidate : candidate + ".exe";
    for (auto directory : environment.value("PATH").split(';', Qt::SkipEmptyParts)) {
        if (directory.startsWith('"') && directory.endsWith('"')) directory = directory.mid(1, directory.size() - 2);
        if (!isWindowsAbsolutePath(directory)) continue;
        const QFileInfo file(QDir(directory).filePath(name));
        if (file.isFile()) return QDir::toNativeSeparators(file.absoluteFilePath());
    }
    throw std::runtime_error(("在命令的最终 PATH 中找不到程序：" + program).toUtf8().constData());
}
}

#include "WindowsPlatform.h"
#include "Win32Support.h"
#include "domain/Configuration.h"
#include <QDir>
#include <QFileInfo>
#include <QStringDecoder>
#include <QThread>
#include <mutex>
#include <thread>
#include <vector>

namespace cst {
namespace {
class Attributes {
public:
    Attributes() {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage_.resize(size);
        value_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(value_, 1, 0, &size)) win::fail("InitializeProcThreadAttributeList");
    }
    ~Attributes() { DeleteProcThreadAttributeList(value_); }
    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return value_; }
private:
    std::vector<BYTE> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST value_ = nullptr;
};
class ManagedProcess final : public IManagedProcess {
public:
    ManagedProcess(const ProcessSpec &spec, QString helper, std::function<void(ProcessOutput)> output)
        : helper_(std::move(helper)), environment_(spec.environment), output_(std::move(output)) {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
        if (!CreatePipe(&outRead, &outWrite, &security, 0)) win::fail("CreatePipe(stdout)");
        stdout_.reset(outRead); win::Handle stdoutWrite(outWrite);
        if (!CreatePipe(&errRead, &errWrite, &security, 0)) win::fail("CreatePipe(stderr)");
        stderr_.reset(errRead); win::Handle stderrWrite(errWrite);
        if (!SetHandleInformation(stdout_.get(), HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(stderr_.get(), HANDLE_FLAG_INHERIT, 0)) win::fail("SetHandleInformation");
        win::Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
        if (!input) win::fail("CreateFile(NUL)");
        job_.reset(CreateJobObjectW(nullptr, nullptr)); if (!job_) win::fail("CreateJobObject");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) win::fail("JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE");
        completion_.reset(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
        if (!completion_) win::fail("CreateIoCompletionPort");
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{this, completion_.get()};
        if (!SetInformationJobObject(job_.get(), JobObjectAssociateCompletionPortInformation, &association, sizeof(association))) win::fail("Job completion port");
        Attributes attributes;
        HANDLE inherited[] = {stdoutWrite.get(), stderrWrite.get(), input.get()};
        if (!UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) win::fail("Inherited handle list");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdOutput = stdoutWrite.get(); startup.StartupInfo.hStdError = stderrWrite.get(); startup.StartupInfo.hStdInput = input.get();
        startup.lpAttributeList = attributes.get();
        auto command = spec.shellCommandLine.isEmpty() ? windowsCommandLine(spec.program, spec.arguments) : quoteWindowsArgument(spec.program) + ' ' + spec.shellCommandLine;
        if (command.size() >= 32767) throw std::invalid_argument("Windows 命令行超过 32766 字符");
        auto environment = environmentBlock(spec.environment);
        PROCESS_INFORMATION information{};
        if (!CreateProcessW(win::wide(spec.program), reinterpret_cast<wchar_t *>(command.data()), nullptr, nullptr, TRUE,
                            CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE | EXTENDED_STARTUPINFO_PRESENT,
                            environment.data(), win::wide(spec.workingDirectory), &startup.StartupInfo, &information)) win::fail("CreateProcess：" + spec.program);
        process_.reset(information.hProcess); win::Handle thread(information.hThread); pid_ = information.dwProcessId;
        if (!AssignProcessToJobObject(job_.get(), process_.get())) {
            const auto error = GetLastError(); TerminateProcess(process_.get(), win::forcedExit); WaitForSingleObject(process_.get(), INFINITE);
            win::fail("AssignProcessToJobObject", error);
        }
        if (ResumeThread(thread.get()) == DWORD(-1)) {
            const auto error = GetLastError(); TerminateJobObject(job_.get(), win::forcedExit); WaitForSingleObject(process_.get(), INFINITE);
            win::fail("ResumeThread", error);
        }
        stdoutWrite.reset(); stderrWrite.reset();
        try {
            outThread_ = std::thread([this] { read(stdout_.get(), "stdout", stdoutDone_); });
            errThread_ = std::thread([this] { read(stderr_.get(), "stderr", stderrDone_); });
            completionThread_ = std::thread([this] {
                while (!shuttingDown_.load()) {
                    DWORD message = 0; ULONG_PTR key = 0; LPOVERLAPPED processId = nullptr;
                    if (GetQueuedCompletionStatus(completion_.get(), &message, &key, &processId, 100)) {
                        if (message == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) activeZero_.store(true);
                        if ((message == JOB_OBJECT_MSG_EXIT_PROCESS || message == JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS) && reinterpret_cast<ULONG_PTR>(processId) == pid_)
                            rootExited_.store(true);
                    }
                }
            });
        } catch (...) {
            TerminateJobObject(job_.get(), win::forcedExit);
            if (outThread_.joinable()) outThread_.join(); if (errThread_.joinable()) errThread_.join();
            throw;
        }
    }
    ~ManagedProcess() override {
        if (job_) {
            TerminateJobObject(job_.get(), win::forcedExit);
            while (!jobEmpty()) QThread::msleep(10);
        }
        shuttingDown_.store(true);
        if (completionThread_.joinable()) completionThread_.join();
        if (outThread_.joinable()) outThread_.join();
        if (errThread_.joinable()) errThread_.join();
    }
    quint32 rootPid() const override { return pid_; }
    bool rootRunning() const override { return !rootExited_.load() && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT; }
    bool empty() const override { return jobEmpty() && stdoutDone_.load() && stderrDone_.load(); }
    QList<quint32> processIds() const override {
        DWORD capacity = 32;
        for (;;) {
            const auto size = sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + sizeof(ULONG_PTR) * capacity;
            std::vector<BYTE> buffer(size);
            auto *list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST *>(buffer.data());
            if (QueryInformationJobObject(job_.get(), JobObjectBasicProcessIdList, list, static_cast<DWORD>(size), nullptr)) {
                QList<quint32> result;
                for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) result.append(static_cast<quint32>(list->ProcessIdList[i]));
                return result;
            }
            if (GetLastError() != ERROR_MORE_DATA) win::fail("QueryInformationJobObject(ProcessIdList)");
            capacity *= 2;
        }
    }
    std::optional<ProcessResult> result() const override {
        if (rootRunning()) return std::nullopt;
        DWORD code = 0; if (!GetExitCodeProcess(process_.get(), &code)) win::fail("GetExitCodeProcess");
        return ProcessResult{static_cast<qint32>(code), code >= 0xC0000000u};
    }
    void forceStop() override {
        std::lock_guard lock(stopMutex_);
        terminate();
    }
    void stop(int graceMs) override {
        std::lock_guard lock(stopMutex_);
        if (empty()) return;
        if (!helper_.isEmpty() && rootRunning()) {
            WindowsProcessRunner runner({});
            auto helper = runner.start({helper_, {QString::number(pid_)}, QFileInfo(helper_).absolutePath(), environment_, {}, {}, {}, {}}, {});
            const auto deadline = GetTickCount64() + 2000;
            while (!helper->empty() && GetTickCount64() < deadline) QThread::msleep(10);
            if (!helper->empty()) helper->forceStop();
        }
        const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(graceMs);
        while (!empty() && GetTickCount64() < deadline) QThread::msleep(10);
        terminate();
    }
private:
    bool jobEmpty() const {
        if (activeZero_.load()) return true;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(job_.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) return false;
        return accounting.ActiveProcesses == 0;
    }
    void terminate() {
        if (!jobEmpty() && !TerminateJobObject(job_.get(), win::forcedExit)) win::fail("TerminateJobObject");
        while (!empty()) QThread::msleep(10);
    }
    void read(HANDLE pipe, const QString &channel, std::atomic_bool &done) noexcept {
        QByteArray pending; char buffer[8192]; DWORD count = 0;
        const auto deliver = [&](QByteArray line) {
            if (line.endsWith('\r')) line.chop(1);
            QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
            const QString text = decoder(line);
            if (output_) { try { output_({channel, text, decoder.hasError(), QDateTime::currentDateTimeUtc()}); } catch (...) { /* A consumer cannot interrupt pipe draining. */ } }
        };
        while (ReadFile(pipe, buffer, sizeof(buffer), &count, nullptr) && count != 0) {
            pending.append(buffer, static_cast<qsizetype>(count));
            qsizetype newline = 0;
            while ((newline = pending.indexOf('\n')) >= 0) { deliver(pending.left(newline)); pending.remove(0, newline + 1); }
            if (pending.size() > 1024 * 1024) { deliver(pending); pending.clear(); }
        }
        if (!pending.isEmpty()) deliver(pending);
        done.store(true);
    }
    QString helper_;
    Environment environment_;
    std::function<void(ProcessOutput)> output_;
    win::Handle job_, completion_, process_, stdout_, stderr_;
    quint32 pid_ = 0;
    std::thread outThread_, errThread_, completionThread_;
    std::atomic_bool shuttingDown_{false}, activeZero_{false}, rootExited_{false}, stdoutDone_{false}, stderrDone_{false};
    std::mutex stopMutex_;
};
}

WindowsProcessRunner::WindowsProcessRunner(QString helper) : signalHelper_(std::move(helper)) {
    const auto block = GetEnvironmentStringsW(); if (!block) win::fail("GetEnvironmentStrings");
    for (const wchar_t *entry = block; *entry; entry += wcslen(entry) + 1) {
        const auto text = QString::fromWCharArray(entry);
        const auto separator = text.indexOf('=', text.startsWith('=') ? 1 : 0);
        if (separator > 0) inherited_.insert(text.left(separator).toUpper(), text.mid(separator + 1));
    }
    FreeEnvironmentStringsW(block);
}
std::shared_ptr<IManagedProcess> WindowsProcessRunner::start(const ProcessSpec &spec, std::function<void(ProcessOutput)> output) {
    if (!isWindowsAbsolutePath(spec.program) || spec.program.endsWith(".cmd", Qt::CaseInsensitive) || spec.program.endsWith(".bat", Qt::CaseInsensitive))
        throw std::invalid_argument("托管进程必须使用可执行文件的绝对路径");
    return std::make_shared<ManagedProcess>(spec, signalHelper_, std::move(output));
}
Environment WindowsProcessRunner::inheritedEnvironment() const { return inherited_; }
QString WindowsProcessRunner::resolveExecutable(const QString &program, const QStringList &tools) const {
    if (program.endsWith(".cmd", Qt::CaseInsensitive) || program.endsWith(".bat", Qt::CaseInsensitive)) throw std::invalid_argument("批处理必须使用 shell 模式");
    if (isWindowsAbsolutePath(program)) {
        if (QFileInfo(program).isFile()) return QDir::toNativeSeparators(QFileInfo(program).absoluteFilePath());
        throw std::runtime_error(("可执行文件不存在：" + program).toUtf8().constData());
    }
    if (program.contains('/') || program.contains('\\') || program.contains(':')) throw std::invalid_argument("不允许相对可执行文件路径");
    auto directories = tools;
    directories.append(inherited_.value("PATH").split(';', Qt::SkipEmptyParts));
    const auto name = program.contains('.') ? program : program + ".exe";
    for (auto directory : directories) {
        if (directory.startsWith('"') && directory.endsWith('"')) directory = directory.mid(1, directory.size() - 2);
        if (!isWindowsAbsolutePath(directory)) continue;
        const QFileInfo file(QDir(directory).filePath(name));
        if (file.isFile()) return QDir::toNativeSeparators(file.absoluteFilePath());
    }
    throw std::runtime_error(("在 toolDirectories 和启动 PATH 中找不到程序：" + program).toUtf8().constData());
}
}

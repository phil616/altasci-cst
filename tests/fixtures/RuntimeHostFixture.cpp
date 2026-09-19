#include "application/RuntimeClient.h"
#include "infrastructure/common/SystemClock.h"
#include <QCoreApplication>
#include <QDir>
#include <atomic>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <signal.h>
#endif
using namespace cst;
class Process final : public IManagedProcess {
public:
    std::atomic_bool alive{true};
    quint32 rootPid() const override { return 42; }
    bool rootRunning() const override { return alive; }
    bool empty() const override { return !alive; }
    QList<quint32> processIds() const override { return alive ? QList<quint32>{42} : QList<quint32>{}; }
    std::optional<ProcessResult> result() const override { if (alive) return {}; return ProcessResult{0, false}; }
    void stop(int) override { alive = false; }
    void forceStop() override { alive = false; }
    void writeInput(const QByteArray &bytes) override { if (bytes == "exit") alive = false; }
};
class Runner final : public IProcessRunner {
public:
    std::shared_ptr<IManagedProcess> start(const ProcessSpec &spec, std::function<void(ProcessOutput)> output) override {
        auto process = std::make_shared<Process>();
        if (spec.program == "finish") process->alive = false;
        if (output) output({"stdout", "host output", false, QDateTime::currentDateTimeUtc()});
        return process;
    }
    QString resolveExecutable(const QString &program, const QStringList &) const override { return program; }
    Environment inheritedEnvironment() const override { return {}; }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); if (argc != 3) return 2;
    Runner runner; SystemClock clock; const auto args = app.arguments();
    ProjectPaths paths{QDir::tempPath(), qEnvironmentVariable("CST_FIXTURE_STORAGE"), QDir::tempPath()};
    TaskSupervisor supervisor(runner, clock, paths); const auto token = qEnvironmentVariable("CST_HOST_TOKEN");
#ifdef Q_OS_WIN
    const auto owner = OpenProcess(SYNCHRONIZE, FALSE, args[2].toUInt());
    const auto code = serveRuntime(supervisor, paths, args[1], token, [owner] { return owner && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT; });
    if (owner) CloseHandle(owner); return code;
#else
    const auto pid = args[2].toInt();
    return serveRuntime(supervisor, paths, args[1], token, [pid] { return kill(pid, 0) == 0; });
#endif
}

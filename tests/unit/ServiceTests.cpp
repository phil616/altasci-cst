#include "application/OperationQueue.h"
#include "application/PortReclaimService.h"
#include "application/TaskSupervisor.h"
#include "application/SourceSyncService.h"
#include "application/ProjectConfigService.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include "../TestCompatibility.h"
#include <QUuid>

using namespace cst;
namespace {
class TestClock final : public IClock {
public:
    mutable qint64 now = 0;
    qint64 monotonicMs() const override { return now; }
    void sleep(int ms, const Cancellation &cancel) const override { cancel.check(); now += ms; }
};
class TestProcess final : public IManagedProcess {
public:
    bool alive = true;
    qint64 exit = 0;
    quint32 pid = 42;
    std::function<void()> stopped;
    quint32 rootPid() const override { return pid; }
    bool rootRunning() const override { return alive; }
    bool empty() const override { return !alive; }
    QList<quint32> processIds() const override { return alive ? QList<quint32>{pid} : QList<quint32>{}; }
    std::optional<ProcessResult> result() const override { return alive ? std::nullopt : std::optional<ProcessResult>({exit, false}); }
    void stop(int) override { if (alive && stopped) stopped(); alive = false; }
    void forceStop() override { stop(0); }
};
class TestPorts final : public IPortManager {
public:
    mutable QList<PortOwner> occupied;
    QList<quint32> killed;
    QList<PortOwner> owners(const PortRequirement &) const override { return occupied; }
    void terminateTree(quint32 pid, const PortRequirement &, const Cancellation &cancel) override {
        cancel.check(); killed.append(pid); occupied.clear();
    }
    bool stopService(quint32, const Cancellation &) override { return false; }
    quint32 parentPid(quint32) const override { return 0; }
};
class TestFiles final : public IFileTransaction {
public:
    int renames = 0;
    int failRename = 0;
    bool failDelete = false;
    void renameDirectory(const QString &from, const QString &to) override {
        if (++renames == failRename || !QDir().rename(from, to)) throw std::runtime_error("Injected rename failure");
    }
    bool removeDirectory(const QString &path) override { return !failDelete && QDir(path).removeRecursively(); }
};
void writeMarker(const QString &directory, const QString &name) {
    if (!QDir().mkpath(directory)) throw std::runtime_error("mkdir failed");
    QFile file(directory + '/' + name);
    if (!file.open(QIODevice::WriteOnly) || file.write(name.toUtf8()) < 0) throw std::runtime_error("write failed");
}
class TestRunner final : public IProcessRunner {
public:
    QStringList events;
    QStringList workingDirectories;
    QStringList paths;
    QStringList shellCommandLines;
    QList<std::shared_ptr<TestProcess>> services;
    QString failCommand;
    QString failToStart;
    QString processOutput;
    QString gitVersion = "git version 2.55.0.windows.1";
    QString head = QString(40, 'a');
    bool gitMode = false;
    std::shared_ptr<IManagedProcess> start(const ProcessSpec &spec, std::function<void(ProcessOutput)> output) override {
        if (spec.program == failToStart && !failToStart.isEmpty()) throw std::runtime_error("CreateProcess (Win32 267): invalid directory");
        auto process = std::make_shared<TestProcess>();
        if (gitMode) {
            process->alive = false;
            if (spec.arguments[0] == "--version") output({"stdout", gitVersion, false, {}});
            if (spec.arguments[0] == "clone") writeMarker(spec.arguments.last(), "remote");
            if (spec.arguments[0] == "rev-parse") output({"stdout", head, false, {}});
            if (spec.arguments[0] == failCommand) process->exit = 1;
            events.append(spec.arguments.join(' '));
            return process;
        }
        events.append("start:" + spec.program);
        workingDirectories.append(spec.workingDirectory);
        paths.append(spec.environment.value("PATH"));
        shellCommandLines.append(spec.shellCommandLine);
        process->stopped = [this, name = spec.program] { events.append("stop:" + name); };
        if (output && !processOutput.isEmpty()) output({"stderr", processOutput, false, {}});
        if (spec.program.contains("prepare")) { process->alive = false; if (spec.program == failCommand) process->exit = 1; }
        else services.append(process);
        return process;
    }
    QString resolveExecutable(const QString &program, const QStringList &) const override { return program; }
    Environment inheritedEnvironment() const override { return {}; }
};
QJsonObject task(const QString &id, int order, quint16) {
    return {{"id", id}, {"name", id}, {"order", order},
        {"environment", QJsonObject{{"inheritSystem", false}, {"envFiles", QJsonArray{}}, {"variables", QJsonObject{}}}},
        {"prepareCommands", QJsonArray{QJsonObject{{"id", id + "-prepare"}, {"name", id + "-prepare"}, {"mode", "exec"}, {"program", id + "-prepare"}, {"arguments", QJsonArray{}}, {"workingDirectory", "C:/project/prepare"}, {"timeoutMs", 1000}, {"successExitCodes", QJsonArray{0}}}}},
        {"serviceCommand", QJsonObject{{"id", id + "-serve"}, {"mode", "exec"}, {"program", id + "-serve"}, {"arguments", QJsonArray{}}, {"workingDirectory", "C:/project/service"}, {"timeoutMs", 0}, {"successExitCodes", QJsonArray{0}}}},
        {"restartPolicy", QJsonObject{{"maxRestarts", 5}, {"windowSeconds", 600}, {"backoffSeconds", 1}, {"maxBackoffSeconds", 30}}}, {"shutdownGraceMs", 0}};
}
QJsonObject project(quint16 port) {
    return {{"id", "test"}, {"source", QJsonObject{{"workingDirectory", "C:/project"}}},
        {"tasks", QJsonArray{task("second", 20, port), task("first", 10, port)}}, {"requiredPorts", QJsonArray{}},
        {"toolDirectories", QJsonArray{}}, {"settings", QJsonObject{{"portReclaimTimeoutMs", 1000}, {"maxAncestorEscalation", 8}}}};
}
}
class ServiceTests final : public QObject {
    Q_OBJECT
private slots:
    void failedPreparationIncludesExitAndOutput() {
        TestRunner runner; runner.failCommand = "first-prepare";
        runner.processOutput = QString(20000, 'x') + "\x1b[31mModuleNotFoundError: missing_dependency\x1b[0m";
        TestClock clock; Cancellation cancel; TaskSupervisor supervisor(runner, clock, {});
        try { supervisor.start(project(0), "run", cancel); QFAIL("Expected failure"); }
        catch (const std::runtime_error &e) {
            const auto message = QString::fromUtf8(e.what());
            QVERIFY(message.contains("first-prepare")); QVERIFY(message.contains("C:/project/prepare"));
            QVERIFY(message.contains("0x00000001")); QVERIFY(message.contains("ModuleNotFoundError: missing_dependency"));
            QVERIFY(!message.contains(QChar(0x1b))); QVERIFY(message.size() < 9000);
        }
        supervisor.stop();
    }
    void spawnFailureIncludesCommandContext() {
        TestRunner runner; runner.failToStart = "first-serve";
        TestClock clock; Cancellation cancel; TaskSupervisor supervisor(runner, clock, {});
        try { supervisor.start(project(0), "run", cancel); QFAIL("Expected failure"); }
        catch (const std::runtime_error &e) {
            const auto message = QString::fromUtf8(e.what());
            QVERIFY(message.contains("first-serve")); QVERIFY(message.contains("C:/project/service"));
            QVERIFY(message.contains("Win32 267")); QVERIFY(message.contains("pipes"));
        }
        supervisor.stop();
    }
    void restartLogsFailureBeforeNextAttempt() {
        TestRunner runner; runner.processOutput = "The system cannot find the path specified.";
        TestClock clock; Cancellation cancel; TaskSupervisor supervisor(runner, clock, {});
        QStringList logs;
        supervisor.output = [&](const QString &, ProcessOutput out) { if (out.channel == "cst") logs.append(out.text); };
        supervisor.start(project(0), "run", cancel);
        runner.services[0]->alive = false; runner.services[0]->exit = 1;
        supervisor.tick(cancel);
        QCOMPARE(supervisor.statuses().first().state, "Restarting");
        QVERIFY(logs.join('\n').contains("The system cannot find the path specified."));
        QVERIFY(logs.join('\n').contains("0x00000001"));
        supervisor.stop();
    }
    void executionOrderAndRestart() {
        QTcpServer endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        TestRunner runner; TestClock clock; Cancellation cancel;
        TaskSupervisor supervisor(runner, clock, {"C:/app", "C:/data", "C:/Windows"});
        supervisor.start(project(endpoint.serverPort()), "operation", cancel);
        QCOMPARE(runner.events, QStringList({"start:first-prepare", "start:first-serve", "start:second-prepare", "start:second-serve"}));
        QCOMPARE(runner.workingDirectories, QStringList({"C:/project/prepare", "C:/project/service", "C:/project/prepare", "C:/project/service"}));
        clock.now += 3000;
        runner.services[0]->alive = false;
        supervisor.tick(cancel); clock.now += 1000; supervisor.tick(cancel);
        QCOMPARE(runner.events.last(), "start:first-serve");
        QCOMPARE(runner.events.count("start:first-prepare"), 1);
        QCOMPARE(supervisor.statuses()[0].restartCount, 1);
        supervisor.stop();
        QCOMPARE(runner.events.mid(runner.events.size() - 2), QStringList({"stop:second-serve", "stop:first-serve"}));
        QVERIFY(supervisor.empty());
    }
    void rapidExitUsesConfiguredRestartBudget() {
        TestRunner runner; TestClock clock; Cancellation cancel;
        TaskSupervisor supervisor(runner, clock, {});
        supervisor.start(project(0), "operation", cancel);
        QCOMPARE(runner.events.size(), 4);
        runner.services[0]->alive = false;
        supervisor.tick(cancel);
        QCOMPARE(supervisor.statuses()[0].state, "Restarting");
        QCOMPARE(runner.events.size(), 4);
        QCOMPARE(runner.events.count("start:first-serve"), 1);
        supervisor.stop();
    }
    void oneShotCompletesAndTreeLifetimeKeepsDescendants() {
        TestRunner runner; TestClock clock; Cancellation cancel; TaskSupervisor supervisor(runner, clock, {});
        auto once = task("once", 0, 0); once["kind"] = "task"; once["prepareCommands"] = QJsonArray{};
        auto cmd = once["serviceCommand"].toObject(); cmd["program"] = "once-prepare"; cmd["timeoutMs"] = 1000; once["serviceCommand"] = cmd;
        supervisor.start({{"tasks", QJsonArray{once}}}, "run", cancel);
        QCOMPARE(supervisor.statuses().first().state, "Completed"); QVERIFY(supervisor.empty()); supervisor.stop();
    }
    void neverRestartRejectsUnexpectedServiceExit() {
        TestRunner runner; TestClock clock; Cancellation cancel; TaskSupervisor supervisor(runner, clock, {});
        auto config = task("service", 0, 0); config["restartPolicy"] = QJsonObject{{"mode", "never"}};
        supervisor.start({{"tasks", QJsonArray{config}}}, "run", cancel); runner.services.first()->alive = false;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, supervisor.tick(cancel)); supervisor.stop();
    }
    void failureShortCircuitAndCleanup() {
        QTcpServer endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        TestRunner runner; runner.failCommand = "second-prepare";
        TestClock clock; Cancellation cancel;
        TaskSupervisor supervisor(runner, clock, {});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, supervisor.start(project(endpoint.serverPort()), "operation", cancel));
        QVERIFY(!runner.events.contains("start:second-serve"));
        supervisor.stop(); QVERIFY(supervisor.empty()); QCOMPARE(runner.events.last(), "stop:first-serve");
    }
    void activationScriptWrapsExecAndShellCommands() {
        QTemporaryDir directory; QFile activation(directory.filePath("activate.bat"));
        QVERIFY(activation.open(QIODevice::WriteOnly)); activation.close();
        TestRunner runner; TestClock clock; Cancellation cancel;
        TaskSupervisor supervisor(runner, clock, {});
        auto taskObject = task("venv", 10, 0);
        auto service = taskObject["serviceCommand"].toObject();
        service["mode"] = "exec";
        service["program"] = "python.exe";
        service["arguments"] = QJsonArray{"-m", "app"};
        service["workingDirectory"] = "C:/project/service";
        service["activationScript"] = activation.fileName();
        service["timeoutMs"] = 0;
        service["successExitCodes"] = QJsonArray{0};
        taskObject["serviceCommand"] = service;
        taskObject["prepareCommands"] = QJsonArray{};
        const auto document = QJsonObject{{"schemaVersion", 1}, {"project", QJsonObject{
            {"id", "test"}, {"source", QJsonObject{{"workingDirectory", "C:/project"}}},
            {"toolDirectories", QJsonArray{}}, {"requiredPorts", QJsonArray{}},
            {"tasks", QJsonArray{taskObject}},
            {"settings", QJsonObject{{"portReclaimTimeoutMs", 1000}, {"maxAncestorEscalation", 8}}}
        }}};
        supervisor.start(document["project"].toObject(), "operation", cancel);
        QVERIFY(!runner.shellCommandLines.isEmpty());
        const auto line = runner.shellCommandLines.last();
        QVERIFY(line.contains("call \"" + normalizeWindowsPathInput(activation.fileName()) + "\""));
        QVERIFY(line.contains("\"python.exe\""));
        QVERIFY(line.contains("\"-m\"") && line.contains("\"app\""));
        supervisor.stop();
    }
    void shellCommandsReceiveToolDirectoriesOnPath() {
        TestRunner runner; TestClock clock; Cancellation cancel;
        TaskSupervisor supervisor(runner, clock, {});
        auto taskObject = task("shell-only", 10, 0);
        auto service = taskObject["serviceCommand"].toObject();
        service["mode"] = "shell";
        service.remove("program"); service.remove("arguments");
        service["script"] = "pnpm install";
        service["timeoutMs"] = 0;
        service["successExitCodes"] = QJsonArray{0};
        taskObject["serviceCommand"] = service;
        taskObject["prepareCommands"] = QJsonArray{};
        const auto document = QJsonObject{{"schemaVersion", 1}, {"project", QJsonObject{
            {"id", "test"}, {"source", QJsonObject{{"workingDirectory", "C:/project"}}},
            {"toolDirectories", QJsonArray{"C:/Tools"}}, {"requiredPorts", QJsonArray{}},
            {"tasks", QJsonArray{taskObject}},
            {"settings", QJsonObject{{"portReclaimTimeoutMs", 1000}, {"maxAncestorEscalation", 8}}}
        }}};
        supervisor.start(document["project"].toObject(), "operation", cancel);
        QVERIFY(!runner.paths.isEmpty());
        for (const auto &path : runner.paths) QVERIFY(path.startsWith("C:\\Tools"));
        supervisor.stop();
    }
    void portFreeStability() {
        TestPorts ports; TestClock clock; Cancellation cancel; PortReclaimService reclaim(ports, clock);
        ports.occupied.append({"tcp", "127.0.0.1", 8000, 42, 9, "owner"});
        reclaim.reclaim({{"tcp", "127.0.0.1", 8000, "task"}}, 2000, 8, {}, cancel);
        QCOMPARE(ports.killed, QList<quint32>{42}); QVERIFY(clock.now >= 750);
    }
    void operationQueueSerializes() {
        OperationQueue queue;
        QStringList events; bool finished = false;
        queue.enqueue([&] { events.append("begin1"); }, [&] { QThread::msleep(10); }, [&](std::exception_ptr error) { QVERIFY(!error); events.append("end1"); });
        queue.enqueue([&] { events.append("begin2"); }, [] { throw std::runtime_error("failure"); }, [&](std::exception_ptr error) { QVERIFY(error); events.append("end2"); finished = true; });
        QCOMPARE(events, QStringList{"begin1"});
        QTRY_VERIFY(finished);
        QCOMPARE(events, QStringList({"begin1", "end1", "begin2", "end2"}));
    }
    void syncTransaction_data() { QTest::addColumn<int>("failure"); QTest::newRow("success") << 0; QTest::newRow("first-rename") << 1; QTest::newRow("second-rename") << 2; }
    void syncTransaction() {
        QFETCH(int, failure);
        QTemporaryDir directory; TestRunner runner; runner.gitMode = true; TestFiles files; files.failRename = failure;
        TestClock clock; Cancellation cancel; QMutex mutex;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces); const auto target = directory.filePath("target");
        writeMarker(target, "local");
        SourceSyncService sync(runner, files, clock, directory.filePath("storage"), "askpass", mutex);
        const SyncRequest request{id, "https://example.test/repo", "main", target, "git", "credential", "operation"};
        if (failure) { QVERIFY_THROWS_EXCEPTION(std::runtime_error, sync.synchronize(request, cancel, [] { return true; })); QVERIFY(QFile::exists(target + "/local")); }
        else { QCOMPARE(sync.synchronize(request, cancel, [] { return true; }), runner.head); QVERIFY(QFile::exists(target + "/remote")); QVERIFY(!QFile::exists(target + "/local")); }
        QVERIFY(!QFile::exists(directory.filePath("storage/state/" + id + "/sync-journal.json")));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, sync.synchronize(request, cancel, [] { return false; }));
    }
    void journalRecovery_data() {
        QTest::addColumn<int>("stage");
        QTest::newRow("validated") << 0; QTest::newRow("old-moved") << 1; QTest::newRow("installed") << 2;
        QTest::newRow("first-install") << 3; QTest::newRow("missing-all") << 4;
    }
    void journalRecovery() {
        QFETCH(int, stage);
        QTemporaryDir directory; TestRunner runner; runner.gitMode = true; TestFiles files; TestClock clock; Cancellation cancel; QMutex mutex;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto target = directory.filePath("target"); const auto suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto staging = directory.filePath(".target.cst-staging-" + suffix); const auto backup = directory.filePath(".target.cst-backup-" + suffix);
        if (stage == 0 || stage == 2) writeMarker(target, "target");
        if (stage == 1 || stage == 2) writeMarker(backup, "backup");
        if (stage != 4) writeMarker(staging, "staging");
        const auto journal = directory.filePath("storage/state/" + id + "/sync-journal.json");
        saveJson(journal, {{"target", target}, {"staging", staging}, {"backup", backup}, {"phase", "crash"}});
        SourceSyncService sync(runner, files, clock, directory.filePath("storage"), "askpass", mutex);
        const SyncRequest request{id, "https://example.test/repo", "main", target, "git", "credential", "operation"};
        if (stage == 4) { QVERIFY_THROWS_EXCEPTION(std::runtime_error, sync.recover(request, cancel)); QVERIFY(QFile::exists(journal)); }
        else {
            sync.recover(request, cancel);
            QVERIFY(QFile::exists(target + (stage == 1 ? "/backup" : stage == 3 ? "/staging" : "/target")));
            QVERIFY(!QFile::exists(journal)); QVERIFY(!QFileInfo::exists(backup)); QVERIFY(!QFileInfo::exists(staging));
        }
    }
};
QTEST_GUILESS_MAIN(ServiceTests)
#include "ServiceTests.moc"

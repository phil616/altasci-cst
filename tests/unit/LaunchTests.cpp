#include "application/LaunchPlanner.h"
#include "application/ProjectConfigService.h"
#include "../TestCompatibility.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>

using namespace cst;
class RecordingRunner final : public IProcessRunner {
public:
    mutable Environment selected;
    mutable QString cwd;
    std::shared_ptr<IManagedProcess> start(const ProcessSpec &, std::function<void(ProcessOutput)>) override { throw std::runtime_error("not used"); }
    QString resolveExecutable(const QString &program, const QStringList &) const override { return program; }
    QString resolveInEnvironment(const QString &program, const Environment &environment, const QString &directory) const override { selected = environment; cwd = directory; return program; }
    Environment inheritedEnvironment() const override { return {{"PATH", "C:/global"}, {"SYSTEMROOT", "C:/Windows"}, {"COMSPEC", "C:/Windows/System32/cmd.exe"}, {"SECRET_AMBIENT", "not inherited"}}; }
};
class LaunchTests final : public QObject {
    Q_OBJECT
private slots:
    void finalEnvironmentDrivesLookup() {
        RecordingRunner runner; LaunchPlanner planner(runner, {});
        QJsonObject task{{"environment", QJsonObject{{"inheritSystem", false}, {"variables", QJsonObject{{"Path", "C:/chosen"}}}}}};
        const auto plan = planner.resolve({}, task, {{"mode", "exec"}, {"program", "python"}, {"workingDirectory", "C:/project"}}, "run");
        QCOMPARE(runner.selected.value("PATH"), "C:/chosen"); QVERIFY(!runner.selected.contains("SECRET_AMBIENT"));
        QCOMPARE(plan.environment.value("PATH"), "C:/chosen"); QCOMPARE(runner.cwd, "C:/project"); QVERIFY(!plan.attemptId.isEmpty());
    }
    void uvDoesNotResolveTarget() {
        RecordingRunner runner; LaunchPlanner planner(runner, {});
        const auto plan = planner.resolve({}, {}, {{"mode", "uv"}, {"toolArguments", QJsonArray{"--locked"}}, {"arguments", QJsonArray{"python", "-m", "app", "a&b", ""}}}, "run");
        QCOMPARE(plan.program, "uv.exe"); QCOMPARE(plan.arguments, QStringList({"run", "--locked", "--", "python", "-m", "app", "a&b", ""}));
        QVERIFY(plan.shellCommandLine.isEmpty());
    }
    void venvSelectsInterpreterWithoutShell() {
        RecordingRunner runner; LaunchPlanner planner(runner, {});
        const auto plan = planner.resolve({}, {}, {{"mode", "python-venv"}, {"workingDirectory", "C:/project"}, {"venv", ".venv"}, {"arguments", QJsonArray{"-m", "app"}}}, "run");
        QCOMPARE(plan.program, "C:/project/.venv/Scripts/python.exe"); QVERIFY(plan.shellCommandLine.isEmpty());
        QCOMPARE(plan.environment.value("VIRTUAL_ENV"), "C:/project/.venv"); QVERIFY(runner.selected.value("PATH").startsWith("C:/project/.venv/Scripts;"));
    }
    void npmKeepsScriptAndArgumentBoundaries() {
        QTemporaryDir dir; QFile cli(dir.filePath("npm-cli.js")); QVERIFY(cli.open(QIODevice::WriteOnly)); cli.close();
        RecordingRunner runner; LaunchPlanner planner(runner, {});
        const auto plan = planner.resolve({}, {}, {{"mode", "npm"}, {"npmCli", cli.fileName()}, {"script", "dev"}, {"toolArguments", QJsonArray{"--workspace=web"}}, {"arguments", QJsonArray{"--host", "a b&c"}}}, "run");
        QCOMPARE(plan.program, "node.exe"); QCOMPARE(plan.arguments, QStringList({cli.fileName(), "run", "dev", "--workspace=web", "--", "--host", "a b&c"}));
    }
    void environmentFilesResolveAfterPreparation() {
        QTemporaryDir dir; RecordingRunner runner; LaunchPlanner planner(runner, {});
        const auto task = QJsonObject{{"environment", QJsonObject{{"envFiles", QJsonArray{dir.filePath("generated.env")}}}}};
        const auto command = QJsonObject{{"program", "tool.exe"}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, planner.resolve({}, task, command, "run"));
        QFile file(dir.filePath("generated.env")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("PATH=C:/generated\n"); file.close();
        const auto plan = planner.resolve({}, task, command, "run"); QCOMPARE(plan.environment.value("PATH"), "C:/generated");
    }
    void profileCycleAndLegacyMetacharactersRejected() {
        RecordingRunner runner; LaunchPlanner planner(runner, {});
        QJsonObject project{{"environments", QJsonObject{{"a", QJsonObject{{"extends", "a"}}}}}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, planner.resolve(project, {{"environmentRef", "a"}}, {{"program", "x"}}, "run"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, planner.resolve({}, {}, {{"mode", "exec"}, {"program", "python"}, {"activationScript", "C:/activate.bat"}, {"arguments", QJsonArray{"%PATH%"}}}, "run"));
    }
};
QTEST_GUILESS_MAIN(LaunchTests)
#include "LaunchTests.moc"

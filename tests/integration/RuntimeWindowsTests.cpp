#include "application/LaunchPlanner.h"
#include "infrastructure/windows/WindowsPlatform.h"
#include "infrastructure/common/SystemClock.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <windows.h>
using namespace cst;
class RuntimeWindowsTests final : public QObject {
    Q_OBJECT
    QString python_, uv_, node_, helper_;
    void write(const QString &path, const QByteArray &bytes) {
        QFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) throw std::runtime_error("fixture write failed");
    }
    QString execute(WindowsProcessRunner &runner, ProcessSpec plan) {
        QMutex mutex; QString output;
        auto process = runner.start(plan, [&](ProcessOutput item) { QMutexLocker lock(&mutex); output += item.text; });
        QElapsedTimer time; time.start();
        while (!process->treeEmpty() && time.elapsed() < 90000) QTest::qWait(10);
        const bool finished = process->treeEmpty(); process->forceStop();
        if (!finished || !process->result() || process->result()->exitCode != 0) throw std::runtime_error(("fixture command failed: " + output).toUtf8().constData());
        return output;
    }
private slots:
    void initTestCase() {
        python_ = qEnvironmentVariable("CST_TEST_PYTHON"); if (python_.isEmpty()) python_ = QStandardPaths::findExecutable("python.exe");
        uv_ = qEnvironmentVariable("CST_TEST_UV"); if (uv_.isEmpty()) uv_ = QStandardPaths::findExecutable("uv.exe");
        node_ = QStandardPaths::findExecutable("node.exe"); helper_ = QCoreApplication::applicationDirPath() + "/cst-signal-helper.exe";
        QVERIFY2(QFile::exists(python_), "Python required for runtime integration tests");
        QVERIFY2(QFile::exists(uv_), "uv required for runtime integration tests");
        QVERIFY2(QFile::exists(node_), "Node/npm required for runtime integration tests");
    }
    void venvPreparationAndFinalEnvironment() {
        QTemporaryDir dir(QDir::tempPath() + "/CST 中文 runtime XXXXXX"); QVERIFY(dir.isValid());
        WindowsProcessRunner runner(helper_); LaunchPlanner planner(runner, {});
        execute(runner, planner.resolve({}, {}, {{"mode", "exec"}, {"program", python_}, {"workingDirectory", dir.path()}, {"arguments", QJsonArray{"-m", "venv", ".venv", "--without-pip"}}}, "setup"));
        auto plan = planner.resolve({}, {}, {{"mode", "python-venv"}, {"workingDirectory", dir.path()}, {"venv", ".venv"}, {"arguments", QJsonArray{"-c", "import sys,json;print(json.dumps({'exe':sys.executable,'prefix':sys.prefix,'arg':sys.argv[1:]}))", "", "a b", "a&b", "%PATH%", "x!y", "tail\\", "a\"b"}}}, "venv");
        const auto result = QJsonDocument::fromJson(execute(runner, plan).trimmed().toUtf8()).object();
        QVERIFY(result.value("exe").toString().contains(".venv")); QCOMPARE(result.value("arg").toArray(), QJsonArray({"", "a b", "a&b", "%PATH%", "x!y", "tail\\", "a\"b"}));
        Environment env = runner.inheritedEnvironment(); env["PATH"] = dir.path() + "/.venv/Scripts";
        QVERIFY(runner.resolveInEnvironment("python", env, dir.path()).contains(".venv"));
    }
    void uvProjectAndNpmLifecycle_data() {
        QTest::addColumn<bool>("terminal");
        QTest::newRow("pipes") << false;
        QTest::newRow("terminal") << true;
    }
    void uvProjectAndNpmLifecycle() {
        QFETCH(bool, terminal);
        QTemporaryDir dir; WindowsProcessRunner runner(helper_); LaunchPlanner planner(runner, {});
        write(dir.filePath("pyproject.toml"), "[project]\nname='cst-fixture'\nversion='0.0.0'\nrequires-python='>=3.12'\n");
        auto plan = planner.resolve({}, {}, {{"mode", "uv"}, {"program", uv_}, {"workingDirectory", dir.path()}, {"toolArguments", QJsonArray{"--python", python_}}, {"arguments", QJsonArray{"python", "-c", "import sys;print('UV_PREFIX='+sys.prefix)"}}}, "uv");
        plan.terminal = terminal; plan.inputEnabled = terminal;
        const auto output = execute(runner, plan); QVERIFY(output.contains("UV_PREFIX=")); QVERIFY(output.contains(".venv"));
        write(dir.filePath("package.json"), "{\"name\":\"cst-fixture\",\"version\":\"0.0.0\",\"scripts\":{\"precheck\":\"node -e \\\"console.log('PRE')\\\"\",\"check\":\"node check.cjs\",\"postcheck\":\"node -e \\\"console.log('POST')\\\"\"}}");
        write(dir.filePath("check.cjs"), "console.log('NPM_ARG='+JSON.stringify(process.argv.slice(2)));console.log('BIN='+process.env.PATH.includes('node_modules'));\n");
        plan = planner.resolve({}, {}, {{"mode", "npm"}, {"program", node_}, {"workingDirectory", dir.path()}, {"script", "check"}, {"arguments", QJsonArray{"hello world"}},
            {"environment", QJsonObject{{"inheritSystem", false}, {"variables", QJsonObject{{"PATH", qEnvironmentVariable("SystemRoot") + "/System32"}}}}}}, "npm");
        plan.terminal = terminal; plan.inputEnabled = terminal;
        const auto npm = execute(runner, plan); QVERIFY(npm.contains("PRE")); QVERIFY(npm.contains("POST")); QVERIFY(npm.contains("hello world")); QVERIFY(npm.contains("BIN=true"));
    }
    void shellActivationUsesProjectDirectory_data() {
        QTest::addColumn<bool>("terminal");
        QTest::newRow("pipes") << false;
        QTest::newRow("terminal") << true;
    }
    void shellActivationUsesProjectDirectory() {
        QFETCH(bool, terminal);
        QTemporaryDir dir(QDir::tempPath() + "/CST 中文 venv XXXXXX"); QVERIFY(dir.isValid());
        WindowsProcessRunner runner(helper_); LaunchPlanner planner(runner, {});
        execute(runner, planner.resolve({}, {}, {{"mode", "exec"}, {"program", python_}, {"workingDirectory", dir.path()},
            {"arguments", QJsonArray{"-m", "venv", ".venv", "--without-pip"}}}, "setup"));
        write(dir.filePath("check.py"), "import os,sys\nassert sys.prefix != sys.base_prefix\nassert os.path.exists('check.py')\nprint('VENV_OK', flush=True)\n");
        const QJsonObject project{{"source", QJsonObject{{"workingDirectory", dir.path()}}}};
        auto plan = planner.resolve(project, {}, {{"mode", "shell"}, {"activationScript", ".venv/Scripts/activate.bat"},
            {"script", "python check.py"}}, "shell");
        plan.terminal = terminal; plan.inputEnabled = terminal;
        QVERIFY(execute(runner, plan).contains("VENV_OK"));
        plan = planner.resolve(project, {}, {{"mode", "python-venv"}, {"venv", ".venv"}, {"arguments", QJsonArray{"check.py"}}}, "venv");
        plan.terminal = terminal; plan.inputEnabled = terminal;
        QVERIFY(execute(runner, plan).contains("VENV_OK"));
    }
    void conptyInputResizeAndCleanup() {
        QTemporaryDir dir; WindowsProcessRunner runner(helper_); LaunchPlanner planner(runner, {}); QMutex mutex; QByteArray output;
        auto plan = planner.resolve({}, {}, {{"mode", "exec"}, {"program", python_}, {"workingDirectory", dir.path()}, {"arguments", QJsonArray{"-u", "-c", "import sys;print('TTY='+str(sys.stdin.isatty()),flush=True);print('PROMPT>',end='',flush=True);s=input();print('ECHO='+s,flush=True)"}}, {"io", QJsonObject{{"mode", "terminal"}}}}, "terminal");
        auto process = runner.start(plan, [&](ProcessOutput item) { QMutexLocker lock(&mutex); output += item.bytes; });
        const auto contains = [&](const QByteArray &text) { QMutexLocker lock(&mutex); return output.contains(text); };
        QTRY_VERIFY_WITH_TIMEOUT(contains("PROMPT>"), 10000); QVERIFY(contains("TTY=True"));
        process->resizeTerminal(120, 40); process->writeInput("hello\r");
        QTRY_VERIFY_WITH_TIMEOUT(contains("ECHO=hello"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(process->treeEmpty(), 10000); process->forceStop(); QVERIFY(process->empty());
    }
    void rootExitPreservesOwnedDescendantUntilStopped() {
        QTemporaryDir dir; WindowsProcessRunner runner(helper_); LaunchPlanner planner(runner, {});
        auto plan = planner.resolve({}, {}, {{"mode", "exec"}, {"program", python_}, {"workingDirectory", dir.path()}, {"arguments", QJsonArray{"-c", "import subprocess,sys;subprocess.Popen([sys.executable,'-c','import time;time.sleep(120)'])"}}}, "tree");
        auto process = runner.start(plan, {}); QTRY_VERIFY_WITH_TIMEOUT(!process->rootRunning(), 10000);
        QVERIFY(!process->treeEmpty()); const auto pids = process->processIds(); QVERIFY(!pids.isEmpty());
        process->stop(100); QVERIFY(process->treeEmpty());
        for (const auto pid : pids) { HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid); if (h) { QCOMPARE(WaitForSingleObject(h, 0), DWORD(WAIT_OBJECT_0)); CloseHandle(h); } }
    }
};
QTEST_GUILESS_MAIN(RuntimeWindowsTests)
#include "RuntimeWindowsTests.moc"

#include "application/RuntimeClient.h"
#include "../TestCompatibility.h"
#include <QTest>
#include <QTemporaryDir>
#include <QLockFile>

using namespace cst;
class RuntimeTests final : public QObject {
    Q_OBJECT
private slots:
    void independentHostStartOutputInputStopAndLock() {
        try {
        QTemporaryDir directory; qputenv("CST_FIXTURE_STORAGE", directory.path().toUtf8());
        QString executable = QCoreApplication::applicationDirPath() + "/cst-test-runtime-host";
#ifdef Q_OS_WIN
        executable += ".exe";
#endif
        RuntimeClient client(executable); Cancellation cancel;
        QJsonObject project{{"id", "fixture"}, {"tasks", QJsonArray{QJsonObject{{"id", "server"}, {"name", "fixture"}, {"serviceCommand", QJsonObject{{"mode", "exec"}, {"program", "serve"}, {"successExitCodes", QJsonArray{0}}}}}}}};
        QList<ProcessOutput> output;
        client.output = [&](const QString &, ProcessOutput item) { output.append(std::move(item)); };
        client.preflight(project, cancel); QLockFile locked(directory.filePath("locks/fixture.lock")); QVERIFY(!locked.tryLock(0));
        client.start(project, "run-id", cancel); QVERIFY(!client.empty()); QCOMPARE(client.statuses().first().state, "Running");
        QVERIFY(!output.isEmpty()); QCOMPARE(output.first().runId, "run-id"); bool hasAttempt = false; for (const auto &item : output) if (item.channel == "stdout" && !item.attemptId.isEmpty()) hasAttempt = true; QVERIFY(hasAttempt);
        client.resizeTerminal("server", 90, 30); client.writeInput("server", "ignored"); QVERIFY(!client.empty());
        client.stop(); QVERIFY(client.empty()); QVERIFY(locked.tryLock(0)); locked.unlock();
        client.stop(); QVERIFY(client.empty());
        } catch (const std::exception &error) { QFAIL(error.what()); }
    }
    void cancellingPreparationCleansUp() {
        try {
        QTemporaryDir directory; qputenv("CST_FIXTURE_STORAGE", directory.path().toUtf8());
        QString executable = QCoreApplication::applicationDirPath() + "/cst-test-runtime-host";
#ifdef Q_OS_WIN
        executable += ".exe";
#endif
        RuntimeClient client(executable); Cancellation cancel;
        QJsonObject command{{"mode", "exec"}, {"program", "serve"}, {"timeoutMs", 20000}, {"successExitCodes", QJsonArray{0}}};
        QJsonObject project{{"id", "fixture"}, {"tasks", QJsonArray{QJsonObject{{"id", "prepare"}, {"kind", "task"}, {"serviceCommand", command}}}}};
        client.preflight(project, cancel);
        cancel.requested.store(true);
        QVERIFY_THROWS_EXCEPTION(Cancelled, client.start(project, "cancelled-run", cancel));
        client.stop(); QVERIFY(client.empty());
        } catch (const std::exception &error) { QFAIL(error.what()); }
    }
};
QTEST_GUILESS_MAIN(RuntimeTests)
#include "RuntimeTests.moc"

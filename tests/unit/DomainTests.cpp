#include "domain/Configuration.h"
#include "domain/Runtime.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
#include "../TestCompatibility.h"

using namespace cst;
class DomainTests final : public QObject {
    Q_OBJECT
    QJsonObject schema_;
    QJsonObject example_;
    const ProjectPaths paths_{"C:\\Program Files\\CST", "C:\\ProgramData\\CST", "C:\\Windows"};
    ValidationIssues validate(const QJsonObject &document) const { return ConfigurationValidator(schema_, paths_).validate(document); }
    ValidationIssues validateRun(const QJsonObject &document) const { return ConfigurationValidator(schema_, paths_).validateForRun(document); }
    QJsonObject changedTask(const QString &key, const QJsonValue &value) const {
        auto document = example_;
        auto project = document["project"].toObject();
        auto tasks = project["tasks"].toArray();
        auto task = tasks[0].toObject();
        task[key] = value; tasks[0] = task; project["tasks"] = tasks; document["project"] = project;
        return document;
    }
private slots:
    void initTestCase() {
        QFile schema(QStringLiteral(CST_SOURCE_DIR "/config/cst-project.schema.json"));
        QFile example(QStringLiteral(CST_SOURCE_DIR "/config/cst-project.example.json"));
        QVERIFY(schema.open(QIODevice::ReadOnly)); QVERIFY(example.open(QIODevice::ReadOnly));
        schema_ = QJsonDocument::fromJson(schema.readAll()).object();
        example_ = QJsonDocument::fromJson(example.readAll()).object();
        QVERIFY(!schema_.isEmpty()); QVERIFY(!example_.isEmpty());
    }
    void exampleValid() {
        const auto issues = validate(example_);
        for (const auto &issue : issues) qWarning().noquote() << issue.path << issue.message;
        QVERIFY(issues.isEmpty());
    }
    void schemaMutations() {
        auto document = example_; document["unknown"] = true; QVERIFY(!validate(document).isEmpty());
        document = example_; document["schemaVersion"] = 2; QVERIFY(!validate(document).isEmpty());
        document = example_; document.remove("project"); QVERIFY(!validate(document).isEmpty());
        QVERIFY(!validate(changedTask("order", 1.5)).isEmpty());
        QVERIFY(!validate(changedTask("order", -1)).isEmpty());
        QVERIFY(!validate(changedTask("id", "X")).isEmpty());
        QVERIFY(!validate(changedTask("name", QString(81, 'x'))).isEmpty());
        auto task = example_["project"].toObject()["tasks"].toArray()[0].toObject();
        auto service = task["serviceCommand"].toObject();
        service["script"] = "echo invalid"; QVERIFY(!validate(changedTask("serviceCommand", service)).isEmpty());
        service.remove("script"); service["timeoutMs"] = 1; QVERIFY(!validate(changedTask("serviceCommand", service)).isEmpty());
        service["timeoutMs"] = 0; service["program"] = "npm.CMD"; QVERIFY(!validate(changedTask("serviceCommand", service)).isEmpty());
        service["program"] = "uv.exe"; service["successExitCodes"] = QJsonArray{0, 0}; QVERIFY(!validate(changedTask("serviceCommand", service)).isEmpty());
        QVERIFY(!validate(changedTask("workingDirectory", "{{UNKNOWN}}\\x")).isEmpty());
        QVERIFY(!validate(changedTask("workingDirectory", "relative\\x")).isEmpty());
        auto readiness = task["readiness"].toObject(); readiness["probes"] = QJsonArray{};
        QVERIFY(validate(changedTask("readiness", readiness)).isEmpty());
        QVERIFY(!validateRun(changedTask("readiness", readiness)).isEmpty());
        auto env = task["environment"].toObject(); env["variables"] = QJsonObject{{"export A", "x"}};
        QVERIFY(!validate(changedTask("environment", env)).isEmpty());
    }
    void crossConstraints() {
        auto document = example_;
        auto project = document["project"].toObject();
        auto tasks = project["tasks"].toArray(); auto task = tasks[1].toObject();
        task["order"] = 10; tasks[1] = task; project["tasks"] = tasks; document["project"] = project;
        QVERIFY(!validate(document).isEmpty());
        project = example_["project"].toObject();
        auto ports = project["requiredPorts"].toArray(); auto port = ports[0].toObject();
        port["ownerTaskId"] = "missing"; ports[0] = port; project["requiredPorts"] = ports; document["project"] = project;
        QVERIFY(!validate(document).isEmpty());
        project = example_["project"].toObject(); auto source = project["source"].toObject();
        for (const auto &path : {"C:\\Windows\\source", "C:\\ProgramData\\CST\\source", "C:\\Program Files\\CST\\source", "C:\\other\\..\\Windows\\source"}) {
            source["workingDirectory"] = path; project["source"] = source; document["project"] = project;
            QVERIFY(!validate(document).isEmpty());
        }
    }
    void quoting_data() {
        QTest::addColumn<QString>("input"); QTest::addColumn<QString>("expected");
        QTest::newRow("empty") << "" << "\"\"";
        QTest::newRow("space") << "hello world" << "\"hello world\"";
        QTest::newRow("quote") << "a\"b" << "\"a\\\"b\"";
        QTest::newRow("trailing") << "C:\\folder\\" << "\"C:\\folder\\\\\"";
        QTest::newRow("slash-quote") << "a\\\"b" << "\"a\\\\\\\"b\"";
    }
    void quoting() { QFETCH(QString, input); QFETCH(QString, expected); QCOMPARE(quoteWindowsArgument(input), expected); }
    void normalizedPathInputs() {
        QCOMPARE(normalizeWindowsPathInput("  C:/Program Files/My App  "), QString("C:\\Program Files\\My App"));
        QCOMPARE(normalizeWindowsPathInput("\"C:/Program Files/My App\""), QString("C:\\Program Files\\My App"));
        QCOMPARE(normalizeWindowsPathInput("C:/folder/../files"), QString("C:\\files"));
        QCOMPARE(normalizeWindowsPathInput("//server/share/folder name"), QString("\\\\server\\share\\folder name"));
        QCOMPARE(normalizeWindowsPathInput("{{PROJECT_DIR}}/backend"), QString("{{PROJECT_DIR}}\\backend"));
        QCOMPARE(normalizeWindowsPathInput("C:\\"), QString("C:\\"));
        QCOMPARE(normalizeWindowsPathInput("uv.exe"), QString("uv.exe"));
    }
    void normalizedProjectPaths() {
        auto document=example_;auto project=document["project"].toObject();auto source=project["source"].toObject();
        source["workingDirectory"]="C:/Program Files/My Project";source["gitExecutable"]="C:/Program Files/Git/cmd/git.exe";project["source"]=source;
        project["toolDirectories"]=QJsonArray{"C:/Program Files/nodejs"};
        auto tasks=project["tasks"].toArray();auto task=tasks[0].toObject();task["workingDirectory"]="{{PROJECT_DIR}}/backend";auto env=task["environment"].toObject();env["envFiles"]=QJsonArray{"{{DATA_DIR}}/env/backend.env"};task["environment"]=env;auto service=task["serviceCommand"].toObject();service["program"]="C:/Program Files/My App/app.exe";task["serviceCommand"]=service;tasks[0]=task;project["tasks"]=tasks;document["project"]=project;
        const auto normalized=normalizeProjectPaths(document);const auto np=normalized["project"].toObject();
        QCOMPARE(np["source"].toObject()["workingDirectory"].toString(),QString("C:\\Program Files\\My Project"));
        QCOMPARE(np["source"].toObject()["gitExecutable"].toString(),QString("C:\\Program Files\\Git\\cmd\\git.exe"));
        QCOMPARE(np["toolDirectories"].toArray()[0].toString(),QString("C:\\Program Files\\nodejs"));
        QCOMPARE(np["tasks"].toArray()[0].toObject()["workingDirectory"].toString(),QString("{{PROJECT_DIR}}\\backend"));
        QCOMPARE(np["tasks"].toArray()[0].toObject()["environment"].toObject()["envFiles"].toArray()[0].toString(),QString("{{DATA_DIR}}\\env\\backend.env"));
        QCOMPARE(np["tasks"].toArray()[0].toObject()["serviceCommand"].toObject()["program"].toString(),QString("C:\\Program Files\\My App\\app.exe"));
    }
    void pathsAndUrls() {
        QVERIFY(isWindowsAbsolutePath("C:\\folder")); QVERIFY(isWindowsAbsolutePath("\\\\server\\share\\folder"));
        for (const auto &path : {"C:folder", "\\folder", "relative", "\\\\?\\C:\\test", "C:\\NUL.txt", "C:\\folder.\\x", "C:\\folder:x"}) QVERIFY(!isWindowsAbsolutePath(path));
        QVERIFY(isWithinWindowsPath("c:/WINDOWS/a/../b", "C:\\Windows"));
        QVERIFY(!isWithinWindowsPath("C:\\WindowsOther", "C:\\Windows"));
        QVERIFY(isAllowedUrl("https://example.com")); QVERIFY(!isAllowedUrl("file:///x"));
        QVERIFY(!isAllowedUrl("https://user:secret@example.com/repo", true));
        QVERIFY(!isAllowedUrl("https://example.com/repo?token=x", true));
        QVERIFY(!isAllowedUrl("https://example.com/repo#main", true));
        QVERIFY(!isAllowedUrl("https:relative"));
        QCOMPARE(expandPlaceholders("{{PROJECT_DIR}}/a", {{"PROJECT_DIR", "C:/project"}}), "C:/project/a");
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, expandPlaceholders("{{UNKNOWN}}", {}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, expandPlaceholders("{{PROJECT_DIR}}", {{"PROJECT_DIR", "{{DATA_DIR}}"}}));
        QVERIFY(addressesConflict("::", "127.0.0.1")); QVERIFY(addressesConflict("0.0.0.0", "127.0.0.1"));
        QVERIFY(!addressesConflict("127.0.0.2", "127.0.0.1"));
    }
    void environment() {
        const auto parsed = parseEnv("\xef\xbb\xbf# comment\r\nPath='a b'\r\nA=\"${NO_EXPANSION}\\n\"\nEMPTY=\n");
        QCOMPARE(parsed.value("PATH"), "a b"); QCOMPARE(parsed.value("A"), "${NO_EXPANSION}\\n"); QCOMPARE(parsed.value("EMPTY"), "");
        for (const auto &bad : {QByteArray("export A=x"), QByteArray("A"), QByteArray("A='x"), QByteArray("A=\xff")}) QVERIFY_THROWS_EXCEPTION(std::invalid_argument, parseEnv(bad));
        const auto merged = mergeEnvironment(true, {{"Path", "system"}}, {{{"PATH", "first"}}, {{"path", "second"}}}, {{"PaTh", "configured"}}, {{"CST_PROJECT_ID", "injected"}});
        QCOMPARE(merged.value("PATH"), "configured"); QCOMPARE(merged.size(), 2);
        QCOMPARE(mergeEnvironment(false, {{"X", "system"}}, {}, {}, {}).size(), 0);
        const auto block = environmentBlock(merged);
        QVERIFY(block.endsWith(QString(2, QChar::Null))); QVERIFY(block.startsWith("CST_PROJECT_ID=injected"));
        QCOMPARE(environmentBlock({}), QString(2, QChar::Null));
    }
    void stateMachine() {
        using S = ProjectState; using E = RuntimeEvent;
        const QList<std::tuple<S, E, S>> legal{
            {S::Stopped,E::Start,S::Preflight},{S::Failed,E::Start,S::Preflight},
            {S::Preflight,E::ChecksPassed,S::ReclaimingPorts},{S::Preflight,E::CheckFailed,S::Failed},
            {S::Preflight,E::Stop,S::Stopping},{S::Preflight,E::Close,S::Stopping},
            {S::ReclaimingPorts,E::PortsFree,S::Starting},{S::ReclaimingPorts,E::ReclaimFailed,S::Failed},
            {S::ReclaimingPorts,E::Stop,S::Stopping},{S::ReclaimingPorts,E::Close,S::Stopping},
            {S::Starting,E::TasksReady,S::Running},{S::Starting,E::Stop,S::Stopping},{S::Starting,E::Close,S::Stopping},{S::Starting,E::TaskFailed,S::Stopping},
            {S::Running,E::Stop,S::Stopping},{S::Running,E::Close,S::Stopping},{S::Running,E::UnrecoverableCrash,S::Stopping},
            {S::Stopping,E::JobsEmpty,S::Stopped},{S::Stopped,E::Sync,S::Syncing},{S::Failed,E::Sync,S::Syncing},
            {S::Syncing,E::SyncSucceeded,S::Stopped},{S::Syncing,E::SyncFailed,S::Failed}};
        for (int s = 0; s <= int(S::Syncing); ++s) for (int e = 0; e <= int(E::SyncFailed); ++e) {
            std::optional<S> expected;
            for (const auto &[from, event, to] : legal) if (from == S(s) && event == E(e)) expected = to;
            QCOMPARE(transition(S(s), E(e)), expected);
        }
        QCOMPARE(transition(S::Stopping, E::JobsEmpty, true), std::optional<S>(S::Failed));
    }
    void restartWindow() {
        RestartBudget budget;
        for (qint64 delay : {1000,2000,4000,8000,15000}) QCOMPARE(budget.schedule(0), std::optional<qint64>(delay));
        QVERIFY(!budget.schedule(599999)); QCOMPARE(budget.schedule(600000), std::optional<qint64>(30000));
        budget.reset(); QCOMPARE(budget.attempts(), 0); QCOMPARE(budget.schedule(600001), std::optional<qint64>(1000));
    }
    void redaction() {
        const auto result = redactSecrets("https://user:secret@example.com/?token=abc password=\"two words\" Authorization: Bearer xyz\ncredential blob=bytes\nknown", {"known"});
        for (const auto &secret : {"user:secret", "abc", "two words", "xyz", "bytes", "known"}) QVERIFY(!result.contains(secret));
        QVERIFY(result.contains("example.com"));
    }
};
QTEST_GUILESS_MAIN(DomainTests)
#include "DomainTests.moc"

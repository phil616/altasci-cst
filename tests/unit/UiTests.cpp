#include "ui/MainWindow.h"
#include "infrastructure/common/SystemClock.h"
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace cst;
namespace {
class Urls final : public IUrlLauncher { public: QString opened; void open(const QString &url) override { opened=url; } };
class Runner final : public IProcessRunner {
public:
    std::shared_ptr<IManagedProcess> start(const ProcessSpec &,std::function<void(ProcessOutput)>)override{throw std::runtime_error("Unexpected process in UI test");}
    QString resolveExecutable(const QString &program,const QStringList &)const override{return program;}
    Environment inheritedEnvironment()const override{return {};}
};
class Ports final : public IPortManager {
public:
    QList<PortOwner> owners(const PortRequirement &)const override{return {};}
    void terminateTree(quint32,const PortRequirement &,const Cancellation &)override{throw std::runtime_error("Unexpected termination in UI test");}
    bool stopService(quint32,const Cancellation &)override{return false;}
    quint32 parentPid(quint32)const override{return 0;}
};
class Files final : public IFileTransaction {
public:
    void renameDirectory(const QString &,const QString &)override{throw std::runtime_error("Unexpected rename in UI test");}
    bool removeDirectory(const QString &)override{return false;}
};
class Credentials final : public ICredentialStore {
public:
    QMap<QString,Credential> values;
    void write(const QString &target,const Credential &value)override{values[target]=value;}
    std::optional<Credential> read(const QString &target)const override{return values.contains(target)?std::optional<Credential>(values.value(target)):std::nullopt;}
    void remove(const QString &target)override{values.remove(target);}
};
}
class UiTests final : public QObject {
    Q_OBJECT
private slots:
    void userActionsAndState(){
        Urls urls;UserPage page(urls);page.setFixedSize(1000,700);
        const auto example=readJson(CST_SOURCE_DIR "/config/cst-project.example.json");page.setProject(example);page.show();
        auto *main=page.findChild<QPushButton *>("mainAction");QVERIFY(main);QCOMPARE(main->text(),"一键启动");QCOMPARE(main->property("stateColor").toString(),"#2563EB");
        auto *frontend=page.findChild<QPushButton *>("open-frontend");auto *docs=page.findChild<QPushButton *>("open-docs");auto *feedback=page.findChild<QPushButton *>("feedback");
        QVERIFY(frontend&&docs&&feedback);QVERIFY(!frontend->isEnabled());QVERIFY(docs->isEnabled());QVERIFY(feedback->isEnabled());
        QTest::mouseClick(docs,Qt::LeftButton);QCOMPARE(urls.opened,"https://docs.example.com/customer-project");
        for(auto state:{ProjectState::Preflight,ProjectState::ReclaimingPorts,ProjectState::Starting,ProjectState::Running}){page.setState(state);QCOMPARE(main->text(),"一键停止");QCOMPARE(main->property("stateColor").toString(),"#C62828");QVERIFY(main->isEnabled());}
        QVERIFY(frontend->isEnabled());page.setState(ProjectState::Stopping);QCOMPARE(main->text(),"正在停止…");QVERIFY(!main->isEnabled());
        page.setState(ProjectState::Syncing);QCOMPARE(main->text(),"正在同步代码…");QVERIFY(!main->isEnabled());
        page.setState(ProjectState::Failed);QCOMPARE(main->text(),"一键启动");
        page.setActive(false);QVERIFY(!main->isDefault());page.setActive(true);QVERIFY(main->isDefault());
        QCOMPARE(page.width(),1000);QTRY_COMPARE(docs->geometry().top(),frontend->geometry().top());QTRY_VERIFY(feedback->geometry().top()>docs->geometry().top());
        page.setFixedSize(800,700);QCOMPARE(page.width(),800);QTRY_VERIFY(docs->geometry().top()>frontend->geometry().top());
        QVERIFY(main->height()>=main->fontMetrics().height()+12);
    }
    void commandModesExclusive(){
        const auto schema=readJson(CST_SOURCE_DIR "/config/cst-project.schema.json");
        const auto command=readJson(CST_SOURCE_DIR "/config/cst-project.example.json").value("project").toObject().value("tasks").toArray()[0].toObject().value("serviceCommand");
        SchemaEditor editor(schema,{{"$ref","#/$defs/command"}},command);editor.show();
        auto *mode=editor.findChild<SchemaEditor *>("mode")->findChild<QComboBox *>();QVERIFY(mode);
        QVERIFY(editor.value().toObject().contains("program"));QVERIFY(!editor.value().toObject().contains("script"));
        mode->setCurrentText("shell");QVERIFY(editor.value().toObject().contains("script"));QVERIFY(!editor.value().toObject().contains("program"));QVERIFY(!editor.value().toObject().contains("arguments"));
    }
    void navigationCancellationAndClose(){
        QTemporaryDir directory;const auto schema=readJson(CST_SOURCE_DIR "/config/cst-project.schema.json");
        const ProjectPaths paths{"C:/Program Files/CST","C:/ProgramData/CST","C:/Windows"};
        ProjectConfigService configuration(ConfigurationValidator(schema,paths));ProjectCatalogService catalog(directory.path(),configuration);
        const auto example=readJson(CST_SOURCE_DIR "/config/cst-project.example.json");catalog.importProject(example);
        Runner runner;Ports ports;Files files;Credentials credentials;SystemClock clock;QMutex mutex;
        ReadinessService readiness(clock);PortReclaimService reclaim(ports,clock);TaskSupervisor supervisor(runner,readiness,reclaim,clock,paths);
        SourceSyncService sync(runner,files,clock,directory.path(),"askpass",mutex);LogService logs(directory.path());DiagnosticExportService diagnostics(runner,ports,clock,logs,directory.path());
        ProjectRuntimeService runtime(configuration,catalog,supervisor,reclaim,sync);Urls urls;
        auto *user=new UserPage(urls);auto *admin=new AdminPage(runtime,configuration,catalog,credentials,ports,sync,diagnostics,logs,schema,paths);
        MainWindow window(runtime,user,admin,logs);window.show();runtime.initialize(directory.path());QTRY_VERIFY(!runtime.busy());
        QVERIFY(!runtime.currentProject().isEmpty());auto *main=user->findChild<QPushButton *>("mainAction");
        auto *adminNavigation=window.findChild<QPushButton *>("adminNavigation");QTest::mouseClick(adminNavigation,Qt::LeftButton);QVERIFY(admin->isVisible());QVERIFY(!main->isDefault());
        QTest::mouseClick(window.findChild<QPushButton *>("userNavigation"),Qt::LeftButton);QVERIFY(user->isVisible());QVERIFY(main->isDefault());
        runtime.start();QCOMPARE(main->text(),"一键停止");QCOMPARE(runtime.state(),ProjectState::Preflight);
        for(auto *editor:admin->findChildren<SchemaEditor *>())QVERIFY(!editor->isEnabled());
        QSignalSpy close(&runtime,&ProjectRuntimeService::closeReady);window.close();QVERIFY(window.isVisible());
        QTRY_VERIFY(!window.isVisible());QCOMPARE(close.count(),1);QVERIFY(supervisor.empty());
    }
};
QTEST_MAIN(UiTests)
#include "UiTests.moc"

#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/UiSupport.h"
#include <QDir>
#include <QTabWidget>
#include <QSpinBox>
#include <QCheckBox>
#include <QScrollArea>
#include <QScrollBar>
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
    void initTestCase(){applyTheme(*qobject_cast<QApplication *>(QCoreApplication::instance()));}
    void userActionsAndState(){
        Urls urls;UserPage page(urls);page.setFixedSize(1000,700);
        page.setProject({});page.show();
        auto *main=page.findChild<QPushButton *>("mainAction");QVERIFY(main);QVERIFY(!main->isVisible());
        auto *configure=page.findChild<QPushButton *>("configureAction");QVERIFY(configure);QVERIFY(configure->isVisible());
        QSignalSpy configureSpy(&page,&UserPage::configureRequested);QTest::mouseClick(configure,Qt::LeftButton);QCOMPARE(configureSpy.count(),1);
        const auto example=readJson(CST_SOURCE_DIR "/config/cst-project.example.json");page.setProject(example);QCoreApplication::processEvents();
        QVERIFY(main->isVisible());QVERIFY(!configure->isVisible());QCOMPARE(main->text(),"一键启动");QCOMPARE(main->property("stateColor").toString(),"#2563EB");
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
        mode->setCurrentText("shell");
        auto *script=editor.findChild<SchemaEditor *>("script")->findChild<QPlainTextEdit *>();QVERIFY(script);
        script->setPlainText("echo first\necho second");QCOMPARE(editor.value().toObject().value("script").toString(),QString("echo first\necho second"));QVERIFY(editor.value().toObject().contains("script"));QVERIFY(!editor.value().toObject().contains("program"));QVERIFY(!editor.value().toObject().contains("arguments"));
    }
    void taskTabsPreserveConfiguration(){
        const auto schema=readJson(CST_SOURCE_DIR "/config/cst-project.schema.json");
        const auto task=readJson(CST_SOURCE_DIR "/config/cst-project.example.json").value("project").toObject().value("tasks").toArray().first().toObject();
        SchemaEditor editor(schema,{{"$ref","#/$defs/task"}},task);editor.resize(650,500);editor.show();
        auto *tabs=editor.findChild<QTabWidget *>("taskEditorTabs");QVERIFY(tabs);QCOMPARE(tabs->count(),5);
        QCOMPARE(editor.value().toObject(),task);
        for(int index=0;index<tabs->count();++index){tabs->setCurrentIndex(index);QCoreApplication::processEvents();QCOMPARE(editor.value().toObject(),task);}
        auto *name=editor.findChild<SchemaEditor *>("name")->findChild<QLineEdit *>();QVERIFY(name);name->setText("修改任务名称");
        auto expected=task;expected["name"]="修改任务名称";QCOMPARE(editor.value().toObject(),expected);
        QVERIFY(!QPixmap(":/ui/down.png").isNull());QVERIFY(!QPixmap(":/ui/up.png").isNull());QVERIFY(!QPixmap(":/ui/check.png").isNull());
    }
    void logLineFormatting(){
        const auto formatted=formatLogLine(R"({"ts":"2026-09-17T12:00:00.000Z","level":"error","channel":"stderr","event":"process.stderr","message":"boom"})");
        QVERIFY(formatted.contains("boom"));QVERIFY(formatted.contains("ERROR"));QVERIFY(formatted.contains("[stderr]"));QVERIFY(!formatted.contains("\"message\""));
        QCOMPARE(formatLogLine("plain text"),QString("plain text"));
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
        MainWindow window(runtime,user,admin,logs);window.setFixedSize(1180,760);window.show();runtime.initialize(directory.path());QTRY_VERIFY(!runtime.busy());
        QVERIFY(!runtime.currentProject().isEmpty());auto *main=user->findChild<QPushButton *>("mainAction");
        auto *adminNavigation=window.findChild<QPushButton *>("adminNavigation");QTest::mouseClick(adminNavigation,Qt::LeftButton);QVERIFY(admin->isVisible());QVERIFY(!main->isDefault());
        QTest::mouseClick(window.findChild<QPushButton *>("userNavigation"),Qt::LeftButton);QVERIFY(user->isVisible());QVERIFY(main->isDefault());
        const auto capture=qEnvironmentVariable("CST_UI_CAPTURE_DIR");
        if(!capture.isEmpty()){
            QDir().mkpath(capture);
            QCoreApplication::processEvents();QVERIFY(window.grab().save(capture+"/user.png"));
            adminNavigation->click();
            auto *navigation=admin->findChild<QListWidget *>("adminSidebar");QVERIFY(navigation);
            for(int row=0;row<navigation->count();++row){
                navigation->setCurrentRow(row);QCoreApplication::processEvents();QVERIFY(window.grab().save(capture+"/admin-"+QString::number(row)+".png"));
                for(auto *scroll:admin->findChildren<QScrollArea *>())if(scroll->isVisible())scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                QCoreApplication::processEvents();QVERIFY(window.grab().save(capture+"/admin-"+QString::number(row)+"-bottom.png"));
                for(auto *scroll:admin->findChildren<QScrollArea *>())scroll->verticalScrollBar()->setValue(0);
            }
            navigation->setCurrentRow(1);
            auto *tabs=admin->findChild<QTabWidget *>("taskEditorTabs");QVERIFY(tabs);
            for(int index=0;index<tabs->count();++index){tabs->setCurrentIndex(index);QCoreApplication::processEvents();QVERIFY(window.grab().save(capture+"/task-"+QString::number(index)+".png"));}
            navigation->setCurrentRow(6);auto *combo=admin->findChild<QComboBox *>();QVERIFY(combo);
            navigation->setCurrentRow(4);admin->setEnabled(false);QCoreApplication::processEvents();QVERIFY(window.grab().save(capture+"/disabled.png"));admin->setEnabled(true);
            window.findChild<QPushButton *>("userNavigation")->click();user->setState(ProjectState::Failed);QCoreApplication::processEvents();QVERIFY(window.grab().save(capture+"/failed.png"));user->setState(ProjectState::Stopped);
            window.setFixedSize(1040,680);adminNavigation->click();navigation->setCurrentRow(1);QCoreApplication::processEvents();QVERIFY(window.grab().save(capture+"/compact-task.png"));window.setFixedSize(1180,760);
        }
        admin->setProject({});
        for(auto *editor:admin->findChildren<SchemaEditor *>())QVERIFY(!editor->isEnabled());
        for(auto *button:admin->findChildren<QPushButton *>())if(button->text()=="创建项目"||button->text()=="导入 JSON")QVERIFY(button->isEnabled());
        if(!capture.isEmpty()){
            adminNavigation->click();admin->findChild<QListWidget *>("adminSidebar")->setCurrentRow(7);QCoreApplication::processEvents();QVERIFY(window.grab().save(capture+"/empty.png"));
        }
        admin->setProject(example);adminNavigation->click();
        auto *projectName=admin->findChild<SchemaEditor *>("project.name")->findChild<QLineEdit *>();QVERIFY(projectName);projectName->setText("未保存的项目名称");
        QVERIFY(!main->isEnabled());
        auto buttons=admin->findChildren<QPushButton *>();QPushButton *discard=nullptr;QPushButton *save=nullptr;
        for(auto *button:buttons){if(button->text()=="撤销修改")discard=button;if(button->text()=="保存配置")save=button;}
        QVERIFY(discard&&save);QVERIFY(discard->isEnabled());QVERIFY(save->isEnabled());discard->click();QVERIFY(!save->isEnabled());QVERIFY(main->isEnabled());
        window.findChild<QPushButton *>("userNavigation")->click();
        runtime.start();QCOMPARE(main->text(),"一键停止");QCOMPARE(runtime.state(),ProjectState::Preflight);
        for(auto *editor:admin->findChildren<SchemaEditor *>())QVERIFY(!editor->isEnabled());
        QSignalSpy close(&runtime,&ProjectRuntimeService::closeReady);window.close();QVERIFY(window.isVisible());
        QTRY_VERIFY(!window.isVisible());QCOMPARE(close.count(),1);QVERIFY(supervisor.empty());
    }
};
QTEST_MAIN(UiTests)
#include "UiTests.moc"

#include "application/DiagnosticExportService.h"
#include "application/ProjectRuntimeService.h"
#include "infrastructure/common/QtUrlLauncher.h"
#include "infrastructure/common/SystemClock.h"
#include "infrastructure/windows/WindowsPlatform.h"
#include "infrastructure/windows/WindowsSession.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QMessageBox>
#include <QTimer>

int main(int argc,char **argv){
    QApplication application(argc,argv);
    cst::applyTheme(application);
    application.setApplicationName("CST");application.setApplicationVersion("1.0.0");application.setOrganizationName("CST");
    try{
        cst::WindowsPrivilegeService privilege;privilege.requireElevationAndDebugPrivilege();
        cst::WindowsSession::requireSupportedWindows();
        cst::WindowsSession session;if(!session.primary())return 0;
        const auto paths=cst::WindowsSession::paths();
        const auto schema=cst::readJson(":/config/cst-project.schema.json");
        cst::ProjectConfigService configuration(cst::ConfigurationValidator(schema,paths));
        cst::ProjectCatalogService catalog(paths.storageDirectory,configuration);
        cst::WindowsCredentialStore credentials;cst::WindowsPortManager ports;cst::WindowsFileTransaction files;cst::SystemClock clock;
        const auto install=QCoreApplication::applicationDirPath();
        cst::WindowsProcessRunner runner(install+"/cst-signal-helper.exe");
        cst::ReadinessService readiness(clock);cst::PortReclaimService reclaim(ports,clock);cst::TaskSupervisor supervisor(runner,readiness,reclaim,clock,paths);
        QMutex operationMutex;cst::SourceSyncService sync(runner,files,clock,paths.storageDirectory,install+"/cst-git-askpass.exe",operationMutex);
        cst::LogService logs(paths.storageDirectory);cst::DiagnosticExportService diagnostics(runner,ports,clock,logs,paths.storageDirectory);
        cst::ProjectRuntimeService runtime(configuration,catalog,supervisor,reclaim,sync);
        cst::QtUrlLauncher urls;
        auto *user=new cst::UserPage(urls);auto *admin=new cst::AdminPage(runtime,configuration,catalog,credentials,ports,sync,diagnostics,logs,schema,paths);
        cst::MainWindow window(runtime,user,admin,logs);
        QObject::connect(&session,&cst::WindowsSession::activateRequested,&window,&cst::MainWindow::activate);
        QObject::connect(&session,&cst::WindowsSession::endSessionRequested,&runtime,&cst::ProjectRuntimeService::close);
        QObject::connect(&runtime,&cst::ProjectRuntimeService::projectChanged,&logs,[&](const QJsonObject &document){
            const auto target=document.value("project").toObject().value("source").toObject().value("credentialTarget").toString();
            if(target.isEmpty())return;
            try{if(const auto value=credentials.read(target))logs.addSecret(value->password);}
            catch(const std::exception &e){QMessageBox::critical(&window,"无法读取凭据状态",QString::fromUtf8(e.what()));}
        });
        QObject::connect(&runtime,&cst::ProjectRuntimeService::processOutput,&logs,[&](const QString &task,const cst::ProcessOutput &output){
            const auto id=runtime.currentProject().value("project").toObject().value("id").toString();
            logs.write(id,task,runtime.operationId(),"process."+output.channel,output.text,"info",output.channel,output.decodeError,output.timestamp);
        });
        QObject::connect(&runtime,&cst::ProjectRuntimeService::stateChanged,&logs,[&](cst::ProjectState state){
            const auto id=runtime.currentProject().value("project").toObject().value("id").toString();
            if(id.isEmpty())return;
            logs.write(id,{},runtime.operationId(),"project.state",cst::stateName(state));
            try{cst::saveJson(paths.storageDirectory+"/state/"+id+"/runtime.json",{{"state",cst::stateName(state)},{"operationId",runtime.operationId()},{"tasks",runtime.taskSnapshot()}});}
            catch(const std::exception &e){QMessageBox::critical(&window,"运行状态保存失败",QString::fromUtf8(e.what()));}
        });
        QObject::connect(&runtime,&cst::ProjectRuntimeService::operationFinished,&logs,[&](const QString &result,bool success){
            const auto id=runtime.currentProject().value("project").toObject().value("id").toString();
            if(!id.isEmpty())logs.write(id,{},runtime.operationId(),"operation.finished",result,success?"info":"error");
        });
        window.show();QTimer::singleShot(0,&runtime,[&]{runtime.initialize(paths.storageDirectory);});
        return application.exec();
    }catch(const std::exception &error){QMessageBox::critical(nullptr,"CST 无法启动",QString::fromUtf8(error.what()));return 1;}
}

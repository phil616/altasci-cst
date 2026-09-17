#include "application/SourceSyncService.h"
#include "infrastructure/common/SystemClock.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include "../TestCompatibility.h"
#include <QUuid>
#ifdef Q_OS_WIN
#include "infrastructure/windows/WindowsPlatform.h"
#endif

using namespace cst;
namespace {
#ifndef Q_OS_WIN
// A test-only adapter lets the same deployment algorithm exercise real local Git
// repositories on Linux. The Windows tests use the production Job runner below.
class GitProcess final : public IManagedProcess {
public:
    mutable QProcess process;
    std::function<void(ProcessOutput)> output;
    mutable QByteArray stdoutBytes,stderrBytes;
    void drain()const{
        const auto emitLines=[&](QByteArray &pending,const QByteArray &bytes,const QString &channel){
            pending+=bytes;qsizetype newline=0;
            while((newline=pending.indexOf('\n'))>=0){if(output)output({channel,QString::fromUtf8(pending.left(newline)),false,QDateTime::currentDateTimeUtc()});pending.remove(0,newline+1);}
            if(process.state()==QProcess::NotRunning&&!pending.isEmpty()){if(output)output({channel,QString::fromUtf8(pending),false,QDateTime::currentDateTimeUtc()});pending.clear();}
        };
        emitLines(stdoutBytes,process.readAllStandardOutput(),"stdout");emitLines(stderrBytes,process.readAllStandardError(),"stderr");
    }
    quint32 rootPid()const override{return quint32(process.processId());}
    bool rootRunning()const override{if(process.state()!=QProcess::NotRunning)process.waitForFinished(1);drain();return process.state()!=QProcess::NotRunning;}
    bool empty()const override{return !rootRunning();}
    QList<quint32> processIds()const override{return rootRunning()?QList<quint32>{rootPid()}:QList<quint32>{};}
    std::optional<ProcessResult> result()const override{if(rootRunning())return std::nullopt;return ProcessResult{process.exitCode(),process.exitStatus()==QProcess::CrashExit};}
    void stop(int grace)override{process.terminate();if(!process.waitForFinished(grace))forceStop();drain();}
    void forceStop()override{process.kill();process.waitForFinished(5000);drain();}
};
class GitRunner final : public IProcessRunner {
public:
    std::shared_ptr<IManagedProcess> start(const ProcessSpec &spec,std::function<void(ProcessOutput)> output)override{
        auto process=std::make_shared<GitProcess>();process->output=std::move(output);QProcessEnvironment environment;
        for(auto it=spec.environment.begin();it!=spec.environment.end();++it)environment.insert(it.key(),it.value());
        process->process.setProcessEnvironment(environment);process->process.setWorkingDirectory(spec.workingDirectory);process->process.start(spec.program,spec.arguments);
        if(!process->process.waitForStarted(5000))throw std::runtime_error("Git fixture process failed to start");return process;
    }
    QString resolveExecutable(const QString &program,const QStringList &)const override{return program;}
    Environment inheritedEnvironment()const override{Environment result;const auto system=QProcessEnvironment::systemEnvironment();for(const auto &key:system.keys())result[key]=system.value(key);result["GIT_ALLOW_PROTOCOL"]="file";return result;}
};
#endif
class Exchange final : public IFileTransaction {
public:
    int moves=0;bool failInstall=false;
    void renameDirectory(const QString &from,const QString &to)override{
        if(++moves==2&&failInstall)throw std::runtime_error("Injected second-rename failure");
#ifdef Q_OS_WIN
        WindowsFileTransaction().renameDirectory(from,to);
#else
        if(!QDir().rename(from,to))throw std::runtime_error("rename failed");
#endif
    }
    bool removeDirectory(const QString &path)override{return QDir(path).removeRecursively();}
};
void write(const QString &path,const QByteArray &bytes){QFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())throw std::runtime_error("fixture write failed");}
}
class GitSyncTests final : public QObject {
    Q_OBJECT
    QString git_;
    QString run(const QString &directory,const QStringList &arguments){
        QProcess process;process.setWorkingDirectory(directory);auto environment=QProcessEnvironment::systemEnvironment();environment.insert("GIT_ALLOW_PROTOCOL","file");process.setProcessEnvironment(environment);process.start(git_,arguments);
        if(!process.waitForFinished(30000)||process.exitCode()!=0)throw std::runtime_error(process.readAllStandardError().constData());return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }
    void initializeRepo(const QString &path){QDir().mkpath(path);run(path,{"init","--initial-branch=main"});run(path,{"config","user.email","test@example.invalid"});run(path,{"config","user.name","CST Integration"});}
private slots:
    void initTestCase(){
        git_=qEnvironmentVariable("CST_TEST_GIT");if(git_.isEmpty())git_=QStandardPaths::findExecutable("git");
        QVERIFY2(!git_.isEmpty(),"Set CST_TEST_GIT to Git 2.55.0");
        const auto version=run(QDir::tempPath(),{"--version"});QVERIFY2(version.startsWith("git version 2.55.0"),qPrintable("Integration baseline requires Git 2.55.0; found "+version));
    }
    void replaceAndRollback_data(){QTest::addColumn<bool>("fail");QTest::newRow("remote-authority")<<false;QTest::newRow("rollback")<<true;}
    void replaceAndRollback(){
        QFETCH(bool,fail);QTemporaryDir directory;QVERIFY(directory.isValid());
        const auto source=directory.filePath("source"),sub=directory.filePath("sub"),remote=directory.filePath("remote.git"),target=directory.filePath("target");
        initializeRepo(sub);write(sub+"/module.txt","module\n");run(sub,{"add","."});run(sub,{"commit","-m","module"});
        initializeRepo(source);write(source+"/tracked.txt","first\n");write(source+"/.gitignore","ignored.txt\n");run(source,{"add","."});run(source,{"commit","-m","first"});
        run(source,{"submodule","add",sub,"module"});write(source+"/tracked.txt","second\n");run(source,{"add","."});run(source,{"commit","-m","second"});
        const auto head=run(source,{"rev-parse","HEAD"});run(directory.path(),{"clone","--bare",source,remote});run(directory.path(),{"clone",remote,target});
        write(target+"/tracked.txt","local edit\n");write(target+"/untracked.txt","untracked\n");write(target+"/ignored.txt","ignored\n");initializeRepo(target+"/nested");
#ifdef Q_OS_WIN
        qputenv("GIT_ALLOW_PROTOCOL","file");
        WindowsProcessRunner runner(QCoreApplication::applicationDirPath()+"/cst-signal-helper.exe");
#else
        GitRunner runner;
#endif
        Exchange files;files.failInstall=fail;SystemClock clock;QMutex mutex;Cancellation cancel;
        SourceSyncService sync(runner,files,clock,directory.filePath("storage"),QCoreApplication::applicationDirPath()+"/cst-git-askpass.exe",mutex);
        const SyncRequest request{QUuid::createUuid().toString(QUuid::WithoutBraces),remote,"main",target,git_,"CST/git/test/local","integration"};
        if(fail){QVERIFY_THROWS_EXCEPTION(std::runtime_error, sync.synchronize(request,cancel,[]{return true;}));QFile old(target+"/tracked.txt");QVERIFY(old.open(QIODevice::ReadOnly));QCOMPARE(old.readAll(),"local edit\n");QVERIFY(QFile::exists(target+"/untracked.txt"));}
        else{QCOMPARE(sync.synchronize(request,cancel,[]{return true;}),head);QCOMPARE(run(target,{"rev-parse","HEAD"}),head);QVERIFY(run(target,{"status","--porcelain=v1","--untracked-files=all"}).isEmpty());QVERIFY(!QFile::exists(target+"/ignored.txt"));QVERIFY(!QFile::exists(target+"/untracked.txt"));QVERIFY(!QFileInfo::exists(target+"/nested"));QVERIFY(QFile::exists(target+"/module/module.txt"));}
    }
};
QTEST_GUILESS_MAIN(GitSyncTests)
#include "GitSyncTests.moc"

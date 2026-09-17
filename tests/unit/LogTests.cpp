#include "application/LogService.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace cst;
class LogTests final : public QObject {
    Q_OBJECT
private slots:
    void jsonLinesAndSecrets(){
        QTemporaryDir directory;LogService service(directory.path());QSignalSpy failure(&service,&LogService::failed);QSignalSpy lines(&service,&LogService::lineWritten);
        service.addSecret("secret-value");service.write("project","task","operation","process.stderr","password=x secret-value","error","stderr",true);service.flush();
        QTRY_COMPARE(lines.size(),1);QCOMPARE(failure.size(),0);
        QFile file(directory.filePath("logs/project/tasks/task.log"));QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();QVERIFY(!bytes.contains("secret-value"));QVERIFY(!bytes.contains("password=x"));
        QJsonParseError error;const auto record=QJsonDocument::fromJson(bytes,&error).object();QCOMPARE(error.error,QJsonParseError::NoError);
        QCOMPARE(record.value("operationId").toString(),"operation");QCOMPARE(record.value("channel").toString(),"stderr");QVERIFY(record.value("decodeError").toBool());
        const auto sanitized=LogService::redactJson(QJsonObject{{"variables",QJsonObject{{"API_TOKEN","raw-secret"},{"NAME","kept"}}}}).toObject();
        QCOMPARE(sanitized.value("variables").toObject().value("API_TOKEN").toString(),"[REDACTED]");QCOMPARE(sanitized.value("variables").toObject().value("NAME").toString(),"kept");
    }
    void rotationLimits(){
        QTemporaryDir directory;LogService service(directory.path());QSignalSpy failure(&service,&LogService::failed);
        const auto main=directory.filePath("logs/project/cst.log"),task=directory.filePath("logs/project/tasks/task.log");
        QDir().mkpath(QFileInfo(task).absolutePath());
        for(int i=0;i<7;++i){
            for(const auto &path:{main,task}){QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.resize(path==main?10*1024*1024:20*1024*1024));}
            service.write("project","task","operation","rotation",QString::number(i));service.flush();
        }
        QCoreApplication::processEvents();QCOMPARE(failure.size(),0);
        for(const auto &path:{main,task}){QVERIFY(QFile::exists(path+".4"));QVERIFY(!QFile::exists(path+".5"));QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("message").toString(),"6");}
    }
};
QTEST_GUILESS_MAIN(LogTests)
#include "LogTests.moc"

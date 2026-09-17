#include "infrastructure/windows/WindowsPlatform.h"
#include "infrastructure/common/SystemClock.h"
#include "application/PortReclaimService.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTcpServer>
#include <QUdpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace cst;
class WindowsProcessTests final : public QObject {
    Q_OBJECT
    QString fixture_;
    QString helper_;
    quint16 available(const QString &protocol,const QString &address){
        if(protocol=="tcp"){QTcpServer server;if(!server.listen(QHostAddress(address),0))throw std::runtime_error("No available TCP fixture port");return server.serverPort();}
        QUdpSocket socket;if(!socket.bind(QHostAddress(address),0,QUdpSocket::DontShareAddress))throw std::runtime_error("No available UDP fixture port");return socket.localPort();
    }
private slots:
    void initTestCase(){
        WindowsPrivilegeService privilege;
        try{privilege.requireElevationAndDebugPrivilege();}catch(const std::exception &e){QFAIL(e.what());}
        fixture_=QCoreApplication::applicationDirPath()+"/cst-test-port-owner.exe";
        helper_=QCoreApplication::applicationDirPath()+"/cst-signal-helper.exe";
        QVERIFY(QFile::exists(fixture_));QVERIFY(QFile::exists(helper_));
    }
    void jobCloseKillsDescendants_data(){
        QTest::addColumn<QString>("protocol");QTest::addColumn<QString>("address");
        QTest::newRow("tcp4")<<"tcp"<<"127.0.0.1";QTest::newRow("tcp6")<<"tcp"<<"::1";
        QTest::newRow("udp4")<<"udp"<<"127.0.0.1";QTest::newRow("udp6")<<"udp"<<"::1";
    }
    void jobCloseKillsDescendants(){
        QFETCH(QString,protocol);QFETCH(QString,address);
        const auto port=available(protocol,address);WindowsPortManager ports;WindowsProcessRunner runner(helper_);
        const PortRequirement required{protocol,address,port,"fixture"};
        auto job=runner.start({fixture_,{"--port",QString::number(port),"--address",address,"--protocol",protocol,"--depth","2","--ignore-break"},QDir::tempPath(),runner.inheritedEnvironment(),{},"test","fixture","job-close"},{});
        QTRY_VERIFY_WITH_TIMEOUT(!ports.owners(required).isEmpty(),10000);
        QVERIFY(job->processIds().size()>=3);
        for(const auto &owner:ports.owners(required))QVERIFY(job->processIds().contains(owner.pid));
        job.reset();QTRY_VERIFY_WITH_TIMEOUT(ports.owners(required).isEmpty(),10000);
    }
    void gracefulThenForcedStop(){
        const auto port=available("tcp","127.0.0.1");WindowsPortManager ports;WindowsProcessRunner runner(helper_);
        const PortRequirement required{"tcp","127.0.0.1",port,"fixture"};
        auto job=runner.start({fixture_,{"--port",QString::number(port),"--depth","2","--ignore-break"},QDir::tempPath(),runner.inheritedEnvironment(),{},"test","fixture","stop"},{});
        QTRY_VERIFY_WITH_TIMEOUT(!ports.owners(required).isEmpty(),10000);
        job->stop(100);QVERIFY(job->empty());QVERIFY(ports.owners(required).isEmpty());job->stop(100);
    }
    void reclaimRespawningAncestor(){
        const auto port=available("tcp","127.0.0.1");WindowsPortManager ports;SystemClock clock;Cancellation cancel;PortReclaimService reclaim(ports,clock);
        const PortRequirement required{"tcp","127.0.0.1",port,"fixture"};
        QProcess parent;parent.start(fixture_,{"--port",QString::number(port),"--depth","2","--respawn","--ignore-break"});QVERIFY(parent.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(!ports.owners(required).isEmpty(),10000);
        try{reclaim.reclaim({required},15000,8,{},cancel);}catch(const std::exception &e){parent.kill();parent.waitForFinished();QFAIL(e.what());}
        QTRY_VERIFY_WITH_TIMEOUT(ports.owners(required).isEmpty(),5000);
        QVERIFY(parent.waitForFinished(5000)||parent.state()==QProcess::NotRunning);
    }
    void protectedBoundaryFails(){
        WindowsPortManager ports;Cancellation cancel;
        try{ports.terminateTree(4,{"tcp","127.0.0.1",1,"fixture"},cancel);QFAIL("Protected PID was accepted");}
        catch(const std::exception &e){const auto message=QString::fromUtf8(e.what());QVERIFY(message.contains("PID=4"));QVERIFY(message.contains("Win32"));QVERIFY(message.contains("端口"));}
    }
};
QTEST_GUILESS_MAIN(WindowsProcessTests)
#include "WindowsProcessTests.moc"

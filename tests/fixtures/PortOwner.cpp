#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

int main(int argc,char **argv){
    QCoreApplication application(argc,argv);QCommandLineParser parser;parser.addHelpOption();
    parser.addOptions({{{"p","port"},"监听端口","port","0"},{{"a","address"},"绑定地址","address","127.0.0.1"},
        {{"protocol"},"tcp 或 udp","protocol","tcp"},{{"depth"},"生成的子进程层数","depth","0"},
        {{"respawn"},"子进程退出后重生"},{{"ignore-break"},"忽略控制台停止信号"},{{"pid-file"},"追加进程 ID 的文件","path"}});
    parser.process(application);
    const auto port=parser.value("port").toUShort();const auto address=QHostAddress(parser.value("address"));const auto protocol=parser.value("protocol");
    if(address.isNull()||(protocol!="tcp"&&protocol!="udp"))return 2;
#ifdef Q_OS_WIN
    if(parser.isSet("ignore-break"))SetConsoleCtrlHandler([](DWORD)->BOOL{return TRUE;},TRUE);
#endif
    if(parser.isSet("pid-file")){QFile file(parser.value("pid-file"));if(!file.open(QIODevice::WriteOnly|QIODevice::Append))return 3;file.write(QByteArray::number(application.applicationPid())+'\n');file.flush();}
    const auto depth=parser.value("depth").toInt();
    QProcess child;
    if(depth>0){
        QStringList arguments{"--port",QString::number(port),"--address",address.toString(),"--protocol",protocol,"--depth",QString::number(depth-1)};
        if(parser.isSet("respawn"))arguments.append("--respawn");if(parser.isSet("ignore-break"))arguments.append("--ignore-break");
        if(parser.isSet("pid-file"))arguments.append({"--pid-file",parser.value("pid-file")});
        child.setProcessChannelMode(QProcess::ForwardedChannels);
        const auto launch=[&child,arguments]{child.start(QCoreApplication::applicationFilePath(),arguments);};
        QObject::connect(&child,&QProcess::finished,&application,[&application,&parser,launch](int code,QProcess::ExitStatus){if(parser.isSet("respawn"))QTimer::singleShot(50,&application,launch);else application.exit(code);});
        QObject::connect(&child,&QProcess::errorOccurred,&application,[&application](QProcess::ProcessError error){if(error==QProcess::FailedToStart)application.exit(4);});
        launch();return application.exec();
    }
    QTcpServer tcp;QUdpSocket udp;
    if(protocol=="tcp"){
        if(!tcp.listen(address,port))return 5;
        QObject::connect(&tcp,&QTcpServer::newConnection,&application,[&tcp]{while(auto *socket=tcp.nextPendingConnection()){
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket]{socket->readAll();socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK");socket->disconnectFromHost();});
        }});
    }else if(!udp.bind(address,port,QUdpSocket::DontShareAddress))return 6;
    const auto ready=QJsonDocument(QJsonObject{{"pid",application.applicationPid()},{"port",protocol=="tcp"?tcp.serverPort():udp.localPort()},{"protocol",protocol},{"address",address.toString()}}).toJson(QJsonDocument::Compact);
    QFile output;if(!output.open(stdout,QIODevice::WriteOnly))return 7;output.write(ready+'\n');output.flush();
    return application.exec();
}

#include "WindowsSession.h"
#include "WindowsPlatform.h"
#include "Win32Support.h"
#include <shlobj.h>
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QOperatingSystemVersion>
#include <QSysInfo>

namespace cst {
class WindowsSession::Implementation {
public:
    win::Handle mutex;
    QLocalServer server;
    bool primary = false;
};
WindowsSession::WindowsSession(QObject *parent):QObject(parent),implementation_(std::make_unique<Implementation>()){
    const auto name="CST-"+currentUserSid();const auto mutexName="Local\\"+name;
    implementation_->mutex.reset(CreateMutexW(nullptr,FALSE,win::wide(mutexName)));
    if(!implementation_->mutex)win::fail("创建 CST 单实例互斥体");
    if(GetLastError()==ERROR_ALREADY_EXISTS){
        QLocalSocket socket;socket.connectToServer(name);
        if(socket.waitForConnected(1500)){socket.write("activate\n");socket.waitForBytesWritten(1000);socket.disconnectFromServer();}
        return;
    }
    implementation_->primary=true;
    QLocalServer::removeServer(name);implementation_->server.setSocketOptions(QLocalServer::UserAccessOption);
    if(!implementation_->server.listen(name))throw std::runtime_error(implementation_->server.errorString().toUtf8().constData());
    connect(&implementation_->server,&QLocalServer::newConnection,this,[this]{
        while(auto *socket=implementation_->server.nextPendingConnection()){
            connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
            auto receive=[this,socket]{if(!socket->canReadLine())return;if(socket->readLine(64).trimmed()=="activate")emit activateRequested();socket->disconnectFromServer();};
            connect(socket,&QLocalSocket::readyRead,this,receive);receive();
        }
    });
    QCoreApplication::instance()->installNativeEventFilter(this);
}
WindowsSession::~WindowsSession(){if(primary())QCoreApplication::instance()->removeNativeEventFilter(this);}
bool WindowsSession::primary()const{return implementation_->primary;}
bool WindowsSession::nativeEventFilter(const QByteArray &,void *message,qintptr *result){
    const auto *event=static_cast<MSG *>(message);
    if(event->message==WM_QUERYENDSESSION){emit endSessionRequested();*result=TRUE;return true;}
    if(event->message==WM_ENDSESSION&&event->wParam){emit endSessionRequested();}
    return false;
}
ProjectPaths WindowsSession::paths(){
    const auto folder=[](REFKNOWNFOLDERID id){wchar_t *value=nullptr;const auto result=SHGetKnownFolderPath(id,KF_FLAG_DEFAULT,nullptr,&value);if(FAILED(result))win::fail("SHGetKnownFolderPath",static_cast<DWORD>(result));const auto path=QString::fromWCharArray(value);CoTaskMemFree(value);return path;};
    wchar_t windows[MAX_PATH+1]{};const auto length=GetWindowsDirectoryW(windows,MAX_PATH+1);if(!length||length>MAX_PATH)win::fail("GetWindowsDirectory");
    return {folder(FOLDERID_ProgramFiles)+"\\CST",folder(FOLDERID_ProgramData)+"\\CST",QString::fromWCharArray(windows)};
}
void WindowsSession::requireSupportedWindows(){
    const auto version=QOperatingSystemVersion::current();
    if(version.type()!=QOperatingSystemVersion::Windows||version.majorVersion()<10||
       (version.majorVersion()==10&&version.microVersion()<17763)||QSysInfo::currentCpuArchitecture()!="x86_64")
        throw std::runtime_error("CST 需要 Windows 10 1809 或更新版本的 x64 系统");
}
}

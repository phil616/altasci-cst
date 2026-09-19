#include "application/RuntimeClient.h"
#include "infrastructure/windows/WindowsPlatform.h"
#include "infrastructure/windows/WindowsSession.h"
#include "infrastructure/windows/Win32Support.h"
#include "infrastructure/common/SystemClock.h"
#include <QCoreApplication>

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (argc != 3) return 2;
    const auto args = application.arguments(); const auto token = qEnvironmentVariable("CST_HOST_TOKEN");
    qunsetenv("CST_HOST_TOKEN");
    if (token.isEmpty()) return 3;
    cst::win::Handle owner(OpenProcess(SYNCHRONIZE, FALSE, args[2].toUInt()));
    if (!owner) return 4;
    try {
        cst::WindowsProcessRunner runner(QCoreApplication::applicationDirPath() + "/cst-signal-helper.exe");
        cst::SystemClock clock; const auto paths = cst::WindowsSession::paths();
        cst::TaskSupervisor supervisor(runner, clock, paths);
        return cst::serveRuntime(supervisor, paths, args[1], token, [&] { return WaitForSingleObject(owner.get(), 0) == WAIT_TIMEOUT; });
    } catch (...) { return 5; }
}
